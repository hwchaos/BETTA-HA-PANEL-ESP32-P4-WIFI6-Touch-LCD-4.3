// ============================================================
// Panel audio backend
// ============================================================
//
// Ported from ForgeUI 30_Audio.c (fg_audio_* -> panel_audio_*) for
// the BETTA-HA-PANEL firmware.
//
// Current Hardware Path:
//
// Waveshare board BSP (4.3" or ETH kit)
// BSP audio init
// ESP codec device speaker output (ES8311)
//
// ============================================================

#include "audio/panel_audio.h"
#include "esp_log.h"
#include "esp_err.h"
#include "nvs_flash.h"

#include "bsp/esp32_p4_wifi6_touch_lcd_7b.h"
#include "esp_codec_dev.h"
#include "driver/i2s_std.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "panel_radio.h"

#include <math.h>
#include <stdint.h>
#include <stdbool.h>

static const char *TAG = "audio";

/* Default speaker volume. The ES8311 volume register is 0.5 dB/step and the
 * BSP adds ~+3.6 dB of hw-gain compensation, so volume 85 lands around
 * register 0xB7 (~ -3.9 dB), just below the clean 0 dB point (0xBF). That is
 * a clearly louder but still distortion-free default vs. the old 70 (~0xA8).
 * The value is persisted so a reboot no longer drops the volume back down. */
#define AUDIO_VOLUME_DEFAULT  85
/* Namespace kept from the original module name: it holds the saved volume. */
#define AUDIO_NVS_NS       "xz_audio"
#define AUDIO_NVS_VOL   "volume"

static esp_codec_dev_handle_t g_speaker = NULL;
static esp_codec_dev_handle_t g_mic = NULL;
static bool g_ready = false;
static bool g_mic_open = false;
/* Opened by the wake word listener: given up as soon as the speaker is
 * wanted (g_output_wanted), instead of blocking the radio. */
static volatile bool g_mic_shared = false;
static volatile bool g_output_wanted = false;
static bool g_busy  = false;
static int  g_volume = AUDIO_VOLUME_DEFAULT;

#define AUDIO_SAMPLE_RATE 16000
#define AUDIO_CHANNELS    1
#define AUDIO_BITS        16
#define DURATION_MS 150
#define FREQ        1000

/* Hand-over window when the microphone takes the codec back from a running
 * radio stream: the stream sees the stop flag at its next read timeout. */
#define AUDIO_OUTPUT_HANDOVER_MS 1500
/* Time the wake word listener gets to close a shared microphone. */
#define AUDIO_MIC_YIELD_MS 500

static int16_t g_beep_buffer[AUDIO_SAMPLE_RATE * DURATION_MS / 1000];

/* The speaker is a single resource: the voice path opens it at 16 kHz mono,
 * the radio stream borrows it at the stream's own rate. A mutex serialises
 * open/close/volume and the exclusive flag tells the voice path that the
 * radio owns the codec right now. */
static StaticSemaphore_t g_lock_storage;
static SemaphoreHandle_t g_lock = NULL;
static portMUX_TYPE g_lock_mux = portMUX_INITIALIZER_UNLOCKED;
static volatile bool g_output_exclusive = false;
static uint32_t g_output_rate = AUDIO_SAMPLE_RATE;

static void panel_audio_lock_init(void)
{
    if (g_lock != NULL) {
        return;
    }
    portENTER_CRITICAL(&g_lock_mux);
    if (g_lock == NULL) {
        g_lock = xSemaphoreCreateMutexStatic(&g_lock_storage);
    }
    portEXIT_CRITICAL(&g_lock_mux);
}

static void panel_audio_lock(void)
{
    panel_audio_lock_init();
    xSemaphoreTake(g_lock, portMAX_DELAY);
}

static void panel_audio_unlock(void)
{
    if (g_lock != NULL) {
        xSemaphoreGive(g_lock);
    }
}

/*
 * The BSP's internal default is 22050 Hz mono/16-bit. Voice (and Opus)
 * work best at 16 kHz, so we pass an explicit I2S config. GPIOs come from the
 * BSP header so this stays in sync with the board definition.
 */
