#include "assist/panel_assist.h"

#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "ha/ha_client.h"
#include "radio/panel_radio.h"
#include "audio/panel_audio.h"

static const char *TAG = "assist";

#define ASSIST_RATE 16000
#define ASSIST_FRAME_SAMPLES 320                /* 20 ms */
#define ASSIST_PREBUF_SAMPLES (ASSIST_RATE * 3) /* audio recorded before HA is ready */
#define ASSIST_HANDLER_WAIT_MS 4000
#define ASSIST_RESULT_WAIT_MS 30000

/* Requests from the UI. */
#define EV_START BIT0
#define EV_STOP BIT1
#define EV_CANCEL BIT2
/* Pipeline events from the websocket task. */
#define EV_HANDLER BIT3 /* run-start: binary handler id known */
#define EV_STT_DONE BIT4 /* VAD detected the end of speech / STT finished */
#define EV_TTS BIT5     /* tts-end: reply URL available */
#define EV_RUN_END BIT6 /* run-end or error */

typedef struct {
    bool enabled;
    char pipeline_id[RUNTIME_ASSIST_PIPELINE_ID_MAX_LEN];
    bool reply_audio;
    int mic_gain;
    int max_listen_sec;
} assist_cfg_t;

static assist_cfg_t s_cfg;
static SemaphoreHandle_t s_lock;
static EventGroupHandle_t s_events;
static TaskHandle_t s_task;
static panel_assist_status_t s_status;
static volatile bool s_busy;
/* Filled by the websocket callback, read by the assist task. */
static volatile int s_handler_id = -1;
static char s_tts_url[256];
static int64_t s_run_start_us;

static void assist_set_state(panel_assist_state_t state)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_status.state = state;
    s_status.seq++;
    xSemaphoreGive(s_lock);
}

static void assist_set_text(char *dst, size_t dst_len, const char *text)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    snprintf(dst, dst_len, "%s", text != NULL ? text : "");
    s_status.seq++;
    xSemaphoreGive(s_lock);
}

static const char *assist_json_str(cJSON *obj, const char *const *path, size_t depth)
{
    for (size_t i = 0; i < depth && obj != NULL; i++) {
        obj = cJSON_GetObjectItemCaseSensitive(obj, path[i]);
    }
    return (cJSON_IsString(obj) && obj->valuestring != NULL) ? obj->valuestring : NULL;
}

/* Runs in the websocket receive task: only copy data and signal the task. */
static void assist_event_cb(cJSON *event, void *user)
{
    (void)user;
    const char *type = assist_json_str(event, (const char *const[]){"type"}, 1);
    cJSON *data = cJSON_GetObjectItemCaseSensitive(event, "data");
    if (type == NULL) {
        return;
    }
    ESP_LOGI(TAG, "event %s (+%d ms)", type, (int)((esp_timer_get_time() - s_run_start_us) / 1000));

    if (strcmp(type, "run-start") == 0) {
        cJSON *runner = cJSON_GetObjectItemCaseSensitive(data, "runner_data");
        cJSON *hid = cJSON_GetObjectItemCaseSensitive(runner, "stt_binary_handler_id");
        if (cJSON_IsNumber(hid)) {
            s_handler_id = (int)hid->valuedouble;
            xEventGroupSetBits(s_events, EV_HANDLER);
        }
    } else if (strcmp(type, "stt-vad-end") == 0) {
        xEventGroupSetBits(s_events, EV_STT_DONE);
    } else if (strcmp(type, "stt-end") == 0) {
        const char *text = assist_json_str(data, (const char *const[]){"stt_output", "text"}, 2);
        assist_set_text(s_status.heard, sizeof(s_status.heard), text);
        xEventGroupSetBits(s_events, EV_STT_DONE);
    } else if (strcmp(type, "intent-end") == 0) {
        const char *speech = assist_json_str(
            data, (const char *const[]){"intent_output", "response", "speech", "plain", "speech"}, 5);
        assist_set_text(s_status.reply, sizeof(s_status.reply), speech);
    } else if (strcmp(type, "tts-end") == 0) {
        const char *url = assist_json_str(data, (const char *const[]){"tts_output", "url"}, 2);
        if (url != NULL && url[0] != '\0') {
            if (strncmp(url, "http", 4) == 0) {
                snprintf(s_tts_url, sizeof(s_tts_url), "%s", url);
            } else {
                char base[128] = {0};
                if (ha_client_get_base_url(base, sizeof(base))) {
                    snprintf(s_tts_url, sizeof(s_tts_url), "%s%s", base, url);
                }
            }
            xEventGroupSetBits(s_events, EV_TTS);
        }
    } else if (strcmp(type, "error") == 0) {
        const char *code = assist_json_str(data, (const char *const[]){"code"}, 1);
        const char *message = assist_json_str(data, (const char *const[]){"message"}, 1);
        ESP_LOGW(TAG, "pipeline error %s: %s", code ? code : "?", message ? message : "?");
        assist_set_text(s_status.error, sizeof(s_status.error), code != NULL ? code : "error");
        xEventGroupSetBits(s_events, EV_RUN_END | EV_STT_DONE);
    } else if (strcmp(type, "run-end") == 0) {
        xEventGroupSetBits(s_events, EV_RUN_END | EV_STT_DONE);
    }
}

