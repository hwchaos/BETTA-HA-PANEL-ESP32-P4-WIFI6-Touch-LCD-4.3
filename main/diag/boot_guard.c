/* SPDX-License-Identifier: LicenseRef-FNCL-1.1
 * Copyright (c) 2026 Cpt_Kirk
 */
#include "boot_guard.h"

#include <stdio.h>
#include <string.h>

#include "esp_attr.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "app_config.h"
#include "diag/system_log.h"
#include "net/wifi_mgr.h"
#include "util/log_tags.h"

/* Boot counter shared through RTC memory: it survives software, panic and
 * watchdog resets (so a reboot loop is visible in the log) and is zeroed by a
 * real power-on reset. */
#define BOOT_GUARD_MAGIC 0x42475431UL /* "BGT1" */
RTC_NOINIT_ATTR static uint32_t s_rtc_magic;
RTC_NOINIT_ATTR static uint32_t s_rtc_boot_count;
/* Uptime stamp refreshed every few seconds by boot_guard_stage(); after a
 * crash it holds roughly how long the previous boot survived. */
RTC_NOINIT_ATTR static uint32_t s_rtc_uptime_s;
RTC_NOINIT_ATTR static char s_rtc_stage[BOOT_GUARD_STAGE_MAX];

#define BOOT_GUARD_POLL_MS 5000

static bool s_initialized;
static boot_guard_info_t s_info;
static bool s_confirmed;

/* Copies the previous boot's stage out of RTC memory, rejecting whatever a
 * power-on reset left there (uninitialised RTC memory is arbitrary bytes). */
static void boot_guard_take_prev_stage(void)
{
    const size_t len = strnlen(s_rtc_stage, BOOT_GUARD_STAGE_MAX);
    if (len == 0 || len >= BOOT_GUARD_STAGE_MAX) {
        snprintf(s_info.prev_stage, sizeof(s_info.prev_stage), "unknown");
        return;
    }
    for (size_t i = 0; i < len; i++) {
        const char c = s_rtc_stage[i];
        if (c < 0x20 || c > 0x7e) {
            snprintf(s_info.prev_stage, sizeof(s_info.prev_stage), "unknown");
            return;
        }
    }
    memcpy(s_info.prev_stage, s_rtc_stage, len);
    s_info.prev_stage[len] = '\0';
}

void boot_guard_stage(const char *stage)
{
    s_rtc_uptime_s = (uint32_t)(esp_timer_get_time() / 1000000);
    if (stage == NULL || stage[0] == '\0') {
        return;
    }
    strncpy(s_rtc_stage, stage, sizeof(s_rtc_stage) - 1U);
    s_rtc_stage[sizeof(s_rtc_stage) - 1U] = '\0';
}

static esp_err_t boot_guard_read_ota_state(uint32_t *out_state){
    const esp_partition_t *running = esp_ota_get_running_partition();
    if (running == NULL) {
        return ESP_ERR_NOT_FOUND;
    }

    esp_ota_img_states_t state = ESP_OTA_IMG_UNDEFINED;
    esp_err_t err = esp_ota_get_state_partition(running, &state);
    if (err != ESP_OK) {
        return err;
    }

    *out_state = (uint32_t)state;
    return ESP_OK;
}

/* Refreshes the RTC uptime stamp even when no milestone is reached, so a hang
 * or crash between stages still reports roughly when it happened. */
static void boot_guard_uptime_timer_cb(void *arg)
{
    (void)arg;
    s_rtc_uptime_s = (uint32_t)(esp_timer_get_time() / 1000000);
}

