/* SPDX-License-Identifier: LicenseRef-FNCL-1.1
 * Copyright (c) 2026 Cpt_Kirk
 */
#include "drivers/display_init.h"

#include <inttypes.h>
#include <stdbool.h>
#include <string.h>
#include <time.h>

#include "bsp/display.h"
#include "bsp/esp32_p4_wifi6_touch_lcd_7b.h"
#include "esp_cache.h"
#include "esp_err.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "esp_timer.h"
#include "lvgl.h"
#include "nvs.h"

#include "app_config.h"
#include "diag/display_flash_watch.h"
#include "diag/dsi_underrun_watch.h"
#include "diag/storage_guard.h"
#include "diag/system_log.h"
#include "ui/ui_pages.h"
#include "util/log_tags.h"

#define DISPLAY_FULL_BUFFER_PIXELS ((APP_SCREEN_WIDTH * APP_SCREEN_HEIGHT))
#define DISPLAY_POWER_EVAL_PERIOD_US (2000ULL * 1000ULL)

#define DISPLAY_NVS_NAMESPACE "display"
#define DISPLAY_NVS_KEY_POWER "power"
#define DISPLAY_NVS_MAGIC 0x44535057U /* "DSPW" */
#define DISPLAY_NVS_VERSION 1U

typedef struct {
    uint32_t magic;
    uint32_t version;
    int32_t active_percent;
    int32_t dim_percent;
    uint32_t dim_timeout_ms;
    uint32_t off_timeout_ms;
    uint32_t night_enabled;
    int32_t night_start_hour;
    int32_t night_end_hour;
} display_power_nvs_t;

static bool s_display_ready = false;
static lv_display_t *s_lv_display = NULL;
static esp_timer_handle_t s_power_timer = NULL;

/* DSI scan-out framebuffers.  Kept so the screenshot endpoint and the
 * screen-flash detector can read the exact pixels the panel is displaying
 * without going through LVGL (a full LVGL snapshot disturbs the display). */
static esp_lcd_panel_handle_t s_panel = NULL;
static void *s_frame_buffers[DISPLAY_FRAME_BUFFER_MAX];
static size_t s_frame_buffer_bytes = 0;
static int s_frame_buffer_count = 0;
static void display_frame_buffer_cache(void);

static int s_display_brightness = -1;
static int s_active_brightness = APP_DISPLAY_ACTIVE_BRIGHTNESS_PERCENT;
static int s_dim_brightness = APP_DISPLAY_DIM_BRIGHTNESS_PERCENT;
static uint32_t s_dim_timeout_ms = APP_DISPLAY_DIM_TIMEOUT_MS;
static uint32_t s_off_timeout_ms = APP_DISPLAY_OFF_TIMEOUT_MS;
static bool s_night_mode_enabled = APP_DISPLAY_NIGHT_MODE_ENABLED;
static int s_night_start_hour = APP_DISPLAY_NIGHT_START_HOUR;
static int s_night_end_hour = APP_DISPLAY_NIGHT_END_HOUR;
static int64_t s_last_activity_us = 0;
static display_activity_cb_t s_activity_cb = NULL;
/* The driver's own inactivity policy is enabled by default; the shared
 * screensaver disables it when it takes over the backlight. */
static bool s_power_policy_enabled = true;

static void display_power_config_load(void);
static void display_power_config_save(void);

/* Diagnostics: ticks inside lv_timer_handler() while taskLVGL is servicing the
 * display.  When the UI heartbeat stops advancing, the gap between two of these
 * ticks tells the freeze report whether LVGL itself stopped being called at all
 * or whether one single handler call is taking seconds. */
static lv_timer_t *s_trace_timer = NULL;

static void display_trace_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    /* Named "trace" so a freeze report can tell a stalled UI callback apart from
     * a stall after the timer list was processed (refresh/render). */
    system_log_note_lvgl_cb("trace");
    system_log_lvgl_handler_beat();

    /* Render ping: force one refresh pass every APP_LVGL_TRACE_PERIOD_MS even
     * when the UI is completely static.  The render watchdog keys on the render
     * pass counter advancing, so an idle panel must still make passes; this 1x1
     * invalidation is that heartbeat.  Never takes the lock here: this callback
     * already runs inside lv_timer_handler() with the LVGL lock held. */
    lv_area_t ping = {0, 0, 0, 0};
    lv_obj_invalidate_area(lv_screen_active(), &ping);
}

/* ---------------------------------------------------------------------------
 * Render diagnostics
 *
 * "The screen flashes light blue" cannot be diagnosed from the LVGL logs alone:
 * a single style write invalidates, and the resulting refresh pass redraws
 * whatever was invalidated.  These stats answer the two questions that matter -
 * how much of the screen did the pass repaint, and how long did it take - by
 * coalescing every lv_inv_area() notification into a coarse dirty grid and by
 * timing RENDER_START -> RENDER_READY (which includes the flush wait).
 * ------------------------------------------------------------------------- */
#define DISPLAY_DIRTY_COLS 32
#define DISPLAY_DIRTY_ROWS 20
#define DISPLAY_DIRTY_CELLS (DISPLAY_DIRTY_COLS * DISPLAY_DIRTY_ROWS)