static esp_err_t assist_send_frame(uint8_t *frame, const int16_t *pcm, size_t samples)
{
    frame[0] = (uint8_t)s_handler_id;
    memcpy(frame + 1, pcm, samples * sizeof(int16_t));
    return ha_client_send_binary(frame, 1 + samples * sizeof(int16_t));
}

/* One push-to-talk session. */
static void assist_session(void)
{
    assist_cfg_t cfg;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    cfg = s_cfg;
    s_status.heard[0] = '\0';
    s_status.reply[0] = '\0';
    s_status.error[0] = '\0';
    xSemaphoreGive(s_lock);
    s_handler_id = -1;
    s_tts_url[0] = '\0';
    s_run_start_us = esp_timer_get_time();
    xEventGroupClearBits(s_events, EV_HANDLER | EV_STT_DONE | EV_TTS | EV_RUN_END);
    assist_set_state(PANEL_ASSIST_LISTENING);

    int16_t *stereo = heap_caps_malloc(ASSIST_FRAME_SAMPLES * PANEL_AUDIO_MIC_CHANNELS * sizeof(int16_t), MALLOC_CAP_DEFAULT);
    int16_t *prebuf = heap_caps_malloc(ASSIST_PREBUF_SAMPLES * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    uint8_t *frame = heap_caps_malloc(1 + ASSIST_FRAME_SAMPLES * sizeof(int16_t), MALLOC_CAP_DEFAULT);
    size_t prebuf_used = 0;
    bool mic_open = false;

    if (stereo == NULL || prebuf == NULL || frame == NULL) {
        assist_set_text(s_status.error, sizeof(s_status.error), "no-memory");
        goto fail;
    }
    if (ha_client_assist_run(cfg.pipeline_id, ASSIST_RATE, assist_event_cb, NULL) != ESP_OK) {
        assist_set_text(s_status.error, sizeof(s_status.error), "ha-offline");
        goto fail;
    }
    if (panel_audio_open_mic() != ESP_OK) {
        assist_set_text(s_status.error, sizeof(s_status.error), "mic");
        goto fail;
    }
    mic_open = true;
    (void)panel_audio_set_mic_gain((float)cfg.mic_gain);

    /* ---- record ---- */
    const int64_t deadline = esp_timer_get_time() + (int64_t)cfg.max_listen_sec * 1000000;
    int16_t mono[ASSIST_FRAME_SAMPLES];
    bool cancelled = false;
    while (true) {
        EventBits_t bits = xEventGroupGetBits(s_events);
        if (bits & EV_CANCEL) {
            cancelled = true;
            break;
        }
        if ((bits & (EV_STOP | EV_STT_DONE)) || esp_timer_get_time() > deadline) {
            break;
        }
        int frames = panel_audio_read(stereo, ASSIST_FRAME_SAMPLES);
        if (frames <= 0) {
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }
        for (int i = 0; i < frames; i++) {
            int32_t l = stereo[i * PANEL_AUDIO_MIC_CHANNELS + 0];
            int32_t r = stereo[i * PANEL_AUDIO_MIC_CHANNELS + 1];
            mono[i] = (int16_t)((l + r) >> 1);
        }
        if (s_handler_id < 0) {
            size_t n = (size_t)frames;
            if (prebuf_used + n > ASSIST_PREBUF_SAMPLES) {
                n = ASSIST_PREBUF_SAMPLES - prebuf_used;
            }
            memcpy(prebuf + prebuf_used, mono, n * sizeof(int16_t));
            prebuf_used += n;
            continue;
        }
        if (prebuf_used > 0) {
            for (size_t off = 0; off < prebuf_used; off += ASSIST_FRAME_SAMPLES) {
                size_t n = prebuf_used - off;
                if (n > ASSIST_FRAME_SAMPLES) {
                    n = ASSIST_FRAME_SAMPLES;
                }
                (void)assist_send_frame(frame, prebuf + off, n);
            }
            prebuf_used = 0;
        }
        (void)assist_send_frame(frame, mono, (size_t)frames);
    }
    panel_audio_close_mic();
    mic_open = false;
    if (cancelled) {
        goto cancel;
    }

    /* HA not ready yet (slow start): wait for the handler, then flush. */
    if (s_handler_id < 0) {
        EventBits_t bits = xEventGroupWaitBits(
            s_events, EV_HANDLER | EV_RUN_END | EV_CANCEL, pdFALSE, pdFALSE, pdMS_TO_TICKS(ASSIST_HANDLER_WAIT_MS));
        if (bits & EV_CANCEL) {
            goto cancel;
        }
        if (s_handler_id < 0) {
            if (s_status.error[0] == '\0') {
                assist_set_text(s_status.error, sizeof(s_status.error), "no-pipeline");
            }
            goto fail;
        }
        for (size_t off = 0; off < prebuf_used; off += ASSIST_FRAME_SAMPLES) {
            size_t n = prebuf_used - off;
            if (n > ASSIST_FRAME_SAMPLES) {
                n = ASSIST_FRAME_SAMPLES;
            }
            (void)assist_send_frame(frame, prebuf + off, n);
        }
    }
    /* End of audio: a frame with only the handler id. */
    if (!(xEventGroupGetBits(s_events) & EV_RUN_END)) {
        frame[0] = (uint8_t)s_handler_id;
        (void)ha_client_send_binary(frame, 1);
    }

    /* ---- wait for the answer ---- */
    assist_set_state(PANEL_ASSIST_PROCESSING);
    EventBits_t bits = xEventGroupWaitBits(
        s_events, EV_TTS | EV_RUN_END | EV_CANCEL, pdFALSE, pdFALSE, pdMS_TO_TICKS(ASSIST_RESULT_WAIT_MS));
    if (bits & EV_CANCEL) {
        goto cancel;
    }
    if (bits & EV_TTS) {
        assist_set_state(PANEL_ASSIST_DONE);
        if (cfg.reply_audio && s_tts_url[0] != '\0') {
            ESP_LOGI(TAG, "reply audio %s", s_tts_url);
            esp_err_t err = panel_radio_play_once(s_tts_url, "Assist");
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "reply playback failed: %s", esp_err_to_name(err));
            }
        }
        (void)xEventGroupWaitBits(s_events, EV_RUN_END | EV_CANCEL, pdFALSE, pdFALSE, pdMS_TO_TICKS(5000));
    } else if (!(bits & EV_RUN_END)) {
        assist_set_text(s_status.error, sizeof(s_status.error), "timeout");
        goto fail;
    }
    ha_client_assist_detach();
    if (s_status.error[0] != '\0') {
        assist_set_state(PANEL_ASSIST_ERROR);
    } else {
        assist_set_state(PANEL_ASSIST_DONE);
    }
    goto out;

