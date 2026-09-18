/* SPDX-License-Identifier: LicenseRef-FNCL-1.1
 * Copyright (c) 2026 Cpt_Kirk
 *
 * Screen-flash detector ("why does the display blink?").
 *
 * The panel can visibly flash - a whole-screen wash of a light colour - without
 * a single log line, because the pixels are produced by the DSI scan-out while
 * every task reports health.  This watchdog samples the panel's own scan-out
 * framebuffers (not LVGL, not a snapshot) and reports the flashes themselves:
 *
 *  - samples ~every 40 ms, on a grid of ~240 points per framebuffer,
 *  - tracks mean luma, per-channel means, bright-pixel fraction and saturation
 *    per framebuffer,
 *  - a *transient* jump (the image goes much brighter/darker and returns within
 *    ~1.5 s) is a flash: it is logged as a WARN line with the peak values, the
 *    duration, the current backlight and the recent UI/HTTP event notes,
 *  - a jump that does not return but leaves a washed-out screen (high bright
 *    fraction, low saturation - i.e. the light blue/white screen the user sees)
 *    is logged as a stuck light screen,
 *  - a change that brightens the screen and *stays* bright - the screensaver
 *    with its wallpaper, or a page/theme switch - is logged as
 *    "SCREEN CHANGE <kind>" with the mean colour of the pixels that brightened
 *    (flash_rgb), which is what identifies the culprit,
 *  - on top of that 40 ms pass there are two coarse passes over a 48-point grid
 *    of the same framebuffers: a *fast* one every 4 ms, which compares a sample
 *    against the one before it and so catches a screen change that lives for a
 *    single frame, and a *slow* one once a second, which compares a sample
 *    against its 1 s-old baseline and so catches a change that grew step by step
 *    (a fade, a page turn, a wallpaper swap).  Both compare the grid per colour
 *    channel, not by luma, because the worst changes here are colour changes with
 *    a nearly unchanged mean luma - the dark menu and the blue screensaver
 *    wallpaper differ by about 5 luma but by 20+ in the blue and red channels.
 *    They log "FAST|SLOW UP|DOWN" with the colour before and after, the size and
 *    direction of the channel move that fired, the coverage, the mean colour of
 *    the points that moved, the LEDC duty, the software brightness, the page and
 *    the LVGL render counters, and are visible through /api/diagnostics and the
 *    health line,
 *  - the light clock wallpaper going up or down is logged where it happens
 *    ("light content on|off"), so the bright-blue moment is in the log even at
 *    log_verbosity 3, where the RAM ring only keeps warnings,
 *  - the health line carries a "glass=<mean>/<min>/<max> rgb=<r>,<g>,<b>"
 *    fingerprint of the grid, which turns the periodic log itself into a trace of
 *    what was on the glass.
 *
 * The colour of the brightened cells is reported on every path, so a flash the
 * user describes as "light blue" arrives in the log as a readback of that very
 * colour.  Sampling does not begin until the first UI paint is on screen
 * (FW_BOOT_IGNORE_MS), which removed a false "stuck dark" report on every boot.
 *
 * Samples are read-only and lock-free, so the detector never interferes with
 * the UI; on panel variants without DSI framebuffers it compiles to no-ops. */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Start the sampling task.  Call once, after display_init() and
 * system_log_init().  Safe to call on any panel variant (no-op where the
 * framebuffers are not available). */
esp_err_t display_flash_watch_start(void);

/* Enable/disable sampling at runtime (the WebUI log settings use this). */
void display_flash_watch_set_enabled(bool enabled);
bool display_flash_watch_get_enabled(void);

/* Remember what just happened, so a flash reported seconds later can be
 * attributed to it: page switches, backlight changes, screenshot requests and
 * any other notable event.  Cheap and lock-free; keeps the last few notes. */
void display_flash_watch_note(const char *event);

/* Mark the start/end of a full-screen capture (screenshot), which is itself a
 * heavy operation on the PSRAM/DSI path. */
void display_flash_watch_note_snapshot(bool active);

/* Tell the detector that the glass currently shows light content (the
 * screensaver's clock wallpaper).  A backlight rise while this is set is a flash
 * even though not a single pixel changed: the panel got brighter with the light
 * content still up, which is the "screen flashes light blue" on a wake.  It is
 * reported as "SCREEN FLASH backlight=light-content <from>% -> <to>%".  The
 * screensaver sets it when the clock is revealed and clears it once the menu has
 * been painted - i.e. it stays set for exactly the window in which a brighten
 * would be visible as a flash. */