void boot_guard_init(void)
{
    const esp_reset_reason_t reason = esp_reset_reason();

    if (reason == ESP_RST_POWERON || reason == ESP_RST_BROWNOUT || s_rtc_magic != BOOT_GUARD_MAGIC) {
        s_rtc_boot_count = 1;
        s_rtc_magic = BOOT_GUARD_MAGIC;
        s_rtc_uptime_s = 0;
        s_rtc_stage[0] = '\0';
        snprintf(s_info.prev_stage, sizeof(s_info.prev_stage), "unknown");
        s_info.prev_uptime_s = 0;
    } else {
        s_rtc_boot_count++;
        s_info.prev_uptime_s = s_rtc_uptime_s;
        boot_guard_take_prev_stage();
    }

    s_info.reset_reason = reason;
    s_info.boot_count = s_rtc_boot_count;
    s_info.ota_state_err = boot_guard_read_ota_state(&s_info.ota_state);
    s_info.rollback_pending = (s_info.ota_state_err == ESP_OK && s_info.ota_state == ESP_OTA_IMG_PENDING_VERIFY);
    s_info.confirmed = false;
    s_info.confirm_failed = false;
    s_initialized = true;

    static bool timer_started;
    if (!timer_started) {
        const esp_timer_create_args_t args = {
            .callback = boot_guard_uptime_timer_cb,
            .name = "boot_guard",
        };
        esp_timer_handle_t timer = NULL;
        if (esp_timer_create(&args, &timer) == ESP_OK) {
            if (esp_timer_start_periodic(timer, (uint64_t)BOOT_GUARD_POLL_MS * 1000ULL) == ESP_OK) {
                timer_started = true;
            } else {
                esp_timer_delete(timer);
            }
        }
    }
}

void boot_guard_report_boot(void)
{
    if (!s_initialized) {
        boot_guard_init();
    }

    char line[224];
    snprintf(line,
             sizeof(line),
             "boot #%lu, reset reason: %s, OTA state: %s, prev_uptime=%lus, prev_stage=%s",
             (unsigned long)s_info.boot_count,
             boot_guard_reset_reason_str(s_info.reset_reason),
             s_info.ota_state_err == ESP_OK ? boot_guard_ota_state_str(s_info.ota_state)
                                            : esp_err_to_name(s_info.ota_state_err),
             (unsigned long)s_info.prev_uptime_s,
             s_info.prev_stage);
    ESP_LOGI(TAG_APP, "%s", line);
    /* The capture hook only forwards WARN/ERROR, so write the boot line into
     * the persistent log explicitly: it is the first thing to look at when the
     * panel misbehaves after an update. */
    system_log_write_info("app", "%s", line);

    /* A previous boot that died on its own is the most useful symptom of an
     * intermittent fault: name it loudly together with the context it reached. */
    if (s_info.reset_reason == ESP_RST_PANIC || s_info.reset_reason == ESP_RST_TASK_WDT ||
        s_info.reset_reason == ESP_RST_INT_WDT || s_info.reset_reason == ESP_RST_WDT ||
        s_info.reset_reason == ESP_RST_CPU_LOCKUP) {
        ESP_LOGW(TAG_APP,
                 "previous boot crashed after %lus while at stage '%s' (reset=%s) - check /api/logs and the UART backtrace",
                 (unsigned long)s_info.prev_uptime_s,
                 s_info.prev_stage,
                 boot_guard_reset_reason_str(s_info.reset_reason));
    }

    if (s_info.boot_count >= 10) {
        ESP_LOGW(TAG_APP,
                 "%lu boots since power-on: repeated restarts, check /api/logs and /api/diagnostics",
                 (unsigned long)s_info.boot_count);
    }
    if (s_info.rollback_pending) {
        ESP_LOGW(TAG_APP, "running image is PENDING_VERIFY; it must confirm itself or the panel rolls back on reboot");
    }
}

