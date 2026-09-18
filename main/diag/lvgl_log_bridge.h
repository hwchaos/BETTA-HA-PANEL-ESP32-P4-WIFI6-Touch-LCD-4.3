/* SPDX-License-Identifier: LicenseRef-FNCL-1.1
 * Copyright (c) 2026 Cpt_Kirk
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* LVGL diagnostics bridge.
 *
 * LVGL's own log module (lv_log) is compiled in for the panel7 build
 * (CONFIG_LV_USE_LOG) but its messages used to go nowhere, because the ESP-IDF
 * port only forwards them to printf when LV_LOG_PRINTF is set.  This bridge
 * registers the LVGL print callback and routes every LVGL warning/error into:
 *   - the UART/console output (so it shows on the serial monitor), and
 *   - the persistent system log ring, visible from verbosity 3 upwards.
 *
 * That makes drawing/API complaints ("lv_obj_set_style_*: invalid ...", failed
 * object creation, style property out of range) diagnosable instead of being
 * silently dropped, which is exactly what the "screen flashed but the log is
 * empty" reports needed.
 *
 * Call once, after system_log_init() and before display_init()/any LVGL use.
 * A no-op when LVGL logging is compiled out (every other panel variant). */
void lvgl_log_bridge_init(void);

/* Record an LVGL/UI line through the same path (tag "LVGL", info level).
 * Intended for drawing milestones that should always be visible in the log
 * (screen transitions, style rebuilds, full refresh requests). */
void lvgl_log_bridge_note(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* Statistics for the heartbeat: total LVGL lines seen and how many were
 * suppressed by the repeat filter (LVGL tends to repeat one message per
 * frame, which would otherwise bury the log). */
void lvgl_log_bridge_stats(unsigned *seen, unsigned *suppressed);

#ifdef __cplusplus
}
#endif