static i2s_std_config_t panel_audio_build_i2s_config(void)
{
    i2s_std_config_t cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(AUDIO_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIP_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                       I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = BSP_I2S_MCLK,
            .bclk = BSP_I2S_SCLK,
            .ws   = BSP_I2S_LCLK,
            .dout = BSP_I2S_DOUT,
            .din  = BSP_I2S_DSIN,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv   = false,
            },
        },
    };
    return cfg;
}

esp_codec_dev_sample_info_t panel_audio_sample_info(void)
{
    esp_codec_dev_sample_info_t fs = {
        .bits_per_sample = AUDIO_BITS,
        .channel = AUDIO_CHANNELS,
        .channel_mask = 0,
        .sample_rate = AUDIO_SAMPLE_RATE,
        .mclk_multiple = 0,
    };
    return fs;
}

/* Mic capture runs in stereo so both physical ES7210 mics (one per I2S
 * slot) are read; the voice client downmixes to mono. */
esp_codec_dev_sample_info_t panel_audio_mic_sample_info(void)
{
    esp_codec_dev_sample_info_t fs = {
        .bits_per_sample = AUDIO_BITS,
        .channel = PANEL_AUDIO_MIC_CHANNELS,
        .channel_mask = 0,
        .sample_rate = AUDIO_SAMPLE_RATE,
        .mclk_multiple = 0,
    };
    return fs;
}

static void panel_audio_save_volume(void)
{
    nvs_handle_t h;
    if (nvs_open(AUDIO_NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    nvs_set_i32(h, AUDIO_NVS_VOL, (int32_t)g_volume);
    nvs_commit(h);
    nvs_close(h);
}

static void panel_audio_load_volume(void)
{
    nvs_handle_t h;
    if (nvs_open(AUDIO_NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        return;
    }
    int32_t v = -1;
    if (nvs_get_i32(h, AUDIO_NVS_VOL, &v) == ESP_OK && v >= 0 && v <= 100) {
        g_volume = (int)v;
    }
    nvs_close(h);
}

static esp_err_t panel_audio_init_locked(void)
{
    if (g_ready) return ESP_OK;

    ESP_LOGI(TAG, "Init audio...");

    panel_audio_load_volume();

    i2s_std_config_t i2s_cfg = panel_audio_build_i2s_config();
    esp_err_t err = bsp_audio_init(&i2s_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "bsp_audio_init failed: %s", esp_err_to_name(err));
        return err;
    }

    g_speaker = bsp_audio_codec_speaker_init();
    if (!g_speaker) {
        ESP_LOGE(TAG, "Speaker init failed");
        return ESP_FAIL;
    }

    g_mic = bsp_audio_codec_microphone_init();
    if (!g_mic) {
        ESP_LOGE(TAG, "Mic init failed");
        return ESP_FAIL;
    }

    esp_codec_dev_sample_info_t fs = panel_audio_sample_info();
    fs.sample_rate = g_output_rate;

    err = esp_codec_dev_open(g_speaker, &fs);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_codec_dev_open(speaker) failed: %s", esp_err_to_name(err));
        return err;
    }

    err = esp_codec_dev_set_out_vol(g_speaker, g_volume);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Initial volume set failed: %s", esp_err_to_name(err));
    }

    g_ready = true;
    ESP_LOGI(TAG, "Audio ready (duplex %d Hz / mono / 16-bit)", AUDIO_SAMPLE_RATE);

    return ESP_OK;
}

esp_err_t panel_audio_init(void)
{
    panel_audio_lock();
    const esp_err_t err = panel_audio_init_locked();
    panel_audio_unlock();
    return err;
}

/* --------------------------------------------------------- output sharing */

/* Open the speaker at `rate` (mono / 16-bit) and re-apply the volume.
 * esp_codec_dev_open() is a no-op on an already open device, so the speaker is
 * always closed first to let the new sample rate reach the I2S clock. */
static esp_err_t panel_audio_reopen_output(uint32_t rate)
{
    esp_err_t err = panel_audio_init_locked();
    if (err != ESP_OK) {
        return err;
    }
    if (g_speaker == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_codec_dev_close(g_speaker);
    esp_codec_dev_sample_info_t fs = panel_audio_sample_info();
    fs.sample_rate = rate;
    err = esp_codec_dev_open(g_speaker, &fs);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "speaker reopen at %u Hz failed: %s", (unsigned)rate, esp_err_to_name(err));
        return err;
    }
    g_output_rate = rate;
    esp_codec_dev_set_out_vol(g_speaker, g_volume);
    return ESP_OK;
}