/* A pass that repaints almost everything is the fingerprint of the flash: the
 * panel redraws 1024x600 through a partial (1/5 screen) buffer with sw_rotate,
 * so a full pass visibly tears. */
#define DISPLAY_FULL_PASS_PCT 80
/* Healthy passes here are a few milliseconds (small tile updates). */
#define DISPLAY_SLOW_PASS_MS 120

static display_render_stats_t s_render_stats;
static uint8_t s_dirty_grid[DISPLAY_DIRTY_CELLS];
static uint16_t s_dirty_cells = 0;
static uint8_t s_pass_dirty_pct = 0;
static int64_t s_pass_start_us = 0;

/* Notable passes are only recorded here - LVGL runs this callback inside its
 * refresh pass, so it must not log, lock or touch the filesystem.  The UI/log
 * task collects the note with display_render_note_take() and reports it. */
static display_render_note_t s_render_note;
static bool s_render_note_pending = false;
static uint32_t s_render_notes_dropped = 0;

static void display_dirty_mark(const lv_area_t *area)
{
    if (area == NULL) {
        return;
    }
    int32_t cell_w = (APP_SCREEN_WIDTH + DISPLAY_DIRTY_COLS - 1) / DISPLAY_DIRTY_COLS;
    int32_t cell_h = (APP_SCREEN_HEIGHT + DISPLAY_DIRTY_ROWS - 1) / DISPLAY_DIRTY_ROWS;
    if (cell_w <= 0 || cell_h <= 0) {
        return;
    }
    int32_t c0 = area->x1 / cell_w;
    int32_t c1 = area->x2 / cell_w;
    int32_t r0 = area->y1 / cell_h;
    int32_t r1 = area->y2 / cell_h;
    if (c0 < 0) c0 = 0;
    if (r0 < 0) r0 = 0;
    if (c1 >= DISPLAY_DIRTY_COLS) c1 = DISPLAY_DIRTY_COLS - 1;
    if (r1 >= DISPLAY_DIRTY_ROWS) r1 = DISPLAY_DIRTY_ROWS - 1;

    s_render_stats.invalidations++;
    for (int32_t r = r0; r <= r1; r++) {
        for (int32_t c = c0; c <= c1; c++) {
            uint8_t *cell = &s_dirty_grid[r * DISPLAY_DIRTY_COLS + c];
            if (*cell == 0) {
                *cell = 1;
                s_dirty_cells++;
            }
        }
    }
}

static void display_note_render_pass(uint32_t ms, uint32_t dirty_pct, bool full)
{
    if (s_render_note_pending) {
        s_render_notes_dropped++;
    }
    s_render_note.ms = ms;
    s_render_note.dirty_pct = (uint8_t)dirty_pct;
    s_render_note.full = full;
    s_render_note.full_passes = s_render_stats.full_passes;
    s_render_note.slow_passes = s_render_stats.slow_passes;
    snprintf(s_render_note.page_id, sizeof(s_render_note.page_id), "%s", ui_pages_current_id());
    s_render_note_pending = true;
}

static void display_render_event_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);

    if (code == LV_EVENT_INVALIDATE_AREA) {
        display_dirty_mark((const lv_area_t *)lv_event_get_param(e));
        return;
    }

    if (code == LV_EVENT_RENDER_START) {
        /* Bracket the repaint (render + flush wait) on the DSI bus monitor: it
         * is the only consumer that is running during every visible update, so
         * without this the underrun lines report "busy=" with no attribution.
         * Guarded against nesting so a pass that never reports READY cannot
         * leave the monitor permanently claiming "render busy". */
        if (!dsi_bus_activity_active(DSI_BUS_RENDER)) {
            dsi_bus_activity_begin(DSI_BUS_RENDER);
        }
        s_pass_dirty_pct = (uint8_t)((s_dirty_cells * 100U) / DISPLAY_DIRTY_CELLS);
        s_render_stats.last_dirty_pct = s_pass_dirty_pct;
        if (s_pass_dirty_pct > s_render_stats.max_dirty_pct) {
            s_render_stats.max_dirty_pct = s_pass_dirty_pct;
        }
        memset(s_dirty_grid, 0, sizeof(s_dirty_grid));
        s_dirty_cells = 0;
        s_pass_start_us = esp_timer_get_time();
        return;
    }

    if (code == LV_EVENT_RENDER_READY) {
        if (dsi_bus_activity_active(DSI_BUS_RENDER)) {
            dsi_bus_activity_end(DSI_BUS_RENDER);
        }
        uint32_t ms = (uint32_t)((esp_timer_get_time() - s_pass_start_us) / 1000);
        uint32_t pct = s_pass_dirty_pct;
        s_render_stats.passes++;
        s_render_stats.last_pass_ms = ms;
        if (ms > s_render_stats.max_pass_ms) {
            s_render_stats.max_pass_ms = ms;
        }
        if (ms >= DISPLAY_SLOW_PASS_MS) {
            s_render_stats.slow_passes++;
        }
        /* A pass that repaints nearly the whole screen is the light-blue wash
         * the user sees: the panel redraws everything through a partial buffer. */
        bool full = pct >= DISPLAY_FULL_PASS_PCT;
        if (full) {
            s_render_stats.full_passes++;
        }
        /* The pass is on the glass now: this is the moment the saver's deferred
         * brighten is allowed to run (see display_flash_watch_note_paint). */
        display_flash_watch_note_paint(full, s_pass_start_us, ms, pct);
        if (full || ms >= DISPLAY_SLOW_PASS_MS) {
            display_note_render_pass(ms, pct, full);
        }
        return;
    }
}

