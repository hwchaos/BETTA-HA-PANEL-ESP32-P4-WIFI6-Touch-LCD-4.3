/* SPDX-License-Identifier: LicenseRef-FNCL-1.1
 * Copyright (c) 2026 Cpt_Kirk
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

esp_err_t ui_runtime_init(void);
esp_err_t ui_runtime_load_layout(const char *layout_json);
esp_err_t ui_runtime_reload_layout(void);
/* Ask the UI task to rebuild the panel from the stored layout (theme + layout
 * files have to be saved before calling this). Falls back to a synchronous
 * rebuild when the event queue is saturated so the caller always learns whether
 * the panel actually picked up the new data. Safe to call from any task. */
esp_err_t ui_runtime_request_layout_reload(void);
/* Same, but the rebuilt UI shows page_id again instead of the first page (the
 * widget/page ids come from the stored layout, so a page that no longer exists
 * falls back to the first page). Never rebuilds from inside the caller, which
 * makes it safe to call from a page show callback. */
esp_err_t ui_runtime_request_layout_reload_on_page(const char *page_id);
esp_err_t ui_runtime_start(void);

/* Watchdog support: the system log task uses these to detect a UI task that is
 * stuck (e.g. blocked while holding the LVGL display lock) and restart the
 * panel instead of letting it hang forever. */
bool ui_runtime_is_running(void);
uint32_t ui_runtime_get_heartbeat(void);
/* State of the UI task for the freeze report, mirroring eTaskState:
 * 0=run, 1=ready, 2=blocked, 3=suspended, 4=deleted, -1=no task yet.  A task
 * that is ready but not running is starved by higher-priority work rather than
 * stuck, so the watchdog may re-arm instead of rebooting. */
int ui_runtime_get_task_state(void);
/* Report progress from inside long-running UI work (layout rebuild) so a slow
 * but healthy operation is not mistaken for a stalled task. */
void ui_runtime_kick_heartbeat(void);

/* Widget-apply diagnostics for the 30 s heartbeat line: how many state sweeps
 * ran, how long the last/maximum one took, how many widgets it actually
 * repainted and how many it skipped because their data had not changed. */
void ui_runtime_get_apply_stats(char *out, size_t out_len);