esp_err_t panel_audio_acquire_output(uint32_t rate)
{
    if (rate == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    /* The wake word listener holds the mic only on loan: ask it to let go. */
    if (g_mic_open && g_mic_shared) {
        g_output_wanted = true;
        for (int waited = 0; g_mic_open && waited < AUDIO_MIC_YIELD_MS; waited += 10) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }

    panel_audio_lock();
    esp_err_t err;
    if (g_output_exclusive) {
        err = ESP_ERR_INVALID_STATE;
    } else if (g_mic_open) {
        /* The microphones and the speaker share the I2S clocks: while a voice
         * session holds the mic the output format must not change. */
        err = ESP_ERR_INVALID_STATE;
    } else {
        err = panel_audio_reopen_output(rate);
        if (err == ESP_OK) {
            g_output_exclusive = true;
            ESP_LOGI(TAG, "output taken by the radio at %u Hz", (unsigned)rate);
        }
    }
    /* kept up until the output is taken, so the listener cannot reopen */
    g_output_wanted = false;
    panel_audio_unlock();
    return err;
}

esp_err_t panel_audio_release_output(void)
{
    panel_audio_lock();
    esp_err_t err = ESP_OK;
    if (g_output_exclusive) {
        g_output_exclusive = false;
        if (!g_mic_open) {
            err = panel_audio_reopen_output(AUDIO_SAMPLE_RATE);
        } else {
            g_output_rate = AUDIO_SAMPLE_RATE;
        }
        ESP_LOGI(TAG, "output released back to the voice path");
    }
    panel_audio_unlock();
    return err;
}

esp_err_t panel_audio_set_output_rate(uint32_t rate)
{
    if (rate == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    panel_audio_lock();
    esp_err_t err = ESP_OK;
    if (g_output_rate != rate) {
        err = panel_audio_reopen_output(rate);
    }
    panel_audio_unlock();
    return err;
}

uint32_t panel_audio_get_output_rate(void)
{
    return g_output_rate;
}

bool panel_audio_output_is_exclusive(void)
{
    return g_output_exclusive;
}

esp_err_t panel_audio_set_volume(int volume)
{
    if (volume < 0) volume = 0;
    if (volume > 100) volume = 100;

    g_volume = volume;

    panel_audio_lock();
    esp_err_t err = panel_audio_init_locked();
    if (err == ESP_OK) {
        err = esp_codec_dev_set_out_vol(g_speaker, g_volume);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Volume set failed: %s", esp_err_to_name(err));
        } else {
            ESP_LOGI(TAG, "Volume set: %d%%", g_volume);
            panel_audio_save_volume();
        }
    }
    panel_audio_unlock();

    return err;
}

int panel_audio_get_volume(void)
{
    return g_volume;
}

esp_err_t panel_audio_test_beep(void)
{
    if (g_busy) {
        ESP_LOGW(TAG, "Beep already running");
        return ESP_OK;
    }
    if (g_output_exclusive) {
        ESP_LOGW(TAG, "Speaker is playing a stream");
        return ESP_ERR_INVALID_STATE;
    }

    g_busy = true;

    panel_audio_lock();
    esp_err_t err = panel_audio_init_locked();
    if (err != ESP_OK) {
        panel_audio_unlock();
        g_busy = false;
        return err;
    }

    esp_codec_dev_set_out_vol(g_speaker, g_volume);

    const int samples = AUDIO_SAMPLE_RATE * DURATION_MS / 1000;

    for (int i = 0; i < samples; i++) {
        float t = (float)i / AUDIO_SAMPLE_RATE;
        g_beep_buffer[i] = (int16_t)(sinf(2.0f * (float)M_PI * FREQ * t) * 10000);
    }

    err = esp_codec_dev_write(g_speaker, g_beep_buffer, samples * sizeof(int16_t));
    panel_audio_unlock();

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Write failed: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "BEEP sent at %d%% volume", g_volume);
    }

    g_busy = false;
    return err;
}

