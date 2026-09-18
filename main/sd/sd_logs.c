/* SPDX-License-Identifier: LicenseRef-FNCL-1.1
 * Copyright (c) 2026 Cpt_Kirk
 *
 * Copies the persistent LittleFS system log to the microSD card so long-term
 * history survives a reflash or a factory reset of the internal filesystem.
 *
 * `esp_littlefs` has no readdir(), so the exports are built from the known
 * rotation naming scheme (/littlefs/logs/system.log plus `.1` … `.N`) instead
 * of walking the directory.  Data is streamed through a small fixed buffer, so
 * memory use does not depend on the log size.
 */
#include "sd/sd_logs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "app_config.h"
#include "app_task.h"
#include "diag/dsi_underrun_watch.h"
#include "diag/system_log.h"
#include "diag/storage_guard.h"
#include "sd/sd_card.h"
#include "util/log_tags.h"

#if APP_SD_SUPPORTED

static const char *TAG = TAG_SD;

/* One segment per read; the size covers a full rotation generation plus slack
 * for the lines that can overshoot the cap before a rotation happens. */
#define SD_EXPORT_BUF_BYTES ((size_t)APP_LOG_MAX_FILE_BYTES + 4096U)
/* The FatFs -> SDMMC -> PSRAM write path is by far the deepest call chain in the
 * firmware: measured on the panel it left only 88 bytes of the original 16 KB
 * (4096 words) stack free, i.e. four call frames away from a stack protection
 * fault and a reboot.  This task's stack comes from PSRAM, so doubling it costs
 * internal DRAM nothing. */
#define SD_EXPORT_TASK_STACK 8192
#define SD_EXPORT_TASK_PRIO 1

static TaskHandle_t s_export_task;

/* Appends one LittleFS log file to the open export file.
 * Reads through system_log_read_segment() instead of holding its own handle:
 * a rotation cannot rename a file that has an open handle (esp_littlefs logs
 * "Cannot rename; src ... is open"), so streaming the live file for the whole
 * copy made every rotation during an export fail and let the live file grow
 * past its cap.
 * Returns the number of bytes appended (0 when the generation is missing). */
static size_t append_generation(FILE *dest, int generation, char *buf, size_t buf_len)
{
    int got = system_log_read_segment(generation, buf, buf_len);
    if (got <= 0) {
        return 0;
    }

    if (fwrite(buf, 1, (size_t)got, dest) != (size_t)got) {
        ESP_LOGW(TAG, "Write failed while exporting logs");
        return 0;
    }
    return (size_t)got;
}

esp_err_t sd_logs_prune(void)
{
    if (!sd_card_is_mounted()) {
        return ESP_ERR_INVALID_STATE;
    }

    /* One slot above the limit: a listing that stops exactly at the limit cannot
     * tell a full folder from an overflowing one. */
    enum { LOG_SCAN_SLOTS = APP_SD_LOG_MAX_FILES + 1 };
    for (int pass = 0; pass < LOG_SCAN_SLOTS * 2; pass++) {
        sd_dir_entry_t entries[LOG_SCAN_SLOTS];
        size_t count = 0;
        if (sd_card_list(APP_SD_LOG_DIR, entries, LOG_SCAN_SLOTS, &count) != ESP_OK) {
            /* The folder may not exist yet. */
            return ESP_OK;
        }

        /* Oldest first: the exporter uses a sortable timestamp as file name. */
        size_t oldest = 0;
        uint64_t total_bytes = 0;
        for (size_t i = 0; i < count; i++) {
            total_bytes += entries[i].size;
            if (strcmp(entries[i].name, entries[oldest].name) < 0) {
                oldest = i;
            }
        }

        const bool too_many = count > APP_SD_LOG_MAX_FILES;
        const bool too_big = total_bytes > APP_SD_LOG_MAX_BYTES;
        if (!too_many && !too_big) {
            return ESP_OK;
        }

        char rel[APP_SD_MAX_PATH_LEN];
        snprintf(rel, sizeof(rel), "%s/%s", APP_SD_LOG_DIR, entries[oldest].name);
        if (sd_card_remove(rel) != ESP_OK) {
            return ESP_FAIL;
        }
    }

    return ESP_OK;
}

