/* SPDX-License-Identifier: LicenseRef-FNCL-1.1 */
#include "assist/panel_wake.h"

#include <dirent.h>
#include <math.h>
#include <new>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#include "frontend.h"
#include "frontend_util.h"
#include "tensorflow/lite/micro/micro_interpreter.h"
#include "tensorflow/lite/micro/micro_mutable_op_resolver.h"
#include "tensorflow/lite/micro/micro_resource_variable.h"
#include "tensorflow/lite/schema/schema_generated.h"

#include "assist/panel_assist.h"
#include "diag/storage_guard.h"
extern "C" {
#include "audio/panel_audio.h"
#include "ha/ha_client.h"
}

static const char *TAG = "wake";

/* Feature settings all microWakeWord models are trained with. */
#define WAKE_RATE 16000
#define WAKE_FEATURES 40
#define WAKE_WINDOW_MS 30
/* Detections are ignored for this many feature slices after (re)starting,
 * until the model's internal state has settled (1 s at 10 ms). */
#define WAKE_SETTLE_SLICES 100
#define WAKE_WINDOW_MAX 20
#define WAKE_PEAK_MS 3000

#define WAKE_DIR "/littlefs/wakeword"
#define WAKE_MANIFEST_MAX 4096
#define WAKE_MODEL_MAX (512 * 1024)
#define WAKE_ARENA_MAX (256 * 1024)

/* Built-in model (main/assist/models). */
extern const uint8_t okay_nabu_tflite_start[] asm("_binary_okay_nabu_tflite_start");
extern const uint8_t okay_nabu_tflite_end[] asm("_binary_okay_nabu_tflite_end");

typedef struct {
    char wake_word[48];
    float cutoff;
    int window;
    int arena;
    int step_ms;
} wake_params_t;

static const wake_params_t BUILTIN_PARAMS = {"Okay Nabu", 0.97f, 5, 26080, 10};

typedef struct {
    bool enabled;
    char model[RUNTIME_WAKE_MODEL_MAX_LEN];
    int mic_gain;
} wake_cfg_t;

static SemaphoreHandle_t s_lock;
static wake_cfg_t s_cfg;
static volatile bool s_reload;
static panel_wake_status_t s_status;

/* ---- model (wake task only) ---- */
static tflite::MicroMutableOpResolver<20> s_ops;
static bool s_ops_ready;
static uint8_t *s_model_buf;
static uint8_t *s_arena;
static tflite::MicroInterpreter *s_interp;
static wake_params_t s_params;
static char s_loaded_id[RUNTIME_WAKE_MODEL_MAX_LEN];
static struct FrontendState s_frontend;
static bool s_frontend_ready;
static uint8_t s_probs[WAKE_WINDOW_MAX];
static int s_prob_idx;
static int s_stride_step;
static int s_ignore;
static uint8_t s_last_prob;

/* Test audio handed to the wake task (panel_wake_test). */
static const int16_t *volatile s_test_pcm;
static size_t s_test_len;
static bool s_test_detected;
static uint8_t s_test_peak;
static SemaphoreHandle_t s_test_done;

/* ------------------------------------------------------------ helpers */

static void wake_set_error(const char *msg)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    snprintf(s_status.error, sizeof(s_status.error), "%s", msg != NULL ? msg : "");
    xSemaphoreGive(s_lock);
}