esp_err_t panel_audio_open_mic(void)
{
    /* A running radio stream owns the codec: ask it to stop and give it a
     * moment to hand the speaker back before the mic format is set up. */
    if (g_output_exclusive) {
        ESP_LOGI(TAG, "stopping the radio to open the microphone");
        panel_radio_stop();
        for (int waited = 0; g_output_exclusive && waited < AUDIO_OUTPUT_HANDOVER_MS; waited += 25) {
            vTaskDelay(pdMS_TO_TICKS(25));
        }
        if (g_output_exclusive) {
            ESP_LOGW(TAG, "radio did not release the speaker in %d ms", AUDIO_OUTPUT_HANDOVER_MS);
        }
    }

    panel_audio_lock();
    esp_err_t err = panel_audio_init_locked();
    if (err == ESP_OK && !g_mic_open) {
        esp_codec_dev_sample_info_t fs = panel_audio_mic_sample_info();
        if (esp_codec_dev_open(g_mic, &fs) != ESP_CODEC_DEV_OK) {
            ESP_LOGE(TAG, "Failed to open mic device");
            err = ESP_FAIL;
        } else {
            g_mic_open = true;
            ESP_LOGI(TAG, "Mic opened: stereo %d ch, %d Hz", PANEL_AUDIO_MIC_CHANNELS, AUDIO_SAMPLE_RATE);
        }
    }
    g_mic_shared = false;
    panel_audio_unlock();
    return err;
}

esp_err_t panel_audio_open_mic_shared(void)
{
    panel_audio_lock();
    esp_err_t err = ESP_ERR_INVALID_STATE;
    if (!g_output_exclusive && !g_output_wanted && !g_mic_open) {
        err = panel_audio_init_locked();
        if (err == ESP_OK) {
            esp_codec_dev_sample_info_t fs = panel_audio_mic_sample_info();
            if (esp_codec_dev_open(g_mic, &fs) != ESP_CODEC_DEV_OK) {
                err = ESP_FAIL;
            } else {
                g_mic_open = true;
                g_mic_shared = true;
            }
        }
    }
    panel_audio_unlock();
    return err;
}

bool panel_audio_output_wanted(void)
{
    return g_output_wanted;
}

esp_err_t panel_audio_close_mic(void)
{
    if (g_mic == NULL || !g_mic_open) {
        return ESP_OK;
    }
    esp_codec_dev_close(g_mic);
    g_mic_open = false;
    g_mic_shared = false;
    return ESP_OK;
}

esp_err_t panel_audio_set_mic_gain(float db)
{
    if (g_mic == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    int r = esp_codec_dev_set_in_gain(g_mic, db);
    if (r != ESP_CODEC_DEV_OK) {
        ESP_LOGW(TAG, "set_in_gain(%.1f) failed: %d", db, r);
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t panel_audio_play(const int16_t *pcm, int sample_count)
{
    if (pcm == NULL || sample_count <= 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (g_output_exclusive) {
        /* The radio stream is using the speaker; dropping the TTS chunk is
         * better than corrupting the stream. */
        ESP_LOGW(TAG, "speaker busy with a stream, dropping %d samples", sample_count);
        return ESP_ERR_INVALID_STATE;
    }

    panel_audio_lock();
    esp_err_t err = panel_audio_init_locked();
    if (err == ESP_OK) {
        int bytes = sample_count * sizeof(int16_t) * AUDIO_CHANNELS;
        int w = esp_codec_dev_write(g_speaker, (void *)pcm, bytes);
        if (w < 0) {
            ESP_LOGW(TAG, "Speaker write failed: %d", w);
            err = ESP_FAIL;
        }
    }
    panel_audio_unlock();
    return err;
}

int panel_audio_read(int16_t *pcm, int max_frames)
{
    if (g_mic == NULL || !g_mic_open || pcm == NULL || max_frames <= 0) {
        return -1;
    }
    /* esp_codec_dev_read returns an error code (0 = OK), not a byte count;
     * on success the requested buffer has been filled (interleaved stereo,
     * one frame = one sample per mic). */
    int bytes = max_frames * sizeof(int16_t) * PANEL_AUDIO_MIC_CHANNELS;
    int r = esp_codec_dev_read(g_mic, pcm, bytes);
    if (r < 0) {
        return -1;
    }
    return max_frames;
}

esp_codec_dev_handle_t panel_audio_get_speaker(void)
{
    return g_speaker;
}

esp_codec_dev_handle_t panel_audio_get_mic(void)
{
    return g_mic;
}
