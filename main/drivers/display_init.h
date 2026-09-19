/* SPDX-License-Identifier: LicenseRef-FNCL-1.1
 * Copyright (c) 2026 Cpt_Kirk
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

esp_err_t display_init(void);
bool display_is_ready(void);
bool display_lock(uint32_t timeout_ms);
void display_unlock(void);
esp_err_t display_set_brightness_percent(int percent);
int display_get_brightness_percent(void);
/* Ramp the backlight to `percent` over `duration_ms` instead of stepping there.
 * Used by the screensaver so the jump between the menu brightness and the clock
 * brightness fades: the step is the only part of that transition that can be
 * smoothed without repainting, and a hard step is visible as a blink on a dark
 * wall.  duration_ms = 0 behaves exactly like display_set_brightness_percent().
 * Panel variants without a ramp apply the target immediately; an explicit
 * display_set_brightness_percent() call cancels a ramp in flight. */
esp_err_t display_fade_brightness_percent(int percent, uint32_t duration_ms);

/* Dimming/auto-power policy.  UI settings edits these so that the active
 * (user-preferred) brightness is remembered instead of being reset to the
 * compile-time default every time the screen is touched. */
typedef struct {
    int active_brightness_percent; /* restored on touch/activity            */
    int dim_brightness_percent;    /* brightness applied after inactivity   */
    uint32_t dim_timeout_ms;       /* inactivity before dim; 0 = never dim  */
    uint32_t off_timeout_ms;       /* inactivity before full screen OFF; 0 = never off */
    bool night_mode_enabled;       /* during night window, OFF instead of DIM */
    int night_start_hour;          /* 0..23                                 */
    int night_end_hour;            /* 0..23 (may be < start to wrap midnight) */
} display_power_config_t;

void display_get_power_config(display_power_config_t *out);
void display_set_power_config(const display_power_config_t *cfg);
/* Enable/disable the driver's own inactivity policy.  When the shared
 * screensaver (ui_screen_saver) owns the backlight it turns this off so the
 * two policies do not fight over the brightness.  Disabled is a no-op for
 * drivers that have no periodic policy of their own. */
void display_set_power_policy_enabled(bool enabled);
/* Set the brightness used when activity wakes the panel (e.g. on touch or an
 * MQTT wake/brightness command). Defaults to APP_DISPLAY_ACTIVE_BRIGHTNESS_PERCENT
 * and should be kept in sync with the user's configured brightness. */
void display_set_active_brightness_percent(int percent);
void display_note_activity(void);
/* Same, but records what woke the panel. Use it wherever the firmware can wake
 * the screen on its own (an API call, music starting, the boot splash): the
 * resulting "wake by <source>" line is the only way to tell a spontaneous
 * wake-up apart from a finger on the glass. */
void display_note_activity_from(const char *source);
/* Boot brightness hold: display_init() lights the panel at the saved level and
 * then defers every further backlight request (last one wins) so the boot splash
 * cannot be lit twice in a row.  The splash calls this the moment the first real
 * page is on the glass; any real activity releases it as well, so the hold can
 * never survive the splash.  A no-op when nothing was requested during boot. */
void display_boot_brightness_release(void);

/* Called on every real interaction, before the driver restores the active
 * brightness.  Returning true means "this module is taking care of the backlight
 * for this wake" and the driver then leaves the level alone - the screensaver
 * uses that to get the light clock wallpaper off the glass before the panel gets
 * brighter, which is the difference between a wake-up and a light-blue flash. */
typedef bool (*display_activity_cb_t)(void);
void display_set_activity_callback(display_activity_cb_t cb);
int64_t display_ms_since_activity(void);

/* Framebuffer count a panel driver may report; callers size their arrays with
 * this so they never overrun a driver that exposes more buffers later. */
#define DISPLAY_FRAME_BUFFER_MAX 3

/* Direct access to the panel's scan-out framebuffers (the exact pixels the DSI
 * is feeding to the glass).  Used by the screenshot endpoint and by the
 * screen-flash detector in diag/display_flash_watch.c, both of which must stay
 * away from LVGL rendering.  Implemented by the panel variants that own a
 * framebuffer; the weak fallbacks report no buffers so callers can fall back
 * to another capture path. */
int display_frame_buffer_count(void);
/* Returns the pointer to framebuffer `index` (or NULL) and, when out_bytes is
 * non-NULL, its size in bytes.  The buffers are only published when the panel
 * scans out RGB565 - the format every reader here decodes - and are tightly
 * packed with a row stride of APP_SCREEN_WIDTH * 2. */
void *display_frame_buffer_get(int index, size_t *out_bytes);

/* ---------------------------------------------------------------------------
 * LVGL render diagnostics
 *
 * Answers "why did the screen flash / stall" without guessing: every LVGL
 * refresh pass is timed from RENDER_START to RENDER_READY (the flush wait is
 * included) and the invalidated area is accumulated into a coarse grid so a
 * pass that repaints nearly the whole screen - the light-blue wash the user
 * sees - can be counted and logged.
 * Implemented by the active panel driver; weak zero fallbacks live in
 * display_frame_buffer.c so panel variants without instrumentation still link.
 * ------------------------------------------------------------------------- */
typedef struct {
    uint32_t passes;         /* refresh passes finished                          */
    uint32_t full_passes;    /* passes that repainted >= 80 % of the screen      */
    uint32_t slow_passes;    /* passes that took >= 120 ms                       */
    uint32_t invalidations;  /* lv_inv_area() calls seen (all passes)            */
    uint32_t last_pass_ms;   /* duration of the newest pass                      */
    uint32_t max_pass_ms;    /* worst pass seen                                  */
    uint8_t last_dirty_pct;  /* screen area repainted by the newest pass, percent */
    uint8_t max_dirty_pct;   /* worst coverage seen                              */
} display_render_stats_t;

/* One notable pass (full repaint or slow pass), handed over from the LVGL
 * refresh callback to a normal task so the render path itself never logs. */
typedef struct {
    uint32_t ms;
    uint8_t dirty_pct;
    bool full;
    uint32_t full_passes;    /* counters at the time of the note                 */
    uint32_t slow_passes;
    uint32_t dropped;        /* notable passes overwritten before being taken    */
    char page_id[16];        /* page that was active when it happened            */
} display_render_note_t;

void display_render_stats_get(display_render_stats_t *out);
/* One-line summary for the periodic heartbeat/log line. */
void display_render_stats_format(char *out, size_t out_len);
/* Takes the oldest pending note; returns false when nothing notable happened.
 * Safe to call from any task, never from inside an LVGL event callback. */
bool display_render_note_take(display_render_note_t *out);

/* Force an invalidation of the active screen from outside the LVGL task.  Used
 * by the render watchdog as a soft recovery: if the pipeline stalled with a
 * stale frame, one full invalidation re-queues a repaint.  No-op while the
 * display is not ready.  Takes the LVGL lock internally, so never call it from
 * an LVGL callback (it would deadlock on the re-entrant port lock). */
void display_force_invalidate(void);