cancel:
    ha_client_assist_detach();
    assist_set_state(PANEL_ASSIST_IDLE);
    goto out;

fail:
    ha_client_assist_detach();
    if (mic_open) {
        panel_audio_close_mic();
    }
    assist_set_state(PANEL_ASSIST_ERROR);

out:
    free(stereo);
    free(prebuf);
    free(frame);
}

static void assist_task(void *arg)
{
    (void)arg;
    while (true) {
        xEventGroupWaitBits(s_events, EV_START, pdTRUE, pdFALSE, portMAX_DELAY);
        s_busy = true;
        assist_session();
        s_busy = false;
    }
}

static void assist_copy_cfg(const runtime_settings_t *settings)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_cfg.enabled = settings->assist_enabled;
    snprintf(s_cfg.pipeline_id, sizeof(s_cfg.pipeline_id), "%s", settings->assist_pipeline_id);
    s_cfg.reply_audio = settings->assist_reply_audio;
    s_cfg.mic_gain = settings->assist_mic_gain;
    s_cfg.max_listen_sec = settings->assist_max_listen_sec;
    if (s_cfg.max_listen_sec < 3 || s_cfg.max_listen_sec > 30) {
        s_cfg.max_listen_sec = 10;
    }
    xSemaphoreGive(s_lock);
}