esp_err_t sd_logs_export(char *out_name, size_t out_name_len)
{
    if (!sd_card_is_mounted()) {
        return ESP_ERR_INVALID_STATE;
    }

    /* The card is being formatted right now: the volume under our feet is about
     * to disappear, so writing to it would only add noise.  A firmware upload
     * also holds the guard, and an export there would only steal bandwidth. */
    if (storage_guard_active()) {
        return ESP_ERR_INVALID_STATE;
    }

    time_t now = 0;
    time(&now);
    struct tm local = {0};
    localtime_r(&now, &local);

    char name[APP_SD_MAX_NAME_LEN];
    strftime(name, sizeof(name), "panel-%Y%m%d-%H%M%S.log", &local);

    char rel[APP_SD_MAX_PATH_LEN];
    snprintf(rel, sizeof(rel), "%s/%s", APP_SD_LOG_DIR, name);

    char path[APP_SD_MAX_PATH_LEN];
    if (!sd_card_build_path(rel, path, sizeof(path))) {
        return ESP_ERR_INVALID_ARG;
    }

    FILE *dest = fopen(path, "wb");
    if (dest == NULL) {
        /* FatFs can be built without long file name support, and then the
         * "panel-<timestamp>.log" name above is rejected outright - which used
         * to make every log export fail with a "Cannot open ... for writing"
         * warning.  Fall back to a sortable 8.3 name (L + YY + day-of-year +
         * hour) so the export, and the browser-side log download that uses it,
         * still work on those builds. */
        snprintf(name, sizeof(name), "L%02u%03u%02u.log", (unsigned)(local.tm_year % 100),
                 (unsigned)(local.tm_yday + 1), (unsigned)local.tm_hour);
        snprintf(rel, sizeof(rel), "%s/%s", APP_SD_LOG_DIR, name);
        if (!sd_card_build_path(rel, path, sizeof(path))) {
            return ESP_ERR_INVALID_ARG;
        }
        dest = fopen(path, "wb");
        if (dest == NULL) {
            ESP_LOGW(TAG, "Cannot open %s for writing", path);
            return ESP_FAIL;
        }
    }

    char *buf = malloc(SD_EXPORT_BUF_BYTES);
    if (buf == NULL) {
        fclose(dest);
        remove(path);
        return ESP_ERR_NO_MEM;
    }

    size_t written = 0;
    /* The export streams up to a full rotation set through FatFs on every
     * period; on the SD path that is the longest PSRAM/DMA burst in the whole
     * firmware, so it is announced and bracketed for the DSI monitor. */
    const int64_t export_start_us = esp_timer_get_time();
    system_log_event("sd", "log export begin -> %s", name);
    dsi_bus_activity_begin(DSI_BUS_SD);

    /* Rotated generations are older, so they are copied first. */
    for (int generation = APP_LOG_MAX_ROTATED; generation >= 1; generation--) {
        written += append_generation(dest, generation, buf, SD_EXPORT_BUF_BYTES);
    }
    written += append_generation(dest, 0, buf, SD_EXPORT_BUF_BYTES);

    dsi_bus_activity_end(DSI_BUS_SD);
    free(buf);
    fclose(dest);

    const uint32_t export_ms = (uint32_t)((esp_timer_get_time() - export_start_us) / 1000);
    system_log_event("sd", "log export %s done: %u bytes in %ums", name, (unsigned)written,
                     (unsigned)export_ms);

    if (written == 0) {
        remove(path);
        return ESP_ERR_NOT_FOUND;
    }

    if (out_name != NULL && out_name_len > 0) {
        snprintf(out_name, out_name_len, "%s", name);
    }
    system_log_write_info(TAG_SD, "Log exported to %s (%u bytes)", name, (unsigned)written);
    sd_logs_prune();
    return ESP_OK;
}

static void sd_export_task(void *arg)
{
    (void)arg;

    /* The period is converted to ticks through 64 bits on purpose:
     * pdMS_TO_TICKS() multiplies the millisecond value by configTICK_RATE_HZ
     * inside a 32-bit TickType_t on this (non-SMP) kernel, so passing an
     * hour-scale value in milliseconds wraps.  pdMS_TO_TICKS(6 * 60 * 60 * 1000)
     * evaluated to 125163 ticks, i.e. this "every 6 hours" export actually ran
     * every 2 minutes and 5 seconds - the longest PSRAM/SDIO burst in the
     * firmware, repeating 173 times more often than intended. */
    const TickType_t export_period_ticks =
            (TickType_t)(((uint64_t)APP_SD_LOG_EXPORT_PERIOD_SEC * (uint64_t)configTICK_RATE_HZ));

    /* First export happens on the next boot tick so the card has settled. */
    vTaskDelay(pdMS_TO_TICKS(30000));
    system_log_event("sd", "log export task: period %u s = %u ticks",
                     (unsigned)APP_SD_LOG_EXPORT_PERIOD_SEC, (unsigned)export_period_ticks);

    for (;;) {
        /* Skip while the storage is busy: the volume is torn down for the
         * duration of a format, and an export during an upload would compete
         * with it for the bus. */
        if (sd_card_is_mounted() && !storage_guard_active()) {
            esp_err_t export_err = sd_logs_export(NULL, 0);
            /* This is the deepest FatFs/PSRAM user in the firmware and it runs
             * unattended, so its stack margin is logged: a stack protection
             * fault inside it would otherwise only be visible as a reboot. */
            if (export_err == ESP_OK) {
                system_log_event("sd", "log export task stack margin: %u bytes",
                                 (unsigned)(uxTaskGetStackHighWaterMark(NULL) * sizeof(StackType_t)));
            }
        }
        vTaskDelay(export_period_ticks);
    }
}

void sd_logs_start(void)
{
    if (s_export_task != NULL) {
        return;
    }
#if APP_SD_LOG_EXPORT_PERIOD_SEC > 0
    if (app_task_create(sd_export_task, "sd_logs", SD_EXPORT_TASK_STACK, NULL, SD_EXPORT_TASK_PRIO,
            &s_export_task) != pdPASS) {
        ESP_LOGW(TAG, "Failed to create log export task");
        s_export_task = NULL;
    }
#endif
}

#else /* !APP_SD_SUPPORTED */

esp_err_t sd_logs_export(char *out_name, size_t out_name_len)
{
    (void)out_name;
    (void)out_name_len;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t sd_logs_prune(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

void sd_logs_start(void)
{
}

#endif /* APP_SD_SUPPORTED */