static void boot_guard_confirm_task(void *arg)
{
    (void)arg;

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(BOOT_GUARD_POLL_MS));

        uint32_t state = ESP_OTA_IMG_UNDEFINED;
        esp_err_t err = boot_guard_read_ota_state(&state);
        if (err == ESP_OK && state != ESP_OTA_IMG_PENDING_VERIFY) {
            /* Nothing to confirm (image already valid, or no rollback armed). */
            s_info.rollback_pending = false;
            break;
        }

        const int64_t uptime_ms = esp_timer_get_time() / 1000;
        const bool wifi_ok = wifi_mgr_is_connected();
        const bool uptime_ok = uptime_ms >= APP_BOOT_CONFIRM_TIMEOUT_MS;
        if (!wifi_ok && !uptime_ok) {
            if (uptime_ms >= APP_BOOT_CONFIRM_TIMEOUT_MS / 2) {
                ESP_LOGW(TAG_APP,
                         "rollback confirmation still waiting (wifi=%d, uptime=%llds)",
                         (int)wifi_ok,
                         (long long)(uptime_ms / 1000));
            }
            continue;
        }

        err = esp_ota_mark_app_valid_cancel_rollback();
        if (err == ESP_OK) {
            s_confirmed = true;
            s_info.confirmed = true;
            s_info.rollback_pending = false;
            ESP_LOGI(TAG_APP,
                     "firmware confirmed (wifi=%d, uptime=%llds): OTA rollback cancelled",
                     (int)wifi_ok,
                     (long long)(uptime_ms / 1000));
            break;
        }

        s_info.confirm_failed = true;
        ESP_LOGE(TAG_APP, "failed to confirm firmware: %s (wifi=%d)", esp_err_to_name(err), (int)wifi_ok);
        break;
    }

    vTaskDelete(NULL);
}

void boot_guard_start_confirm_task(void)
{
    if (!s_initialized) {
        boot_guard_init();
    }
    if (s_confirmed || !s_info.rollback_pending) {
        return;
    }
    if (xTaskCreate(boot_guard_confirm_task, "boot_guard", 3072, NULL, 4, NULL) != pdPASS) {
        ESP_LOGW(TAG_APP, "Failed to create boot-guard task");
    }
}

void boot_guard_get_info(boot_guard_info_t *out)
{
    if (out == NULL) {
        return;
    }
    *out = s_info;
    out->confirmed = s_confirmed;
    if (s_info.ota_state_err != ESP_OK) {
        out->ota_state_err = boot_guard_read_ota_state(&out->ota_state);
    }
}

const char *boot_guard_reset_reason_str(esp_reset_reason_t reason)
{
    switch (reason) {
    case ESP_RST_UNKNOWN:
        return "unknown";
    case ESP_RST_POWERON:
        return "power_on";
    case ESP_RST_EXT:
        return "external";
    case ESP_RST_SW:
        return "software";
    case ESP_RST_PANIC:
        return "panic";
    case ESP_RST_INT_WDT:
        return "interrupt_wdt";
    case ESP_RST_TASK_WDT:
        return "task_wdt";
    case ESP_RST_WDT:
        return "wdt";
    case ESP_RST_DEEPSLEEP:
        return "deep_sleep";
    case ESP_RST_BROWNOUT:
        return "brownout";
    case ESP_RST_SDIO:
        return "sdio";
    case ESP_RST_USB:
        return "usb";
    case ESP_RST_JTAG:
        return "jtag";
    case ESP_RST_EFUSE:
        return "efuse";
    case ESP_RST_PWR_GLITCH:
        return "power_glitch";
    case ESP_RST_CPU_LOCKUP:
        return "cpu_lockup";
    default:
        break;
    }
    return "other";
}

const char *boot_guard_ota_state_str(uint32_t state)
{
    switch ((esp_ota_img_states_t)state) {
    case ESP_OTA_IMG_NEW:
        return "new";
    case ESP_OTA_IMG_PENDING_VERIFY:
        return "pending_verify";
    case ESP_OTA_IMG_VALID:
        return "valid";
    case ESP_OTA_IMG_INVALID:
        return "invalid";
    case ESP_OTA_IMG_ABORTED:
        return "aborted";
    case ESP_OTA_IMG_UNDEFINED:
        break;
    default:
        break;
    }
    return "undefined";
}