bool display_render_note_take(display_render_note_t *out)
{
    if (out == NULL || !s_render_note_pending) {
        return false;
    }
    *out = s_render_note;
    out->dropped = s_render_notes_dropped;
    s_render_notes_dropped = 0;
    s_render_note_pending = false;
    return true;
}

void display_render_stats_get(display_render_stats_t *out)
{
    if (out == NULL) {
        return;
    }
    *out = s_render_stats;
}

void display_render_stats_format(char *out, size_t out_len)
{
    if (out == NULL || out_len == 0) {
        return;
    }
    snprintf(out, out_len, "render passes=%u inv=%u full=%u slow=%u dirty=%u%%/max%u%% last=%ums max=%ums",
             (unsigned)s_render_stats.passes, (unsigned)s_render_stats.invalidations,
             (unsigned)s_render_stats.full_passes,
             (unsigned)s_render_stats.slow_passes, (unsigned)s_render_stats.last_dirty_pct,
             (unsigned)s_render_stats.max_dirty_pct, (unsigned)s_render_stats.last_pass_ms,
             (unsigned)s_render_stats.max_pass_ms);
}

static lvgl_port_cfg_t display_port_cfg(void)
{
    lvgl_port_cfg_t cfg = ESP_LVGL_PORT_INIT_CONFIG();
    /* Below the UI task and httpd on purpose - see APP_LVGL_TASK_PRIO. */
    cfg.task_priority = APP_LVGL_TASK_PRIO;
    cfg.task_stack = APP_LVGL_TASK_STACK;
    cfg.task_affinity = 1;
    cfg.task_max_sleep_ms = 100;
    return cfg;
}

static int display_clamp_brightness(int percent)
{
    if (percent < 0) {
        return 0;
    }
    if (percent > 100) {
        return 100;
    }
    return percent;
}

/* ---------------------------------------------------------------------------
 * Backlight fade
 *
 * The panel has two very different backlight levels (the menu at the configured
 * brightness, the clock at the screensaver brightness) and used to step between
 * them in one go: on a dark wall that step reads as a blink, and it happens at
 * the same moment as the full-screen repaint the screensaver triggers.  The ramp
 * is driven by an esp_timer, so it needs no LVGL work at all - only the LEDC
 * duty changes - and it writes the duty directly so a 350 ms fade does not push
 * one "backlight" event per 20 ms step into the log.  One line records the whole
 * fade instead.
 * ------------------------------------------------------------------------- */
#define DISPLAY_FADE_STEP_MS 20

static esp_timer_handle_t s_fade_timer = NULL;
static int s_fade_from = -1;
static int s_fade_to = -1;
static int64_t s_fade_start_us = 0;
static uint32_t s_fade_duration_ms = 0;

static void display_fade_abort(void)
{
    s_fade_from = -1;
    s_fade_to = -1;
    s_fade_duration_ms = 0;
    if (s_fade_timer != NULL && esp_timer_is_active(s_fade_timer)) {
        (void)esp_timer_stop(s_fade_timer);
    }
}

/* ---------------------------------------------------------------------------
 * Boot brightness gate
 *
 * At boot the backlight has three owners in a row: the power config lights the
 * glass at its stored level (the dim level the boot splash is meant to come up
 * at), then the settings/screensaver apply asks for the menu level, and the
 * splash hand-over asks for it again.  Every one of those requests used to hit
 * the LEDC the moment it arrived, so the panel lit up twice in a row after every
 * reboot - and a 350 ms fade armed against the pre-boot level while the level
 * had already jumped made the duty *dip* back down before rising again, which is
 * the "the screen flashed, twice, one after another" report that arrives right
 * after an update.  The gate below holds everything asked while the boot splash
 * is on the glass (last request wins) so the panel makes exactly one transition
 * when the splash hands the screen over.
 * ------------------------------------------------------------------------- */
static bool s_boot_gate_open = true;      /* opened by the first activity after boot */
static int s_boot_gate_pending = -1;      /* last deferred request, -1 = nothing */
static uint32_t s_boot_gate_pending_fade_ms = 0;   /* 0 = apply as a step */

static void display_boot_gate_request(int percent, uint32_t fade_ms)
{
    s_boot_gate_pending = display_clamp_brightness(percent);
    s_boot_gate_pending_fade_ms = fade_ms;
}

static void display_boot_gate_close(void)
{
    s_boot_gate_open = false;
    s_boot_gate_pending = -1;
    s_boot_gate_pending_fade_ms = 0;
}

/* Called when the splash hands the glass over, or by the first real activity if
 * the user gets there first, so the panel can never stay gated. */
