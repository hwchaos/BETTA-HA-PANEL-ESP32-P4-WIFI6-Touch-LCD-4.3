/* SPDX-License-Identifier: LicenseRef-FNCL-1.1
 * Copyright (c) 2026 Cpt_Kirk
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_system.h"

/* Longest stage marker (including the terminator) kept in RTC memory. */
#define BOOT_GUARD_STAGE_MAX 32

typedef struct {
    uint32_t boot_count;                 /* boots since the last power-on reset */
    esp_reset_reason_t reset_reason;     /* reason for the running boot */
    esp_err_t ota_state_err;             /* result of esp_ota_get_state_partition() */
    uint32_t ota_state;                  /* esp_ota_img_states_t when ota_state_err == ESP_OK */
    bool rollback_pending;               /* image still awaits confirmation */
    bool confirmed;                      /* this boot already marked the image valid */
    bool confirm_failed;                 /* last confirmation attempt failed */
    uint32_t prev_uptime_s;              /* how long the previous boot lasted */
    char prev_stage[BOOT_GUARD_STAGE_MAX]; /* furthest stage the previous boot reached */
} boot_guard_info_t;

/* Records the reboot reason and increments the boot counter. Call once, early
 * in app_main(), before the network is started. */
void boot_guard_init(void);

/* Logs the boot counter, reset reason and OTA image state. Call after the
 * system log capture is initialised, otherwise the line only reaches the UART
 * and never shows up in /api/logs. */
void boot_guard_report_boot(void);

/* Records how far this boot got ("wifi", "ha", "ui", "camera", "screenshot",
 * "ota", ...). The value lives in RTC memory and survives a panic, so the next
 * boot can say where the previous one died and after how long. Cheap: a short
 * string copy, callable from any task. Pass NULL or an empty string to only
 * refresh the uptime stamp. */
void boot_guard_stage(const char *stage);

/* Waits until the panel is demonstrably healthy (Wi-Fi station associated or
 * uptime past APP_BOOT_CONFIRM_TIMEOUT_MS) and then marks the running image as
 * valid so a pending OTA rollback is cancelled. Only spawns a task when the
 * running image actually awaits confirmation. */
void boot_guard_start_confirm_task(void);

void boot_guard_get_info(boot_guard_info_t *out);

/* Human-readable name for an esp_reset_reason_t value (never NULL). */
const char *boot_guard_reset_reason_str(esp_reset_reason_t reason);

/* Human-readable name for an esp_ota_img_states_t value (never NULL). */
const char *boot_guard_ota_state_str(uint32_t state);
