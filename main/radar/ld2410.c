#include "radar/ld2410.h"

#include <stdio.h>
#include <string.h>

#include "driver/uart.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "drivers/display_init.h"

static const char *TAG = "ld2410";

#define LD_UART UART_NUM_1
#define LD_RX_BUF 1024
#define LD_FRAME_MAX 64
#define LD_DETECT_TIMEOUT_MS 3000
#define LD_KEEP_AWAKE_PERIOD_MS 5000

typedef struct {
    bool enabled;
    int tx_pin;
    int rx_pin;
    int baud;
    bool wake;
    int wake_cm;
    bool keep_awake;
} ld_cfg_t;

static SemaphoreHandle_t s_lock;
static ld_cfg_t s_cfg;
static ld2410_status_t s_status;
static TaskHandle_t s_task;
static volatile bool s_stop;
static int64_t s_last_frame_us;
static int64_t s_last_wake_us;
static bool s_was_near;
static volatile bool s_selftest; /* test frames: no wake, status restored */

/* ---- frame parsing ---- */

static void ld_handle_report(const uint8_t *d, size_t len)
{
    /* d: [type][0xAA][state][mv L H][mv energy][st L H][st energy][det L H] ... [0x55][0x00] */
    if (len < 11 || (d[0] != 0x01 && d[0] != 0x02) || d[1] != 0xAA) {
        s_status.bad_frames++;
        return;
    }
    const ld2410_target_t target = (ld2410_target_t)(d[2] & 0x03);
    const uint16_t mv_cm = (uint16_t)(d[3] | (d[4] << 8));
    const uint16_t st_cm = (uint16_t)(d[6] | (d[7] << 8));
    const uint16_t det_cm = (uint16_t)(d[9] | (d[10] << 8));

    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_status.frames++;
    s_status.target = target;
    s_status.moving_cm = mv_cm;
    s_status.moving_energy = d[5];
    s_status.static_cm = st_cm;
    s_status.static_energy = d[8];
    s_status.detect_cm = det_cm;
    const ld_cfg_t cfg = s_cfg;
    xSemaphoreGive(s_lock);
    s_last_frame_us = esp_timer_get_time();

    /* "near": the closest present target within the wake distance */
    bool near = false;
    if (target == LD2410_TARGET_MOVING || target == LD2410_TARGET_BOTH) {
        near = near || mv_cm <= cfg.wake_cm;
    }
    if (target == LD2410_TARGET_STATIC || target == LD2410_TARGET_BOTH) {
        near = near || st_cm <= cfg.wake_cm;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_status.near = near;
    xSemaphoreGive(s_lock);

    if (!cfg.wake || s_selftest) {
        if (!s_selftest) {
            s_was_near = near;
        }
        return;
    }
    const int64_t now = esp_timer_get_time();
    if (near && !s_was_near) {
        display_note_activity_from("radar");
        s_last_wake_us = now;
        ESP_LOGI(TAG, "approach: wake display (moving %u cm, static %u cm)", mv_cm, st_cm);
    } else if (near && cfg.keep_awake && now - s_last_wake_us >= (int64_t)LD_KEEP_AWAKE_PERIOD_MS * 1000) {
        display_note_activity_from("radar");
        s_last_wake_us = now;
    }
    s_was_near = near;
}

static void ld_task(void *arg)
{
    (void)arg;
    uint8_t rx[128];
    uint8_t frame[LD_FRAME_MAX];
    size_t fill = 0;
    while (!s_stop) {
        const int n = uart_read_bytes(LD_UART, rx, sizeof(rx), pdMS_TO_TICKS(200));
        if (n > 0) {
            s_status.rx_bytes += (uint32_t)n;
            for (int j = 0; j < n && s_status.first_count < sizeof(s_status.first_bytes); j++) {
                s_status.first_bytes[s_status.first_count++] = rx[j];
            }
        }
        for (int i = 0; i < n; i++) {
            const uint8_t b = rx[i];
            if (fill < 4) {
                static const uint8_t head[4] = {0xF4, 0xF3, 0xF2, 0xF1};
                if (b == head[fill]) {
                    frame[fill++] = b;
                } else {
                    fill = (b == head[0]) ? 1 : 0;
                    if (fill == 1) {
                        frame[0] = b;
                    }
                }
                continue;
            }
            frame[fill++] = b;
            if (fill >= 6) {
                const size_t dlen = (size_t)(frame[4] | (frame[5] << 8));
                if (dlen + 10 > LD_FRAME_MAX) {
                    s_status.bad_frames++;
                    fill = 0;
                    continue;
                }
                if (fill == dlen + 10) {
                    if (frame[fill - 4] == 0xF8 && frame[fill - 3] == 0xF7 && frame[fill - 2] == 0xF6 &&
                        frame[fill - 1] == 0xF5) {
                        ld_handle_report(&frame[6], dlen);
                    } else {
                        s_status.bad_frames++;
                    }
                    fill = 0;
                }
            }
        }
    }
    s_task = NULL;
    vTaskDelete(NULL);
}

/* ---- start / stop ---- */

static void ld_stop(void)
{
    if (s_task != NULL) {
        s_stop = true;
        for (int i = 0; i < 50 && s_task != NULL; i++) {
            vTaskDelay(pdMS_TO_TICKS(20));
        }
    }
    if (uart_is_driver_installed(LD_UART)) {
        uart_driver_delete(LD_UART);
    }
    s_status.running = false;
}

static void ld_start(const ld_cfg_t *cfg)
{
    s_status.error[0] = '\0';
    if (cfg->tx_pin < 0 || cfg->rx_pin < 0) {
        snprintf(s_status.error, sizeof(s_status.error), "TX/RX pin not set");
        return;
    }
    const uart_config_t uc = {
        .baud_rate = cfg->baud,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    esp_err_t err = uart_driver_install(LD_UART, LD_RX_BUF, 0, 0, NULL, 0);
    if (err == ESP_OK) {
        err = uart_param_config(LD_UART, &uc);
    }
    if (err == ESP_OK) {
        err = uart_set_pin(LD_UART, cfg->tx_pin, cfg->rx_pin, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    }
    if (err != ESP_OK) {
        snprintf(s_status.error, sizeof(s_status.error), "UART: %s", esp_err_to_name(err));
        ESP_LOGW(TAG, "UART setup failed: %s", esp_err_to_name(err));
        if (uart_is_driver_installed(LD_UART)) {
            uart_driver_delete(LD_UART);
        }
        return;
    }
    s_stop = false;
    s_status.running = true;
    s_status.frames = 0;
    s_status.bad_frames = 0;
    s_status.rx_bytes = 0;
    s_status.first_count = 0;
    s_last_frame_us = 0;
    if (/* 8 KB: waking the display runs on this task (saver, backlight, logging) */
    xTaskCreate(ld_task, "ld2410", 8192, NULL, 4, &s_task) != pdPASS) {
        snprintf(s_status.error, sizeof(s_status.error), "task could not be started");
        uart_driver_delete(LD_UART);
        s_status.running = false;
        return;
    }
    ESP_LOGI(TAG, "listening on TX %d / RX %d @ %d baud", cfg->tx_pin, cfg->rx_pin, cfg->baud);
}

void ld2410_apply_settings(const runtime_settings_t *settings)
{
    if (settings == NULL) {
        return;
    }
    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutex();
        if (s_lock == NULL) {
            return;
        }
    }
    ld_cfg_t cfg = {
        .enabled = settings->radar_enabled,
        .tx_pin = settings->radar_tx_pin,
        .rx_pin = settings->radar_rx_pin,
        .baud = settings->radar_baud > 0 ? settings->radar_baud : 256000,
        .wake = settings->radar_wake,
        .wake_cm = settings->radar_wake_cm > 0 ? settings->radar_wake_cm : 150,
        .keep_awake = settings->radar_keep_awake,
    };
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const bool restart = cfg.enabled != s_cfg.enabled || cfg.tx_pin != s_cfg.tx_pin ||
                         cfg.rx_pin != s_cfg.rx_pin || cfg.baud != s_cfg.baud || !s_status.running;
    s_cfg = cfg;
    s_status.enabled = cfg.enabled;
    xSemaphoreGive(s_lock);

    if (!restart) {
        return;
    }
    ld_stop();
    if (cfg.enabled) {
        ld_start(&cfg);
    }
}

esp_err_t ld2410_self_test(char *msg, size_t msg_len)
{
    if (!s_status.running || !uart_is_driver_installed(LD_UART)) {
        snprintf(msg, msg_len, "radar is not active");
        return ESP_ERR_INVALID_STATE;
    }
    /* basic report: moving target at 123 cm, energy 50; static 45 cm, 60 */
    static const uint8_t frame[] = {
        0xF4, 0xF3, 0xF2, 0xF1, 0x0D, 0x00, 0x02, 0xAA, 0x03, 0x7B, 0x00, 0x32,
        0x2D, 0x00, 0x3C, 0x2C, 0x01, 0x55, 0x00, 0xF8, 0xF7, 0xF6, 0xF5};
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const ld2410_status_t saved = s_status;
    xSemaphoreGive(s_lock);
    const int64_t saved_last_frame_us = s_last_frame_us;
    const uint32_t frames_before = s_status.frames;
    const uint32_t bytes_before = s_status.rx_bytes;
    s_selftest = true;
    uart_set_loop_back(LD_UART, true);
    for (int i = 0; i < 3; i++) {
        uart_write_bytes(LD_UART, (const char *)frame, sizeof(frame));
        vTaskDelay(pdMS_TO_TICKS(60));
    }
    vTaskDelay(pdMS_TO_TICKS(300));
    uart_set_loop_back(LD_UART, false);
    const uint32_t got_frames = s_status.frames - frames_before;
    const uint32_t got_bytes = s_status.rx_bytes - bytes_before;
    /* the test frames are not sensor data: put the real state back */
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_status = saved;
    xSemaphoreGive(s_lock);
    s_last_frame_us = saved_last_frame_us;
    s_selftest = false;
    if (got_frames > 0) {
        snprintf(msg, msg_len, "OK: %u test frames received and parsed (%u bytes)", (unsigned)got_frames,
            (unsigned)got_bytes);
        return ESP_OK;
    }
    snprintf(msg, msg_len, "failed: %u bytes came back, but no frame was parsed", (unsigned)got_bytes);
    return ESP_FAIL;
}

void ld2410_get_status(ld2410_status_t *out)
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
    const int64_t now = esp_timer_get_time();
    out->last_frame_age_ms = s_last_frame_us > 0 ? (uint32_t)((now - s_last_frame_us) / 1000) : UINT32_MAX;
    out->detected = out->running && s_last_frame_us > 0 && out->last_frame_age_ms < LD_DETECT_TIMEOUT_MS;
}