void display_flash_watch_set_light_content(bool light);

/* Report that LVGL finished a refresh pass, so the frame on the glass is the one
 * that was just painted.  Called by the display driver from LVGL's RENDER_READY
 * event, several times per second, so it must stay cheap: no logging, no locks.
 * "full_pass" is set for a pass that repainted nearly the whole screen (the
 * fingerprint of the saver wallpaper being replaced by the menu); started_us is
 * the timestamp of its RENDER_START, i.e. the proof that the pass began after a
 * given moment. */
void display_flash_watch_note_paint(bool full_pass, int64_t started_us, uint32_t ms,
                                    uint32_t dirty_pct);

/* Timestamp (esp_timer_get_time) of the last full-screen paint, 0 if none has
 * been seen since boot.  "The menu is painted by now" cannot be decided by a
 * timer - LVGL paints when it gets to run - so the screensaver's deferred
 * brighten waits for this instead. */
int64_t display_flash_watch_last_full_paint_us(void);

/* True when a full-screen pass *started* at or after since_us (i.e. it paints
 * content that was invalidated after that moment, not the frame that was already
 * in flight when the request came in). */
bool display_flash_watch_paint_seen_since(int64_t since_us);

/* Report the screensaver's deferred brighten when it finally runs.  With
 * paint_seen=false the raise went through while the light clock wallpaper was
 * still on the glass: nothing in the pixel domain changed, so this is counted
 * and logged as a flash - the blind spot that made every wake-up brighten look
 * innocent ("flashes=0") while the user watched the screen turn blue. */
void display_flash_watch_report_wake_raise(int from_pct, int to_pct, int64_t waited_ms,
                                           bool paint_seen, int64_t paint_age_ms);

/* How many raises went through before the menu was painted (the flash above) and
 * how many full-screen passes have been seen at all. */
uint32_t display_flash_watch_early_raise_count(void);
uint32_t display_flash_watch_full_paint_count(void);

/* Ask whether a backlight *raise* has to wait.  The display driver calls this
 * from every path that changes the backlight level; it returns true while the
 * light clock wallpaper is on the glass and the request would make the panel
 * brighter, in which case the level is remembered and applied (faded) as soon as
 * the light content leaves - i.e. after the menu has been painted.  Dims, and
 * requests at or below the current level, always return false and go through
 * untouched.  Bounded: if the light content never leaves (a stale flag), the held
 * level is forced through after a couple of seconds, because a panel stuck at
 * saver brightness would be a worse bug than the flash this avoids. */
bool display_flash_watch_hold_raise(int current_pct, int target_pct);

/* Human-readable summary of what has been seen so far, e.g.
 * "flashes=3 washes=1 changes=112 samples=48210 gaps=0 enabled=1 light=0
 *  last=<kind> ...".
 * The "last" field repeats the newest report (kind, coverage, luma, rgb) with
 * its age, so the newest flash stays visible in the health line even after the
 * log ring has rotated past the WARN line that announced it.  "light=1" means
 * the light screensaver wallpaper is on the glass right now (and "last=FLASH-BL"
 * carries the two backlight percentages in its luma fields). */
void display_flash_watch_summary(char *out, size_t out_len);

/* Buffer size for display_flash_watch_get_fast(): one header line plus one line
 * per history entry. */
#define DISPLAY_FLASH_WATCH_FAST_TEXT_LEN 1280

/* Newest-last, multi-line dump of the coarse-pass history (fast + slow events in
 * one ring): the 4 ms grid is the only record that can hold a screen change which
 * lasted less than one fine-pass interval, and the 1 s pass is the only one that
 * holds a change that grew instead of jumping.  One line per event with uptime,
 * framebuffer, kind (fast/slow), direction, the mean colour before and after, the
 * channel and size of the move that fired, the luma before/after, coverage, the
 * mean colour of the points that moved, the LEDC duty, the software brightness,
 * whether the light clock wallpaper was up and the LVGL render-pass counter at
 * that moment.  Empty string on panel variants that have no such detector. */
void display_flash_watch_get_fast(char *out, size_t out_len);

#ifdef __cplusplus
}
#endif