void display_boot_brightness_release(void)
{
    if (s_boot_gate_open) {
        return;
    }
    s_boot_gate_open = true;

    const int pending = s_boot_gate_pending;
    const uint32_t fade_ms = s_boot_gate_pending_fade_ms;
    s_boot_gate_pending = -1;
    s_boot_gate_pending_fade_ms = 0;
    if (pending < 0) {
        return;
    }
    ESP_LOGI(TAG_DISPLAY, "boot brightness gate: applying the %d%% the UI asked for during boot", pending);
    if (fade_ms > 0) {
        (void)display_fade_brightness_percent(pending, fade_ms);
    } else {
        (void)display_set_brightness_percent(pending);
    }
}

/* Write the duty without the per-step logging of display_set_brightness_percent()
 * but keep the cached value in sync, so display_get_brightness_percent() and the
 * flap detector always see what the glass is showing. */
static void display_fade_write(int percent)
{
    const int next = display_clamp_brightness(percent);
    if (next == s_display_brightness) {
        return;
    }
    if (bsp_display_brightness_set(next) == ESP_OK) {
        s_display_brightness = next;
    }
}

static void display_fade_timer_cb(void *arg)
{
    (void)arg;
    if (s_fade_to < 0) {
        display_fade_abort();
        return;
    }

    const int64_t elapsed_us = esp_timer_get_time() - s_fade_start_us;
    const int64_t duration_us = (int64_t)s_fade_duration_ms * 1000;
    if (duration_us <= 0 || elapsed_us >= duration_us) {
        const int previous = s_fade_from;
        const int target = s_fade_to;
        const uint32_t duration_ms = s_fade_duration_ms;
        display_fade_abort();
        display_fade_write(target);
        ESP_LOGI(TAG_DISPLAY, "backlight %d%% -> %d%% (fade %" PRIu32 "ms)", previous, target, duration_ms);
        system_log_event("backlight", "%d%% -> %d%% (fade %ums)", previous, target, (unsigned)duration_ms);
        return;
    }

    const int span = s_fade_to - s_fade_from;
    const int value = s_fade_from + (int)(((int64_t)span * elapsed_us) / duration_us);
    display_fade_write(value);
}

esp_err_t display_fade_brightness_percent(int percent, uint32_t duration_ms)
{
    const int target = display_clamp_brightness(percent);
    if (!s_boot_gate_open) {
        display_boot_gate_request(target, duration_ms);
        return ESP_OK;
    }
    /* While the light clock wallpaper is on the glass a raise is held: brightening
     * the panel there lights the lightest frame this firmware shows up at the new
     * level - the "screen flashes light blue" - even though no pixel changed.  The
     * held level is applied, faded, once the light content leaves (see
     * diag/display_flash_watch.c).  Dims and same-level requests are unaffected. */
    if (display_flash_watch_hold_raise(s_display_brightness, target)) {
        return ESP_OK;
    }
    if (duration_ms == 0 || s_display_brightness < 0 || target == s_display_brightness) {
        display_fade_abort();
        return display_set_brightness_percent(target);
    }

    if (s_fade_timer == NULL) {
        const esp_timer_create_args_t args = {
            .callback = display_fade_timer_cb,
            .name = "bl_fade",
        };
        if (esp_timer_create(&args, &s_fade_timer) != ESP_OK) {
            s_fade_timer = NULL;
            return display_set_brightness_percent(target);
        }
    }

    s_fade_from = s_display_brightness;
    s_fade_to = target;
    s_fade_duration_ms = duration_ms;
    s_fade_start_us = esp_timer_get_time();

    (void)esp_timer_stop(s_fade_timer);
    if (esp_timer_start_periodic(s_fade_timer, DISPLAY_FADE_STEP_MS * 1000ULL) != ESP_OK) {
        display_fade_abort();
        return display_set_brightness_percent(target);
    }
    return ESP_OK;
}

esp_err_t display_set_brightness_percent(int percent)
{
    const int next = display_clamp_brightness(percent);
    /* A set to the level a fade is already heading for must not interrupt it: the
     * wake path sets the active brightness on every activity, and cancelling the
     * ramp half-way replaces a smooth fade with a jump in the middle of it. */
    if (s_fade_to == next && s_fade_timer != NULL && esp_timer_is_active(s_fade_timer)) {
        return ESP_OK;
    }
    /* An explicit request always wins over a fade that is still in flight. */
    display_fade_abort();
    if (!s_boot_gate_open) {
        display_boot_gate_request(next, 0);
        return ESP_OK;
    }
    if (display_flash_watch_hold_raise(s_display_brightness, next)) {
        return ESP_OK;
    }
    if (s_display_brightness == next) {
        return ESP_OK;
    }
    const int previous = s_display_brightness;
    esp_err_t err = bsp_display_brightness_set(next);
    if (err == ESP_OK) {
        s_display_brightness = next;
        /* Every backlight transition is logged: a step change is one of the
         * things that can look like a screen flash to the user. */
        ESP_LOGI(TAG_DISPLAY, "backlight %d%% -> %d%%", previous, next);
        system_log_event("backlight", "%d%% -> %d%%", previous, next);
    } else {
        ESP_LOGW(TAG_DISPLAY, "Could not set backlight to %d%%: %s", next, esp_err_to_name(err));
    }
    return err;
}

int display_get_brightness_percent(void)
{
    return s_display_brightness;
}