static bool wake_valid_id(const char *id)
{
    if (id == NULL || id[0] == '\0' || strlen(id) >= RUNTIME_WAKE_MODEL_MAX_LEN) {
        return false;
    }
    for (const char *p = id; *p; p++) {
        if (!((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == '_' || *p == '-')) {
            return false;
        }
    }
    return true;
}

static void wake_path(char *out, size_t len, const char *id, const char *ext)
{
    snprintf(out, len, "%s/%s.%s", WAKE_DIR, id, ext);
}

/* Parameters from a microWakeWord v2 manifest. */
static bool wake_parse_manifest(const char *json, wake_params_t *out, char *model_file, size_t model_len,
    char *err, size_t err_len)
{
    cJSON *root = cJSON_Parse(json);
    if (root == NULL) {
        snprintf(err, err_len, "manifest is not JSON");
        return false;
    }
    bool ok = false;
    cJSON *type = cJSON_GetObjectItemCaseSensitive(root, "type");
    cJSON *version = cJSON_GetObjectItemCaseSensitive(root, "version");
    cJSON *micro = cJSON_GetObjectItemCaseSensitive(root, "micro");
    cJSON *word = cJSON_GetObjectItemCaseSensitive(root, "wake_word");
    cJSON *model = cJSON_GetObjectItemCaseSensitive(root, "model");
    if (!cJSON_IsString(type) || strcmp(type->valuestring, "micro") != 0 || !cJSON_IsObject(micro)) {
        snprintf(err, err_len, "not a microWakeWord manifest");
    } else if (!cJSON_IsNumber(version) || version->valueint != 2) {
        snprintf(err, err_len, "only microWakeWord v2 models are supported");
    } else {
        cJSON *cutoff = cJSON_GetObjectItemCaseSensitive(micro, "probability_cutoff");
        cJSON *window = cJSON_GetObjectItemCaseSensitive(micro, "sliding_window_size");
        cJSON *arena = cJSON_GetObjectItemCaseSensitive(micro, "tensor_arena_size");
        cJSON *step = cJSON_GetObjectItemCaseSensitive(micro, "feature_step_size");
        out->cutoff = cJSON_IsNumber(cutoff) ? (float)cutoff->valuedouble : 0.0f;
        out->window = cJSON_IsNumber(window) ? window->valueint : 0;
        out->arena = cJSON_IsNumber(arena) ? arena->valueint : 0;
        out->step_ms = cJSON_IsNumber(step) ? step->valueint : 10;
        snprintf(out->wake_word, sizeof(out->wake_word), "%s",
            cJSON_IsString(word) ? word->valuestring : "");
        if (model_file != NULL) {
            snprintf(model_file, model_len, "%s", cJSON_IsString(model) ? model->valuestring : "");
        }
        if (out->cutoff <= 0.0f || out->cutoff >= 1.0f || out->window < 1 || out->window > WAKE_WINDOW_MAX ||
            out->arena < 1024 || out->arena > WAKE_ARENA_MAX || out->step_ms < 5 || out->step_ms > WAKE_WINDOW_MS) {
            snprintf(err, err_len, "manifest values out of range");
        } else {
            ok = true;
        }
    }
    cJSON_Delete(root);
    return ok;
}

static char *wake_read_file(const char *path, size_t max, size_t *len_out, bool aligned)
{
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    const long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = NULL;
    if (size > 0 && (size_t)size <= max) {
        buf = (char *)(aligned ? heap_caps_aligned_alloc(16, (size_t)size + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
                               : heap_caps_malloc((size_t)size + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (buf != NULL && fread(buf, 1, (size_t)size, f) != (size_t)size) {
            heap_caps_free(buf);
            buf = NULL;
        }
    }
    fclose(f);
    if (buf != NULL) {
        buf[size] = '\0';
        *len_out = (size_t)size;
    }
    return buf;
}

static bool wake_load_params(const char *id, wake_params_t *out)
{
    if (strcmp(id, PANEL_WAKE_BUILTIN_MODEL) == 0) {
        *out = BUILTIN_PARAMS;
        return true;
    }
    char path[96];
    wake_path(path, sizeof(path), id, "json");
    size_t len = 0;
    char *json = wake_read_file(path, WAKE_MANIFEST_MAX, &len, false);
    if (json == NULL) {
        return false;
    }
    char err[64];
    const bool ok = wake_parse_manifest(json, out, NULL, 0, err, sizeof(err));
    heap_caps_free(json);
    return ok;
}

/* ------------------------------------------------------------ model */

static bool wake_register_ops(void)
{
    if (s_ops_ready) {
        return true;
    }
    /* The operations of the streaming microWakeWord models. */
    if (s_ops.AddCallOnce() != kTfLiteOk || s_ops.AddVarHandle() != kTfLiteOk || s_ops.AddReshape() != kTfLiteOk ||
        s_ops.AddReadVariable() != kTfLiteOk || s_ops.AddStridedSlice() != kTfLiteOk ||
        s_ops.AddConcatenation() != kTfLiteOk || s_ops.AddAssignVariable() != kTfLiteOk ||
        s_ops.AddConv2D() != kTfLiteOk || s_ops.AddMul() != kTfLiteOk || s_ops.AddAdd() != kTfLiteOk ||
        s_ops.AddMean() != kTfLiteOk || s_ops.AddFullyConnected() != kTfLiteOk || s_ops.AddLogistic() != kTfLiteOk ||
        s_ops.AddQuantize() != kTfLiteOk || s_ops.AddDepthwiseConv2D() != kTfLiteOk ||
        s_ops.AddAveragePool2D() != kTfLiteOk || s_ops.AddMaxPool2D() != kTfLiteOk || s_ops.AddPad() != kTfLiteOk ||
        s_ops.AddPack() != kTfLiteOk || s_ops.AddSplitV() != kTfLiteOk) {
        return false;
    }
    s_ops_ready = true;
    return true;
}

static void wake_unload(void)
{
    if (s_interp != NULL) {
        delete s_interp;
        s_interp = NULL;
    }
    heap_caps_free(s_arena);
    s_arena = NULL;
    heap_caps_free(s_model_buf);
    s_model_buf = NULL;
    if (s_frontend_ready) {
        FrontendFreeStateContents(&s_frontend);
        s_frontend_ready = false;
    }
    s_loaded_id[0] = '\0';
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_status.loaded = false;
    xSemaphoreGive(s_lock);
}

static bool wake_load_inner(const char *id);

/* The interpreter object and the frontend's tables are plain mallocs below
 * the PSRAM threshold; the internal RAM is kept for Wi-Fi and TLS. */
static bool wake_load(const char *id)
{
    heap_caps_malloc_extmem_enable(0);
    const bool ok = wake_load_inner(id);
    heap_caps_malloc_extmem_enable(CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL);
    return ok;
}

static bool wake_load_inner(const char *id)
{
    wake_unload();
    if (!wake_register_ops()) {
        wake_set_error("ops");
        return false;
    }
    wake_params_t params;
    if (!wake_load_params(id, &params)) {
        wake_set_error("model not found");
        return false;
    }
    size_t size = 0;
    if (strcmp(id, PANEL_WAKE_BUILTIN_MODEL) == 0) {
        size = (size_t)(okay_nabu_tflite_end - okay_nabu_tflite_start);
        s_model_buf = (uint8_t *)heap_caps_aligned_alloc(16, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (s_model_buf != NULL) {
            memcpy(s_model_buf, okay_nabu_tflite_start, size);
        }
    } else {
        char path[96];
        wake_path(path, sizeof(path), id, "tflite");
        s_model_buf = (uint8_t *)wake_read_file(path, WAKE_MODEL_MAX, &size, true);
    }
    if (s_model_buf == NULL) {
        wake_set_error("model file");
        return false;
    }
    const tflite::Model *model = tflite::GetModel(s_model_buf);
    if (model->version() != TFLITE_SCHEMA_VERSION) {
        wake_set_error("model schema");
        wake_unload();
        return false;
    }
    /* The manifest's arena is the measured minimum; a little more is kept
     * for the resource variables, and one retry with half again. */
    bool ok = false;
    for (int attempt = 0; attempt < 2 && !ok; attempt++) {
        const size_t arena = (size_t)params.arena + 2048 + (attempt ? (size_t)params.arena / 2 : 0);
        s_arena = (uint8_t *)heap_caps_aligned_alloc(16, arena, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (s_arena == NULL) {
            break;
        }
        tflite::MicroAllocator *alloc = tflite::MicroAllocator::Create(s_arena, arena);
        tflite::MicroResourceVariables *vars =
            alloc != NULL ? tflite::MicroResourceVariables::Create(alloc, 20) : NULL;
        if (vars != NULL) {
            s_interp = new (std::nothrow) tflite::MicroInterpreter(model, s_ops, alloc, vars);
        }
        if (s_interp != NULL && s_interp->AllocateTensors() == kTfLiteOk) {
            ok = true;
            ESP_LOGI(TAG, "model %s: arena %u of %u bytes", id, (unsigned)s_interp->arena_used_bytes(),
                (unsigned)arena);
        } else {
            delete s_interp;
            s_interp = NULL;
            heap_caps_free(s_arena);
            s_arena = NULL;
        }
    }
    if (!ok) {
        wake_set_error("model does not load (tensor arena / operations)");
        wake_unload();
        return false;
    }
    TfLiteTensor *input = s_interp->input(0);
    TfLiteTensor *output = s_interp->output(0);
    if (input->type != kTfLiteInt8 || input->dims->size != 3 || input->dims->data[2] != WAKE_FEATURES ||
        output->type != kTfLiteUInt8) {
        wake_set_error("not a streaming wake word model");
        wake_unload();
        return false;
    }

    struct FrontendConfig fc;
    fc.window.size_ms = WAKE_WINDOW_MS;
    fc.window.step_size_ms = (size_t)params.step_ms;
    fc.filterbank.num_channels = WAKE_FEATURES;
    fc.filterbank.lower_band_limit = 125.0f;
    fc.filterbank.upper_band_limit = 7500.0f;
    fc.filterbank.output_scale_shift = 0;
    fc.noise_reduction.smoothing_bits = 10;
    fc.noise_reduction.even_smoothing = 0.025f;
    fc.noise_reduction.odd_smoothing = 0.06f;
    fc.noise_reduction.min_signal_remaining = 0.05f;
    fc.pcan_gain_control.enable_pcan = 1;
    fc.pcan_gain_control.strength = 0.95f;
    fc.pcan_gain_control.offset = 80.0f;
    fc.pcan_gain_control.gain_bits = 21;
    fc.log_scale.enable_log = 1;
    fc.log_scale.scale_shift = 6;
    if (!FrontendPopulateState(&fc, &s_frontend, WAKE_RATE)) {
        wake_set_error("frontend");
        wake_unload();
        return false;
    }
    s_frontend_ready = true;
    s_params = params;
    snprintf(s_loaded_id, sizeof(s_loaded_id), "%s", id);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_status.loaded = true;
    snprintf(s_status.wake_word, sizeof(s_status.wake_word), "%s", params.wake_word);
    s_status.error[0] = '\0';
    xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "model %s (\"%s\") ready: cutoff %.2f, window %d, stride %d", id, params.wake_word,
        (double)params.cutoff, params.window, (int)input->dims->data[1]);
    return true;
}

/* Fresh start: clears the audio history and the model's streaming state. */
static void wake_reset(void)
{
    if (s_frontend_ready) {
        FrontendReset(&s_frontend);
    }
    if (s_interp != NULL) {
        s_interp->Reset();
    }
    memset(s_probs, 0, sizeof(s_probs));
    s_prob_idx = 0;
    s_stride_step = 0;
    s_ignore = -WAKE_SETTLE_SLICES;
}

/* One feature slice into the model; true on a detection. */
static bool wake_feed(const int8_t *features)
{
    TfLiteTensor *input = s_interp->input(0);
    const int stride = input->dims->data[1];
    memcpy(tflite::GetTensorData<int8_t>(input) + WAKE_FEATURES * s_stride_step, features, WAKE_FEATURES);
    if (s_ignore < 0) {
        s_ignore++;
    }
    if (++s_stride_step < stride) {
        return false;
    }
    s_stride_step = 0;

    const int64_t t0 = esp_timer_get_time();
    if (s_interp->Invoke() != kTfLiteOk) {
        wake_set_error("model run failed");
        return false;
    }
    const uint32_t us = (uint32_t)(esp_timer_get_time() - t0);
    const uint8_t prob = s_interp->output(0)->data.uint8[0];
    s_last_prob = prob;
    s_probs[s_prob_idx] = prob;
    s_prob_idx = (s_prob_idx + 1) % s_params.window;

    static int64_t peak_since;
    const int64_t now = esp_timer_get_time();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_status.invoke_us = s_status.invoke_us == 0 ? us : (s_status.invoke_us * 15 + us) / 16;
    if (now - peak_since > (int64_t)WAKE_PEAK_MS * 1000) {
        s_status.peak = 0;
        peak_since = now;
    }
    if (prob > s_status.peak) {
        s_status.peak = prob;
    }
    xSemaphoreGive(s_lock);

    if (s_ignore < 0) {
        return false;
    }
    uint32_t sum = 0;
    for (int i = 0; i < s_params.window; i++) {
        sum += s_probs[i];
    }
    const uint32_t cutoff = (uint32_t)lroundf(s_params.cutoff * 255.0f);
    return sum > cutoff * (uint32_t)s_params.window;
}

/* Audio into the frontend; true on a detection. */
static bool wake_process(const int16_t *pcm, size_t samples)
{
    while (samples > 0) {
        size_t used = 0;
        struct FrontendOutput out = FrontendProcessSamples(&s_frontend, pcm, samples, &used);
        pcm += used;
        samples -= used;
        if (out.size == WAKE_FEATURES) {
            int8_t features[WAKE_FEATURES];
            for (size_t i = 0; i < WAKE_FEATURES; i++) {
                /* the scaling of the TFLite audio frontend the models know */
                int32_t v = ((int32_t)out.values[i] * 256 + 333) / 666 - 128;
                features[i] = (int8_t)(v < -128 ? -128 : (v > 127 ? 127 : v));
            }
            if (wake_feed(features)) {
                return true;
            }
        }
        if (used == 0) {
            break;
        }
    }
    return false;
}

/* ------------------------------------------------------------ task */

/* Microphone level for the status: loudest sample of the last few seconds. */
static void wake_note_level(int32_t loudest)
{
    static int32_t max_abs;
    static int64_t since;
    const int64_t now = esp_timer_get_time();
    if (now - since > (int64_t)WAKE_PEAK_MS * 1000) {
        max_abs = 0;
        since = now;
    }
    max_abs = loudest > max_abs ? loudest : max_abs;
    const int db = max_abs > 0 ? (int)lroundf(20.0f * log10f((float)max_abs / 32768.0f)) : -96;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_status.level_db = (int16_t)db;
    xSemaphoreGive(s_lock);
}

static void wake_set_listening(bool on)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_status.listening = on;
    xSemaphoreGive(s_lock);
}

static void wake_task(void *arg)
{
    (void)arg;
    const int frames = WAKE_RATE / 100; /* 10 ms */
    int16_t *stereo = (int16_t *)heap_caps_malloc(frames * PANEL_AUDIO_MIC_CHANNELS * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    int16_t *mono = (int16_t *)heap_caps_malloc(frames * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    bool mic_open = false;
    int64_t resume_after = 0;

    while (true) {
        wake_cfg_t cfg;
        xSemaphoreTake(s_lock, portMAX_DELAY);
        cfg = s_cfg;
        xSemaphoreGive(s_lock);
        const bool enabled = cfg.enabled && panel_assist_is_enabled();
        const char *model = cfg.model[0] != '\0' ? cfg.model : PANEL_WAKE_BUILTIN_MODEL;

        if (!enabled) {
            if (mic_open) {
                panel_audio_close_mic();
                mic_open = false;
                wake_set_listening(false);
            }
            if (s_interp != NULL) {
                wake_unload();
            }
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }
        if (s_reload || strcmp(s_loaded_id, model) != 0) {
            s_reload = false;
            if (mic_open) {
                panel_audio_close_mic();
                mic_open = false;
                wake_set_listening(false);
            }
            if (!wake_load(model)) {
                ESP_LOGW(TAG, "model %s not loaded: %s", model, s_status.error);
                /* retried on the next settings change */
                while (!s_reload) {
                    xSemaphoreTake(s_lock, portMAX_DELAY);
                    const bool changed = strcmp(s_cfg.model, cfg.model) != 0 || !s_cfg.enabled;
                    xSemaphoreGive(s_lock);
                    if (changed) {
                        break;
                    }
                    vTaskDelay(pdMS_TO_TICKS(500));
                }
                continue;
            }
        }

        if (s_test_pcm != NULL) {
            if (mic_open) {
                panel_audio_close_mic();
                mic_open = false;
                wake_set_listening(false);
            }
            /* no settling time: the test audio starts with its own silence */
            wake_reset();
            s_ignore = 0;
            s_test_detected = false;
            s_test_peak = 0;
            for (size_t off = 0; off < s_test_len; off += (size_t)frames) {
                const size_t n = s_test_len - off < (size_t)frames ? s_test_len - off : (size_t)frames;
                s_last_prob = 0;
                if (wake_process(s_test_pcm + off, n)) {
                    s_test_detected = true;
                }
                s_test_peak = s_last_prob > s_test_peak ? s_last_prob : s_test_peak;
            }
            s_test_pcm = NULL;
            xSemaphoreGive(s_test_done);
            continue;
        }

        /* Pause while a session runs, the speaker is taken or wanted, or Home
         * Assistant is away. */
        const char *pause = NULL;
        if (panel_assist_is_busy()) {
            pause = "assist";
        } else if (panel_audio_output_is_exclusive() || panel_audio_output_wanted()) {
            pause = "speaker";
        } else if (!ha_client_is_connected()) {
            pause = "ha";
        }
        if (pause != NULL || esp_timer_get_time() < resume_after) {
            if (mic_open) {
                panel_audio_close_mic();
                mic_open = false;
                wake_set_listening(false);
                resume_after = esp_timer_get_time() + 500000;
            }
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        if (!mic_open) {
            if (panel_audio_open_mic_shared() != ESP_OK) {
                vTaskDelay(pdMS_TO_TICKS(500));
                continue;
            }
            (void)panel_audio_set_mic_gain((float)cfg.mic_gain);
            mic_open = true;
            wake_reset();
            wake_set_listening(true);
        }

        const int got = panel_audio_read(stereo, frames);
        if (got <= 0) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        int32_t loudest = 0;
        for (int i = 0; i < got; i++) {
            const int32_t l = stereo[i * PANEL_AUDIO_MIC_CHANNELS + 0];
            const int32_t r = stereo[i * PANEL_AUDIO_MIC_CHANNELS + 1];
            mono[i] = (int16_t)((l + r) >> 1);
            const int32_t a = mono[i] < 0 ? -mono[i] : mono[i];
            loudest = a > loudest ? a : loudest;
        }
        wake_note_level(loudest);
        if (!wake_process(mono, (size_t)got)) {
            continue;
        }

        /* Detected: hand the microphones to an Assist session. */
        panel_audio_close_mic();
        mic_open = false;
        wake_set_listening(false);
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_status.detections++;
        s_status.last_detection_s = (uint32_t)(esp_timer_get_time() / 1000000);
        xSemaphoreGive(s_lock);
        ESP_LOGI(TAG, "wake word \"%s\" detected", s_params.wake_word);
        if (panel_assist_start() != ESP_OK) {
            ESP_LOGW(TAG, "assist session did not start");
        }
        /* let the session take over before the pause check above runs */
        for (int i = 0; i < 20 && !panel_assist_is_busy(); i++) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        resume_after = esp_timer_get_time() + 500000;
    }
}

/* ------------------------------------------------------------ API */

static void wake_copy_cfg(const runtime_settings_t *settings)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_cfg.enabled = settings->assist_wake_enabled;
    snprintf(s_cfg.model, sizeof(s_cfg.model), "%s", settings->assist_wake_model);
    s_cfg.mic_gain = settings->assist_mic_gain;
    s_status.enabled = settings->assist_wake_enabled && settings->assist_enabled;
    snprintf(s_status.model, sizeof(s_status.model), "%s",
        s_cfg.model[0] != '\0' ? s_cfg.model : PANEL_WAKE_BUILTIN_MODEL);
    xSemaphoreGive(s_lock);
}

esp_err_t panel_wake_init(const runtime_settings_t *settings)
{
    if (s_lock != NULL) {
        return ESP_OK;
    }
    s_lock = xSemaphoreCreateMutex();
    if (s_lock == NULL) {
        return ESP_ERR_NO_MEM;
    }
    if (settings != NULL) {
        wake_copy_cfg(settings);
    }
    /* Core 0, next to the cover fetcher; LVGL keeps core 1. Stack in PSRAM:
     * the task never touches the flash. */
    if (xTaskCreatePinnedToCoreWithCaps(wake_task, "wake", 8192, NULL, 5, NULL, 0, MALLOC_CAP_SPIRAM) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void panel_wake_apply_settings(const runtime_settings_t *settings)
{
    if (settings != NULL && s_lock != NULL) {
        wake_copy_cfg(settings);
    }
}

void panel_wake_get_status(panel_wake_status_t *out)
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

int panel_wake_list_models(panel_wake_model_t *out, int max)
{
    if (out == NULL || max <= 0) {
        return 0;
    }
    int n = 0;
    memset(&out[n], 0, sizeof(out[n]));
    snprintf(out[n].id, sizeof(out[n].id), "%s", PANEL_WAKE_BUILTIN_MODEL);
    snprintf(out[n].wake_word, sizeof(out[n].wake_word), "%s", BUILTIN_PARAMS.wake_word);
    out[n].builtin = true;
    out[n].cutoff = BUILTIN_PARAMS.cutoff;
    out[n].window = BUILTIN_PARAMS.window;
    out[n].arena = BUILTIN_PARAMS.arena;
    n++;

    DIR *dir = opendir(WAKE_DIR);
    if (dir == NULL) {
        return n;
    }
    struct dirent *e;
    while (n < max && (e = readdir(dir)) != NULL) {
        const size_t len = strlen(e->d_name);
        if (len < 6 || strcmp(e->d_name + len - 5, ".json") != 0 || len - 5 >= RUNTIME_WAKE_MODEL_MAX_LEN) {
            continue;
        }
        char id[RUNTIME_WAKE_MODEL_MAX_LEN];
        memcpy(id, e->d_name, len - 5);
        id[len - 5] = '\0';
        wake_params_t p;
        if (!wake_valid_id(id) || !wake_load_params(id, &p)) {
            continue;
        }
        memset(&out[n], 0, sizeof(out[n]));
        snprintf(out[n].id, sizeof(out[n].id), "%s", id);
        snprintf(out[n].wake_word, sizeof(out[n].wake_word), "%s", p.wake_word);
        out[n].cutoff = p.cutoff;
        out[n].window = p.window;
        out[n].arena = p.arena;
        n++;
    }
    closedir(dir);
    return n;
}

/* GET into a PSRAM buffer (NUL-terminated); follows redirects. */
static char *wake_http_get(const char *url, size_t max, size_t *len_out, char *err, size_t err_len)
{
    esp_http_client_config_t cfg = {};
    cfg.url = url;
    cfg.timeout_ms = 15000;
    cfg.crt_bundle_attach = esp_crt_bundle_attach;
    cfg.max_redirection_count = 5;
    cfg.buffer_size = 2048;
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (client == NULL) {
        snprintf(err, err_len, "http client");
        return NULL;
    }
    char *buf = NULL;
    size_t len = 0;
    for (int redirects = 0; redirects <= 5; redirects++) {
        if (esp_http_client_open(client, 0) != ESP_OK) {
            snprintf(err, err_len, "connection failed");
            break;
        }
        (void)esp_http_client_fetch_headers(client);
        const int status = esp_http_client_get_status_code(client);
        if (status == 301 || status == 302 || status == 303 || status == 307 || status == 308) {
            esp_http_client_set_redirection(client);
            esp_http_client_close(client);
            continue;
        }
        if (status != 200) {
            snprintf(err, err_len, "HTTP %d", status);
            break;
        }
        buf = (char *)heap_caps_aligned_alloc(16, max + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (buf == NULL) {
            snprintf(err, err_len, "no memory");
            break;
        }
        while (true) {
            const int r = esp_http_client_read(client, buf + len, (int)(max - len));
            if (r < 0) {
                snprintf(err, err_len, "download failed");
                heap_caps_free(buf);
                buf = NULL;
                break;
            }
            if (r == 0) {
                break;
            }
            len += (size_t)r;
            if (len >= max) {
                snprintf(err, err_len, "file too large");
                heap_caps_free(buf);
                buf = NULL;
                break;
            }
        }
        break;
    }
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    if (buf != NULL) {
        buf[len] = '\0';
        *len_out = len;
    }
    return buf;
}

static bool wake_write_file(const char *path, const void *data, size_t len)
{
    FILE *f = fopen(path, "wb");
    if (f == NULL) {
        return false;
    }
    const bool ok = fwrite(data, 1, len, f) == len;
    return (fclose(f) == 0) && ok;
}

esp_err_t panel_wake_install(const char *manifest_url, char *id_out, size_t id_len, char *err, size_t err_len)
{
    if (manifest_url == NULL || strncmp(manifest_url, "http", 4) != 0) {
        snprintf(err, err_len, "URL of a manifest (.json) expected");
        return ESP_ERR_INVALID_ARG;
    }
    size_t mlen = 0;
    char *manifest = wake_http_get(manifest_url, WAKE_MANIFEST_MAX, &mlen, err, err_len);
    if (manifest == NULL) {
        return ESP_FAIL;
    }
    wake_params_t params;
    char model_file[96];
    if (!wake_parse_manifest(manifest, &params, model_file, sizeof(model_file), err, err_len)) {
        heap_caps_free(manifest);
        return ESP_ERR_INVALID_ARG;
    }
    /* the model lies next to the manifest unless it is a full URL */
    char url[384];
    if (strstr(model_file, "://") != NULL) {
        snprintf(url, sizeof(url), "%s", model_file);
    } else {
        const char *slash = strrchr(manifest_url, '/');
        const int base = slash != NULL ? (int)(slash - manifest_url) + 1 : 0;
        snprintf(url, sizeof(url), "%.*s%s", base, manifest_url, model_file);
    }
    const char *name = strrchr(url, '/');
    name = name != NULL ? name + 1 : url;
    char id[RUNTIME_WAKE_MODEL_MAX_LEN] = {0};
    const char *dot = strstr(name, ".tflite");
    if (dot == NULL || (size_t)(dot - name) >= sizeof(id)) {
        snprintf(err, err_len, "model file name must end in .tflite");
        heap_caps_free(manifest);
        return ESP_ERR_INVALID_ARG;
    }
    for (size_t i = 0; i < (size_t)(dot - name); i++) {
        const char c = name[i];
        id[i] = (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
    }
    if (!wake_valid_id(id) || strcmp(id, PANEL_WAKE_BUILTIN_MODEL) == 0) {
        snprintf(err, err_len, strcmp(id, PANEL_WAKE_BUILTIN_MODEL) == 0 ? "this model is built in" : "invalid model name");
        heap_caps_free(manifest);
        return ESP_ERR_INVALID_ARG;
    }

    size_t tlen = 0;
    char *tflite = wake_http_get(url, WAKE_MODEL_MAX, &tlen, err, err_len);
    if (tflite == NULL) {
        heap_caps_free(manifest);
        return ESP_FAIL;
    }
    if (tlen < 16 || memcmp(tflite + 4, "TFL3", 4) != 0) {
        snprintf(err, err_len, "not a TensorFlow Lite model");
        heap_caps_free(manifest);
        heap_caps_free(tflite);
        return ESP_ERR_INVALID_ARG;
    }

    char path_json[96];
    char path_model[96];
    wake_path(path_json, sizeof(path_json), id, "json");
    wake_path(path_model, sizeof(path_model), id, "tflite");
    storage_guard_begin("wakeword-install");
    mkdir(WAKE_DIR, 0755);
    /* model first: a manifest marks a complete install */
    bool ok = wake_write_file(path_model, tflite, tlen) && wake_write_file(path_json, manifest, mlen);
    if (!ok) {
        remove(path_json);
        remove(path_model);
    }
    storage_guard_end();
    heap_caps_free(manifest);
    heap_caps_free(tflite);
    if (!ok) {
        snprintf(err, err_len, "storage full or not writable");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "installed %s (\"%s\", %u bytes)", id, params.wake_word, (unsigned)tlen);
    if (id_out != NULL) {
        snprintf(id_out, id_len, "%s", id);
    }
    if (strcmp(s_loaded_id, id) == 0) {
        s_reload = true;
    }
    return ESP_OK;
}

esp_err_t panel_wake_test(const int16_t *pcm, size_t samples, bool *detected, uint8_t *peak)
{
    if (s_lock == NULL || pcm == NULL || samples == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_interp == NULL) {
        return ESP_ERR_INVALID_STATE; /* wake word off or model not loaded */
    }
    if (s_test_done == NULL) {
        s_test_done = xSemaphoreCreateBinary();
        if (s_test_done == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }
    s_test_len = samples;
    s_test_pcm = pcm;
    if (xSemaphoreTake(s_test_done, pdMS_TO_TICKS(30000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    *detected = s_test_detected;
    *peak = s_test_peak;
    return ESP_OK;
}

esp_err_t panel_wake_delete(const char *id)
{
    if (!wake_valid_id(id) || strcmp(id, PANEL_WAKE_BUILTIN_MODEL) == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    char path[96];
    wake_path(path, sizeof(path), id, "json");
    storage_guard_begin("wakeword-delete");
    const bool had = remove(path) == 0;
    wake_path(path, sizeof(path), id, "tflite");
    remove(path);
    storage_guard_end();
    return had ? ESP_OK : ESP_ERR_NOT_FOUND;
}
