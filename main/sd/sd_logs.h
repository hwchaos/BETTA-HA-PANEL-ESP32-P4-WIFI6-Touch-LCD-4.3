/* SPDX-License-Identifier: LicenseRef-FNCL-1.1
 * Copyright (c) 2026 Cpt_Kirk
 */
#pragma once

#include <stddef.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Copies every rotated LittleFS log into a timestamped file on the card, then
 * prunes old exports.  Safe to call at any time; does nothing when the card is
 * not mounted. */
esp_err_t sd_logs_export(char *out_name, size_t out_name_len);

/* Deletes the oldest exports until both the file-count and the byte budget in
 * app_config.h are met. */
esp_err_t sd_logs_prune(void);

/* Starts the low-priority task that re-exports the logs every
 * APP_SD_LOG_EXPORT_PERIOD_SEC.  Idempotent. */
void sd_logs_start(void);

#ifdef __cplusplus
}
#endif