static void display_frame_buffer_cache(void)
{
    s_frame_buffer_count = 0;
    s_frame_buffer_bytes = 0;
    for (int i = 0; i < DISPLAY_FRAME_BUFFER_MAX; i++) {
        s_frame_buffers[i] = NULL;
    }
    if (s_panel == NULL) {
        return;
    }

#if CONFIG_BSP_LCD_COLOR_FORMAT_RGB888
    /* The readers of these buffers (screenshot endpoint, flash detector) decode
     * RGB565, so a 24-bit panel is left to the legacy capture path. */
    ESP_LOGW(TAG_DISPLAY, "Panel uses RGB888; direct framebuffer capture not published");
    return;
#else
    void *fb[DISPLAY_FRAME_BUFFER_MAX] = {NULL, NULL, NULL};
#if CONFIG_BSP_LCD_DPI_BUFFER_NUMS == 1
    esp_err_t err = esp_lcd_dpi_panel_get_frame_buffer(s_panel, 1, &fb[0]);
#elif CONFIG_BSP_LCD_DPI_BUFFER_NUMS == 2
    esp_err_t err = esp_lcd_dpi_panel_get_frame_buffer(s_panel, 2, &fb[0], &fb[1]);
#else
    esp_err_t err = esp_lcd_dpi_panel_get_frame_buffer(s_panel, 3, &fb[0], &fb[1], &fb[2]);
#endif
    if (err != ESP_OK) {
        ESP_LOGW(TAG_DISPLAY, "No access to the panel framebuffers: %s", esp_err_to_name(err));
        return;
    }

    s_frame_buffer_bytes = (size_t)APP_SCREEN_WIDTH * (size_t)APP_SCREEN_HEIGHT * 2U;
    for (int i = 0; i < DISPLAY_FRAME_BUFFER_MAX; i++) {
        if (fb[i] == NULL) {
            break;
        }
        s_frame_buffers[i] = fb[i];
        s_frame_buffer_count++;
    }

    /* Blacken every scan-out buffer here, before anything is drawn into them.
     *
     * The DSI bridge starts reading these PSRAM rectangles the moment the panel
     * is initialised - i.e. before LVGL owns the screen - so until the first
     * refresh the glass is showing whatever the allocator left in that memory.
     * That is the bright, light-blue flash that appears on a fresh boot and,
     * much more visibly, on every reboot of the crash loop: uninitialised PSRAM
     * plus the bridge's cyan substitution colour.  Writing black first makes the
     * panel come up black instead, and it is the same memory LVGL paints into
     * afterwards, so nothing else has to change.
     *
     * The write goes through the data cache, and the bridge reads PSRAM, so the
     * dirty lines have to be written back or the DMA keeps seeing the old
     * contents.  A rejected msync only costs the (cosmetic) black frame, so it
     * is logged and not treated as fatal. */
    for (int i = 0; i < s_frame_buffer_count; i++) {
        memset(s_frame_buffers[i], 0, s_frame_buffer_bytes);
        const esp_err_t sync = esp_cache_msync(s_frame_buffers[i], s_frame_buffer_bytes,
                                               ESP_CACHE_MSYNC_FLAG_DIR_C2M);
        if (sync != ESP_OK) {
            ESP_LOGW(TAG_DISPLAY, "Framebuffer %d blackening not written back: %s", i, esp_err_to_name(sync));
        }
    }
#endif
    ESP_LOGI(TAG_DISPLAY, "Panel framebuffers: %d x %u bytes (direct capture available, cleared to black)",
        s_frame_buffer_count, (unsigned)s_frame_buffer_bytes);
}

int display_frame_buffer_count(void)
{
    return s_frame_buffer_count;
}

void *display_frame_buffer_get(int index, size_t *out_bytes)
{
    if (out_bytes != NULL) {
        *out_bytes = 0;
    }
    if (index < 0 || index >= s_frame_buffer_count) {
        return NULL;
    }
    if (out_bytes != NULL) {
        *out_bytes = s_frame_buffer_bytes;
    }
    return s_frame_buffers[index];
}

static int display_clamp_hour(int hour)
{
    if (hour < 0) {
        return 0;
    }
    if (hour > 23) {
        return 23;
    }
    return hour;
}

void display_get_power_config(display_power_config_t *out)
{
    if (out == NULL) {
        return;
    }
    out->active_brightness_percent = s_active_brightness;
    out->dim_brightness_percent = s_dim_brightness;
    out->dim_timeout_ms = s_dim_timeout_ms;
    out->off_timeout_ms = s_off_timeout_ms;
    out->night_mode_enabled = s_night_mode_enabled;
    out->night_start_hour = s_night_start_hour;
    out->night_end_hour = s_night_end_hour;
}

void display_set_power_config(const display_power_config_t *cfg)
{
    if (cfg == NULL) {
        return;
    }
    s_active_brightness = display_clamp_brightness(cfg->active_brightness_percent);
    s_dim_brightness = display_clamp_brightness(cfg->dim_brightness_percent);
    s_dim_timeout_ms = cfg->dim_timeout_ms;
    s_off_timeout_ms = cfg->off_timeout_ms;
    s_night_mode_enabled = cfg->night_mode_enabled;
    s_night_start_hour = display_clamp_hour(cfg->night_start_hour);
    s_night_end_hour = display_clamp_hour(cfg->night_end_hour);
    display_power_config_save();
    if (!s_display_ready) {
        return;
    }
    /* Treat a config change as user activity: apply the active brightness and
     * re-arm the inactivity timer with the new timeouts. */
    display_note_activity();
}