esp_err_t panel_assist_init(const runtime_settings_t *settings)
{
    if (s_lock != NULL) {
        return ESP_OK;
    }
    s_lock = xSemaphoreCreateMutex();
    s_events = xEventGroupCreate();
    if (s_lock == NULL || s_events == NULL) {
        return ESP_ERR_NO_MEM;
    }
    if (settings != NULL) {
        assist_copy_cfg(settings);
    }
    if (xTaskCreatePinnedToCore(assist_task, "assist", 6144, NULL, 5, &s_task, 1) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "ready (%s)", s_cfg.enabled ? "enabled" : "disabled");
    return ESP_OK;
}

void panel_assist_apply_settings(const runtime_settings_t *settings)
{
    if (settings == NULL || s_lock == NULL) {
        return;
    }
    assist_copy_cfg(settings);
    if (!s_cfg.enabled) {
        panel_assist_cancel();
    }
}

bool panel_assist_is_enabled(void)
{
    return s_lock != NULL && s_cfg.enabled;
}

esp_err_t panel_assist_start(void)
{
    if (!panel_assist_is_enabled()) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_busy) {
        return ESP_ERR_INVALID_STATE;
    }
    s_busy = true; /* the task picks it up right away */
    xEventGroupClearBits(s_events, EV_STOP | EV_CANCEL);
    xEventGroupSetBits(s_events, EV_START);
    return ESP_OK;
}

void panel_assist_stop_listening(void)
{
    if (s_events != NULL) {
        xEventGroupSetBits(s_events, EV_STOP);
    }
}

void panel_assist_cancel(void)
{
    if (s_events == NULL) {
        return;
    }
    if (s_busy) {
        xEventGroupSetBits(s_events, EV_CANCEL);
    } else {
        assist_set_state(PANEL_ASSIST_IDLE);
    }
}

bool panel_assist_is_busy(void)
{
    return s_busy;
}

void panel_assist_get_status(panel_assist_status_t *out)
{
    if (out == NULL) {
        return;
    }
    if (s_lock == NULL) {
        memset(out, 0, sizeof(*out));
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_status;
    xSemaphoreGive(s_lock);
}