/* ------------------------------------------------------------------ */
/* NVS persistence (self-contained in the display driver)              */
/* ------------------------------------------------------------------ */
static void display_power_config_load(void)
{
    nvs_handle_t handle;
    if (nvs_open(DISPLAY_NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) {
        return;
    }

    display_power_nvs_t stored = {0};
    size_t len = sizeof(stored);
    esp_err_t err = nvs_get_blob(handle, DISPLAY_NVS_KEY_POWER, &stored, &len);
    nvs_close(handle);

    if (err != ESP_OK || len != sizeof(stored) || stored.magic != DISPLAY_NVS_MAGIC ||
        stored.version != DISPLAY_NVS_VERSION) {
        return; /* first boot or incompatible layout: keep compile-time defaults */
    }

    s_active_brightness = display_clamp_brightness((int)stored.active_percent);
    s_dim_brightness = display_clamp_brightness((int)stored.dim_percent);
    s_dim_timeout_ms = stored.dim_timeout_ms;
    s_off_timeout_ms = stored.off_timeout_ms;
    s_night_mode_enabled = stored.night_enabled != 0U;
    s_night_start_hour = display_clamp_hour((int)stored.night_start_hour);
    s_night_end_hour = display_clamp_hour((int)stored.night_end_hour);
}

static void display_power_config_save(void)
{
    nvs_handle_t handle;
    if (nvs_open(DISPLAY_NVS_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK) {
        return;
    }

    display_power_nvs_t stored = {
        .magic = DISPLAY_NVS_MAGIC,
        .version = DISPLAY_NVS_VERSION,
        .active_percent = s_active_brightness,
        .dim_percent = s_dim_brightness,
        .dim_timeout_ms = s_dim_timeout_ms,
        .off_timeout_ms = s_off_timeout_ms,
        .night_enabled = s_night_mode_enabled ? 1U : 0U,
        .night_start_hour = s_night_start_hour,
        .night_end_hour = s_night_end_hour,
    };
    if (nvs_set_blob(handle, DISPLAY_NVS_KEY_POWER, &stored, sizeof(stored)) == ESP_OK) {
        storage_guard_flash_op_begin(FLASH_OP_NVS);
        (void)nvs_commit(handle);
        storage_guard_flash_op_end(FLASH_OP_NVS);
    }
    nvs_close(handle);
}

/* ------------------------------------------------------------------ */
/* Inactivity state machine (periodic evaluator)                       */
/* ------------------------------------------------------------------ */
static bool display_night_window_active(void)
{
    if (!s_night_mode_enabled) {
        return false;
    }
    time_t now = 0;
    struct tm info = {0};
    time(&now);
    localtime_r(&now, &info);
    if (info.tm_year < (2016 - 1900)) {
        return false; /* clock not synced yet: never force the screen off */
    }

    const int hour = info.tm_hour;
    if (s_night_start_hour == s_night_end_hour) {
        return false;
    }
    if (s_night_start_hour < s_night_end_hour) {
        return hour >= s_night_start_hour && hour < s_night_end_hour;
    }
    return hour >= s_night_start_hour || hour < s_night_end_hour;
}

static int display_power_target_brightness(void)
{
    if (s_last_activity_us == 0) {
        return s_active_brightness;
    }
    const uint32_t elapsed_ms = (uint32_t)((esp_timer_get_time() - s_last_activity_us) / 1000LL);

    if (s_dim_timeout_ms != 0U && elapsed_ms < s_dim_timeout_ms) {
        return s_active_brightness;
    }

    /* Inactive: go OFF during the night window, otherwise dim first. */
    if (display_night_window_active()) {
        return 0;
    }
    if (s_off_timeout_ms != 0U && elapsed_ms >= s_off_timeout_ms) {
        return 0;
    }
    return s_dim_brightness;
}

static void display_power_timer_cb(void *arg)
{
    (void)arg;
    if (!s_display_ready || !s_power_policy_enabled) {
        return;
    }
    (void)display_set_brightness_percent(display_power_target_brightness());
}

static esp_err_t display_power_timer_init(void)
{
    if (s_power_timer != NULL) {
        return ESP_OK;
    }

    const esp_timer_create_args_t timer_args = {
        .callback = display_power_timer_cb,
        .arg = NULL,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "display_power",
        .skip_unhandled_events = true,
    };
    esp_err_t err = esp_timer_create(&timer_args, &s_power_timer);
    if (err != ESP_OK) {
        return err;
    }
    return esp_timer_start_periodic(s_power_timer, DISPLAY_POWER_EVAL_PERIOD_US);
}

void display_set_active_brightness_percent(int percent)
{
    s_active_brightness = display_clamp_brightness(percent);
}

void display_set_power_policy_enabled(bool enabled)
{
    s_power_policy_enabled = enabled;
    if (!enabled) {
        /* Hand the backlight over to the shared screensaver: stop counting
         * inactivity here so this policy cannot dim behind its back. */
        s_last_activity_us = 0;
    }
}

void display_note_activity(void)
{
    display_note_activity_from("internal");
}

void display_note_activity_from(const char *source)
{
    if (!s_display_ready) {
        return;
    }
    const int before = s_display_brightness;
    /* Real activity means someone is in front of the panel: if the boot splash is
     * somehow still up, the gated request has to land now rather than wait for a
     * hand-over that may never come.  ("internal" is the splash and the saver
     * talking to themselves, which must not end the boot hold.) */
    if (!s_boot_gate_open && source != NULL && strcmp(source, "internal") != 0) {
        display_boot_brightness_release();
    }
    /* While the boot splash is up the UI cannot change the backlight - the level
     * it asked for is waiting in the gate - so this path must not queue a second
     * request behind it. */
    const bool booting = !s_boot_gate_open;
    s_last_activity_us = esp_timer_get_time();

    /* Notify first, brighten second.  Whatever reacts to activity (the
     * screensaver) has to get the light clock wallpaper off the glass *before*
     * the panel gets brighter: raising the level first lit that wallpaper up at
     * menu brightness for as long as the overlay took to disappear, which is the
     * "screen flashes light blue" report.  A callback that returns true owns the
     * backlight for this wake and the driver stays out of the way. */
    bool owned = false;
    if (s_activity_cb != NULL) {
        owned = s_activity_cb();
    }
    if (!owned) {
        if (!booting) {
            (void)display_set_brightness_percent(s_active_brightness);
        }
    }
    /* The first real activity after boot is the splash handing the menu over:
     * apply the brightness the UI asked for while it was up, as one transition. */
    display_boot_brightness_release();
    /* Only a wake is worth a line: ordinary activity fires on every touch, and
     * "something woke the screen" is exactly the case that is otherwise
     * impossible to attribute (the saver dims, then the panel comes back at full
     * brightness with nothing in the log to say why). */
    if (before >= 0 && before != s_active_brightness) {
        const char *who = (source != NULL && source[0] != '\0') ? source : "unknown";
        if (owned) {
            ESP_LOGI(TAG_DISPLAY, "panel woken by %s (backlight %d%%, saver restores %d%%)",
                     who, before, s_active_brightness);
            system_log_event("backlight", "woken by %s (%d%%, saver restores %d%%)",
                             who, before, s_active_brightness);
        } else {
            ESP_LOGI(TAG_DISPLAY, "panel woken by %s (backlight %d%% -> %d%%)",
                     who, before, s_display_brightness);
            system_log_event("backlight", "woken by %s (%d%% -> %d%%)", who, before, s_display_brightness);
        }
    }
}

void display_set_activity_callback(display_activity_cb_t cb)
{
    s_activity_cb = cb;
}

int64_t display_ms_since_activity(void)
{
    if (!s_display_ready || s_last_activity_us == 0) {
        return 0;
    }
    return (esp_timer_get_time() - s_last_activity_us) / 1000;
}

esp_err_t display_init(void)
{
    if (s_display_ready) {
        return ESP_OK;
    }

    lvgl_port_cfg_t lvgl_cfg = display_port_cfg();
    esp_err_t err = lvgl_port_init(&lvgl_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG_DISPLAY, "lvgl_port_init failed: %s", esp_err_to_name(err));
        return err;
    }

    bsp_lcd_handles_t lcd = {0};
    err = bsp_display_new_with_handles(NULL, &lcd);
    if (err != ESP_OK || lcd.panel == NULL) {
        ESP_LOGE(TAG_DISPLAY, "bsp_display_new_with_handles failed: %s", esp_err_to_name(err));
        return (err == ESP_OK) ? ESP_FAIL : err;
    }

    /* Cache (and blacken) the scan-out buffers before anything else touches the
     * glass: the bridge is already streaming them, so every microsecond spent
     * here is a microsecond of uninitialised PSRAM on the panel. */
    s_panel = lcd.panel;
    display_frame_buffer_cache();

    err = esp_lcd_panel_disp_on_off(lcd.panel, true);
    if (err != ESP_OK) {
        ESP_LOGW(TAG_DISPLAY, "Could not enable LCD panel output: %s", esp_err_to_name(err));
    }

    display_power_config_load();
    ESP_LOGI(TAG_DISPLAY,
        "Power config: active=%d%% dim=%d%% dim_after=%" PRIu32 "ms off_after=%" PRIu32 "ms night=%s (%d:00-%d:00)",
        s_active_brightness, s_dim_brightness, s_dim_timeout_ms, s_off_timeout_ms,
        s_night_mode_enabled ? "on" : "off", s_night_start_hour, s_night_end_hour);

    err = display_set_brightness_percent(s_active_brightness);
    if (err != ESP_OK) {
        ESP_LOGW(TAG_DISPLAY, "Could not enable backlight: %s", esp_err_to_name(err));
    }

    /* The boot splash owns the glass from here on (and the saved level above is
     * the one it is meant to come up at): hold every further brightness request
     * until the splash hands the screen over, so the panel lights up once. */
    display_boot_gate_close();

    err = display_power_timer_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG_DISPLAY, "Could not create display power timer: %s", esp_err_to_name(err));
    }

    lvgl_port_display_cfg_t disp_cfg = {
        .io_handle = lcd.io,
        .panel_handle = lcd.panel,
        .control_handle = lcd.control,
        .buffer_size = DISPLAY_FULL_BUFFER_PIXELS / 5U,
        .double_buffer = true,
        .hres = APP_SCREEN_WIDTH,
        .vres = APP_SCREEN_HEIGHT,
        .monochrome = false,
        .rotation = {
            .swap_xy = false,
            .mirror_x = true,
            .mirror_y = true,
        },
#if LV_VERSION_MAJOR >= 9
        .color_format = LV_COLOR_FORMAT_RGB565,
#endif
        .flags = {
            .buff_dma = true,
            .buff_spiram = true,
            .sw_rotate = true,
#if LV_VERSION_MAJOR >= 9
            .swap_bytes = (BSP_LCD_BIGENDIAN ? true : false),
#endif
            .full_refresh = false,
            .direct_mode = false,
        },
    };

    const lvgl_port_display_dsi_cfg_t dsi_cfg = {
        .flags = {
            .avoid_tearing = false,
        },
    };

    static const uint8_t draw_buf_divisors[] = {5U, 8U, 10U, 12U};
    uint8_t used_divisor = 0U;
    uint32_t used_buffer_pixels = 0U;
    for (size_t i = 0; i < (sizeof(draw_buf_divisors) / sizeof(draw_buf_divisors[0])); i++) {
        uint8_t divisor = draw_buf_divisors[i];
        if (divisor == 0U) {
            continue;
        }
        disp_cfg.buffer_size = DISPLAY_FULL_BUFFER_PIXELS / divisor;
        s_lv_display = lvgl_port_add_disp_dsi(&disp_cfg, &dsi_cfg);
        if (s_lv_display != NULL) {
            used_divisor = divisor;
            used_buffer_pixels = disp_cfg.buffer_size;
            break;
        }
        ESP_LOGW(TAG_DISPLAY, "lvgl_port_add_disp_dsi failed with draw_buf=1/%u (%u px), trying smaller buffer",
            (unsigned)divisor, (unsigned)disp_cfg.buffer_size);
    }

    if (s_lv_display == NULL) {
        ESP_LOGE(TAG_DISPLAY, "lvgl_port_add_disp_dsi failed");
        return ESP_FAIL;
    }

    if (lvgl_port_lock(2000)) {
        lv_display_set_antialiasing(s_lv_display, APP_LVGL_ANTIALIASING != 0);
        /* Panel 7B is mounted upside-down; EK79007 MADCTL mirror has no effect
         * in DSI video mode, so rotate 180 in software (matches GT911 mirror). */
        lv_display_set_rotation(s_lv_display, LV_DISPLAY_ROTATION_180);
        if (s_trace_timer == NULL) {
            s_trace_timer = lv_timer_create(display_trace_timer_cb, APP_LVGL_TRACE_PERIOD_MS, NULL);
            if (s_trace_timer == NULL) {
                ESP_LOGW(TAG_DISPLAY, "LVGL trace timer not created: freeze reports lose lvcb/lvhand");
            }
        }
        /* Render diagnostics: RENDER_START/RENDER_READY time each refresh pass and
         * LV_EVENT_INVALIDATE_AREA tracks how much of the screen it covers. */
        lv_display_add_event_cb(s_lv_display, display_render_event_cb, LV_EVENT_INVALIDATE_AREA, NULL);
        lv_display_add_event_cb(s_lv_display, display_render_event_cb, LV_EVENT_RENDER_START, NULL);
        lv_display_add_event_cb(s_lv_display, display_render_event_cb, LV_EVENT_RENDER_READY, NULL);
        lvgl_port_unlock();
    } else {
        ESP_LOGW(TAG_DISPLAY, "Could not lock LVGL for rotation setup");
        lv_display_set_antialiasing(s_lv_display, APP_LVGL_ANTIALIASING != 0);
    }
    ESP_LOGI(TAG_DISPLAY, "LVGL antialiasing: %s", (APP_LVGL_ANTIALIASING != 0) ? "on" : "off");

    s_display_ready = true;
    ESP_LOGI(TAG_DISPLAY,
        "Display initialized (esp_lvgl_port + DSI, avoid_tearing=0, direct_mode=0, double_buffer=1, draw_buf=1/%u, %u px)",
        (unsigned)used_divisor, (unsigned)used_buffer_pixels);
    display_note_activity();
    return ESP_OK;
}

bool display_is_ready(void)
{
    return s_display_ready;
}

bool display_lock(uint32_t timeout_ms)
{
    bool locked = lvgl_port_lock(timeout_ms);
    if (locked) {
        system_log_lock_acquire("lvgl");
    }
    return locked;
}

void display_unlock(void)
{
    system_log_lock_release("lvgl");
    lvgl_port_unlock();
}

void display_force_invalidate(void)
{
    if (!s_display_ready) {
        return;
    }
    if (display_lock(500)) {
        lv_obj_invalidate(lv_screen_active());
        display_unlock();
    }
}
