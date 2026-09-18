/* SPDX-License-Identifier: LicenseRef-FNCL-1.1
 * Copyright (c) 2026 Cpt_Kirk
 *
 * Screen-flash detector - see display_flash_watch.h for the rationale.
 *
 * The detector reads the panel's own DSI scan-out framebuffers directly.  It
 * deliberately does *not* take the LVGL or display lock and does not use
 * lv_snapshot_take(): a probe that re-renders the screen would change what it
 * is trying to measure.
 */

#include "diag/display_flash_watch.h"

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "app_config.h"

#if defined(CONFIG_APP_PANEL_VARIANT_7INCH_1024)

#include "esp_log.h"
#include "esp_timer.h"

#if defined(CONFIG_IDF_TARGET_ESP32P4) && defined(CONFIG_BSP_DISPLAY_BRIGHTNESS_LEDC_CH)
#include "driver/ledc.h"
#define FW_HAS_LEDC_DUTY 1
#else
#define FW_HAS_LEDC_DUTY 0
#endif

#include "diag/system_log.h"
#include "drivers/display_init.h"
#include "ui/ui_pages.h"

static const char *TAG = "flashwatch";

/* ---- Sampling grid ----------------------------------------------------- */
/* 16 x 15 = 240 points per framebuffer, read as 32-bit words (two RGB565
 * pixels each) so the whole pass is ~480 reads - small enough to keep PSRAM
 * bandwidth (shared with the DSI scan-out) essentially untouched. */
#define FW_COLS 16
#define FW_ROWS 15
#define FW_SAMPLES (FW_COLS * FW_ROWS)
#define FW_STEP_X 64
#define FW_STEP_Y 40
#define FW_FIRST_X 16
#define FW_FIRST_Y 20

/* ---- Fast pass --------------------------------------------------------- */
/* The grid above is read every FW_PERIOD_MS, so a wash that lasts a single frame
 * (16.7 ms) can sit between two of its samples and never be counted: the flash is
 * then invisible to this whole file even though the eye sees it.  The fast pass
 * reads a coarse grid every few milliseconds and compares it against the pass
 * immediately before, so the shortest event it can miss is a fraction of a frame
 * instead of a whole one.  It only reacts when a large part of the screen moves
 * at once, so ordinary tile updates do not fill the log with noise. */
#define FW_FAST_COLS 8
#define FW_FAST_ROWS 6
#define FW_FAST_SAMPLES (FW_FAST_COLS * FW_FAST_ROWS)
#define FW_FAST_STEP_X 128
#define FW_FAST_STEP_Y 100
#define FW_FAST_FIRST_X 32
#define FW_FAST_FIRST_Y 40
#define FW_FAST_PERIOD_MS 4
/* Fine-pass cadence: FW_FAST_PERIOD_MS * FW_FAST_EVERY must stay FW_PERIOD_MS. */
#define FW_FAST_EVERY 10
/* Lower than the fine thresholds on purpose: this compares two samples a few
 * milliseconds apart instead of two settled states, so a smaller move is already
 * a full-screen event and anything bigger would only be missed.
 *
 * The move is measured per colour channel (see fw_report_move): a colour change
 * can be large while the mean luma barely moves, so a luma-only threshold would
 * drop the exact events this detector exists for. */
#define FW_FAST_LUMA_JUMP 12
#define FW_FAST_CELL_JUMP 12
#define FW_FAST_CELL_PCT 25
/* One event is logged per hold window: a flash that lasts three frames is one
 * flash, not six (two framebuffers x three samples). */
#define FW_FAST_HOLD_MS 500
#define FW_FAST_HISTORY 8
#define FW_FAST_TEXT_LEN 140

/* Slow pass: the same grid, compared against a sample one second old.  The fast
 * pass only fires on a jump between two 4 ms reads, so a change that arrives in
 * steps - a fade, a page turn, a wallpaper swap - would pass it by; this one
 * catches the end state of such a change wherever it came from. */
#define FW_SLOW_PERIOD_MS 1000
#define FW_SLOW_HOLD_MS 1500
/* Every how many fast iterations the slow pass runs; must stay
 * FW_SLOW_PERIOD_MS / FW_FAST_PERIOD_MS. */
#define FW_SLOW_EVERY (FW_SLOW_PERIOD_MS / FW_FAST_PERIOD_MS)

/* ---- Detection thresholds --------------------------------------------- */
/* A jump of 22 luma is unmistakable: the UI palette lives in a range of about
 * 30..70 luma, so this only fires on a real wash-out or a full-screen swap. */
#define FW_LUMA_JUMP 22
#define FW_RETURN_TOL 12
#define FW_RETURN_MS 1500
#define FW_LIGHT_LUMA 150
#define FW_LIGHT_BRIGHT_PCT 70
#define FW_DARK_LUMA 25
#define FW_DARK_BRIGHT_PCT 3
#define FW_REPORT_COOLDOWN_MS 800
#define FW_GAP_WARN_MS 200

/* ---- Partial-wash detection -------------------------------------------- */
/* The reported "light blue flash" does not move the average luma much: only part
 * of the screen gets washed out, so the average stays inside the tolerance of the
 * tracker above and nothing was ever logged.  Per-cell luma is tracked as well
 * and a coverage trigger sits next to the average-luma one: many grid points
 * brighten at once and fall back within a fraction of a second. */
#define FW_CELL_JUMP 14
/* 40 % rather than 55 %: the panel repaints a 1024x600 change in 1/5-screen
 * stripes, so a transition that takes several samples only ever brightens part
 * of the grid within any single one of them - which is why this trigger, tuned
 * for a one-frame wash, never fired on the flashes the user reported. */
#define FW_WASH_CELL_PCT 40
#define FW_WASH_END_PCT 18
#define FW_WASH_RETURN_MS 900

/* Nothing is evaluated for the first seconds after start: the framebuffer is
 * still black before the UI paints, which the tracker read as a jump to "stuck
 * dark" on every boot (27 identical reports in a single log history). */
#define FW_BOOT_IGNORE_MS 6000
/* Longest "last event" text carried in the always-visible health line. */
#define FW_EVENT_TEXT_LEN 96

#define FW_PERIOD_MS 40
#define FW_DISABLED_PERIOD_MS 500

#define FW_NOTE_COUNT 8
#define FW_NOTE_LEN 72

typedef struct {
    int luma;
    int r;
    int g;
    int b;
    int bright_pct;
    int sat;
} fb_stats_t;

typedef struct {
    bool valid;
    fb_stats_t st;
    /* A jump has been seen and is being watched for a return to the previous
     * brightness (transient = flash) or for a washed-out steady state. */
    bool pending;
    int64_t pending_ms;
    int dir; /* +1 the screen went brighter, -1 darker */
    fb_stats_t base;
    fb_stats_t peak;
    int64_t peak_ms;
    /* Per-cell luma of the previous sample, for partial washes. */
    uint8_t cells[FW_SAMPLES];
    bool cells_valid;
    bool wash_pending;
    int64_t wash_ms;
    int wash_peak_pct;
    int wash_base_luma;
    int wash_peak_luma;
    /* Mean colour of the grid points that brightened, sampled at peak coverage:
     * this is the only place a flash's *colour* is recorded, and the colour is
     * what names the culprit (45,118,167 = the blue screensaver wallpaper). */
    int wash_r;
    int wash_g;
    int wash_b;
} fb_track_t;

typedef struct {
    int64_t ms;
    char text[FW_NOTE_LEN];
} fw_note_t;

/* A backlight fade moves the PWM duty every 20 ms, so a brightness change is
 * only reported once the value has stayed put - that folds one 100%->45% fade
 * into a single log line instead of one per step. */
#define FW_BRIGHT_STABLE_MS 120

/* A backlight rise of at least this much *while the light clock wallpaper is
 * still on the glass* is a flash even though the framebuffer did not change: the
 * panel simply got brighter with the light content still up.  This is the one
 * class of flash that no pixel comparison can ever see, and it is exactly the
 * "screen flashes light blue" the user reported on every wake from the clock. */
#define FW_BACKLIGHT_FLASH_PCT 15

static TaskHandle_t s_task;
static volatile bool s_enabled = true;
static volatile bool s_started;

static fb_track_t s_track[DISPLAY_FRAME_BUFFER_MAX];
static int s_fb_count;

static portMUX_TYPE s_note_mux = portMUX_INITIALIZER_UNLOCKED;
static fw_note_t s_notes[FW_NOTE_COUNT];
static int s_note_next;

static uint32_t s_flash_count;
static uint32_t s_wash_count;
static uint32_t s_change_count;
static uint32_t s_sample_count;
static uint32_t s_gap_count;
static uint32_t s_suppressed;
static int64_t s_last_report_ms = -100000;

/* Backlight (brightness-only) tracking: the framebuffer grid cannot see a
 * brightness change, which is why a whole class of screen changes used to be
 * invisible to this detector. */
static uint32_t s_brightness_count;
static int s_bl_reported_pct = -1;
static int s_bl_seen_pct = -1;
static int64_t s_bl_stable_ms;
/* Set by the screensaver while its light wallpaper is on the glass. */
static volatile bool s_light_content;

/* A backlight raise asked for while that wallpaper is up is held here instead of
 * executed - see display_flash_watch_hold_raise().  Raising the panel with the
 * light frame still up lights that frame up at the new level, which the user
 * sees as a light-blue flash even though not one pixel changed. */
static volatile int s_held_raise_pct = -1;   /* -1 = nothing held */
static esp_timer_handle_t s_held_raise_timer;
static uint32_t s_held_raise_count;

/* Bounded release: a stale "light content" flag must never leave the panel at
 * the saver level for good. */
#define FW_HELD_RAISE_MAX_MS 2000
#define FW_HELD_RAISE_FADE_MS 350

/* ---- Render-pass witness -------------------------------------------------
 *
 * Whether "the menu is painted by now" cannot be decided by a timer: LVGL paints
 * when it gets to run, and the 1024x600 pass that replaces the saver wallpaper
 * takes ~165 ms measured on this panel.  A fixed 250 ms delay therefore lands
 * *inside* that pass whenever the LVGL task was busy (a camera frame, an SD
 * write), and the brighten then ramps the light-blue wallpaper up to menu level
 * before the menu is on the glass - the flash the user reports on every wake.
 * The display driver feeds every refresh pass in here, so the start time of the
 * last full pass is proof that the glass no longer shows the light wallpaper. */
static volatile int64_t s_last_full_paint_start_us;
static volatile uint32_t s_full_paint_count;
static volatile uint32_t s_early_raise_count;   /* raises that beat the paint */

/* Hardware backlight witness: the PWM duty as the LEDC peripheral holds it.
 * Every software path goes through display_set_brightness_percent(), which the
 * brightness tracker above already accounts for, so a duty that moves while that
 * level is settled is a change nothing in this firmware asked for - and it is
 * the only way to tell "the backlight glitched" apart from "the glass was fed a
 * different picture", the two halves of every remaining flash theory. */
static uint32_t s_hw_duty = 0xFFFFFFFFu;
static uint32_t s_hw_duty_changes;
static uint32_t s_hw_duty_from;
static uint32_t s_hw_duty_to;
static int64_t s_hw_duty_last_ms = -100000;
static int64_t s_hw_duty_last_log_ms = -100000;

/* Scratch buffers kept off the task stack: the flashwatch task runs with a
 * small stack (the ESP_LOG* below go through vfprintf, which is stack-hungry),
 * and these two are only ever touched from that task. */
static char s_report_notes[FW_NOTE_COUNT * (FW_NOTE_LEN + 16)];
static uint8_t s_sample_cells[FW_SAMPLES];
static uint16_t s_sample_px[FW_SAMPLES];

/* Last reported event, so the health line keeps naming the newest flash (with
 * its colour) long after the log ring has rotated past the WARN line. */
static portMUX_TYPE s_event_mux = portMUX_INITIALIZER_UNLOCKED;
static int64_t s_last_event_ms;
static char s_last_event_text[FW_EVENT_TEXT_LEN];

/* ---- Fast-pass state --------------------------------------------------- */
/* Per framebuffer: the coarse grid as it was read one pass ago.  The fast pass
 * reports what moved between two of these, which is the only way a single-frame
 * wash can be seen at all.
 *
 * Channels are kept next to luma because the worst screen changes on this panel
 * are *not* brightness changes: the menu and the saver's dimmed blue wallpaper
 * have almost the same mean luma (measured live: 50 -> 55 while the screen went
 * from dark menu to blue clock), so a luma-only test is structurally blind to the
 * very change the user describes as "the screen flashes light blue". */
typedef struct {
    bool valid;
    int luma;
    int r;
    int g;
    int b;
    uint8_t cells[FW_FAST_SAMPLES];
    uint8_t cell_r[FW_FAST_SAMPLES];
    uint8_t cell_g[FW_FAST_SAMPLES];
    uint8_t cell_b[FW_FAST_SAMPLES];
} fw_fast_t;

typedef struct {
    int64_t ms;
    int fb;
    int kind;   /* 0 = fast pass (a jump), 1 = slow pass (a change built up over ~1 s) */
    int dir;
    int luma_from;
    int luma_to;
    int r_from;     /* mean colour of the whole grid before/after, which is what */
    int g_from;     /* separates "the menu went blue" from "the backlight rose" */
    int b_from;
    int r_to;
    int g_to;
    int b_to;
    int move;       /* the largest per-channel move, the number that fired */
    char chan;      /* which channel that was: 'r', 'g' or 'b' */
    int coverage_pct;
    int r;
    int g;
    int b;
    uint32_t duty;
    int brightness_pct;
    int light;
    uint32_t passes;
} fw_fast_event_t;

static fw_fast_t s_fast[DISPLAY_FRAME_BUFFER_MAX];
static fw_fast_t s_slow[DISPLAY_FRAME_BUFFER_MAX];
static uint32_t s_fast_passes;
static uint32_t s_fast_bright;
static uint32_t s_fast_dark;
static int64_t s_fast_hold_ms = -100000;
static uint32_t s_slow_passes;
static uint32_t s_slow_bright;
static uint32_t s_slow_dark;
static int64_t s_slow_hold_ms = -100000;

/* What is on the glass right now, as the fast grid sees it: the mean luma, the
 * extremes and the mean colour of the first framebuffer.  Printed by the health
 * line, which turns the periodic log itself into a trace of the screen - a saver
 * coming up, a page swap, a fade or a wash-out all show as a step in these
 * numbers, minutes after the fact and without anyone watching the panel. */
static volatile int s_glass_luma = -1;
static volatile int s_glass_min = -1;
static volatile int s_glass_max = -1;
static volatile int s_glass_r = -1;
static volatile int s_glass_g = -1;
static volatile int s_glass_b = -1;

/* Kept under a lock because display_flash_watch_get_fast() is read from the HTTP
 * task: the history is what the user hands back after leaving the panel running,
 * so it must be readable while the flashwatch task keeps sampling. */
static portMUX_TYPE s_fast_mux = portMUX_INITIALIZER_UNLOCKED;
static fw_fast_event_t s_fast_events[FW_FAST_HISTORY];
static int s_fast_next;

static int64_t fw_now_ms(void)
{
    return esp_timer_get_time() / 1000;
}

/* ---- Framebuffer statistics ------------------------------------------- */

static bool sample_stats(const uint8_t *fb, size_t bytes, fb_stats_t *out, uint8_t *cells,
                         uint16_t *px_out)
{
    const size_t stride = (size_t)APP_SCREEN_WIDTH * 2U;
    const size_t last_x = (size_t)(FW_FIRST_X + (FW_COLS - 1) * FW_STEP_X);
    const size_t last_y = (size_t)(FW_FIRST_Y + (FW_ROWS - 1) * FW_STEP_Y);

    memset(out, 0, sizeof(*out));
    if (cells != NULL) {
        memset(cells, 0, FW_SAMPLES);
    }
    if (px_out != NULL) {
        memset(px_out, 0, FW_SAMPLES * sizeof(px_out[0]));
    }
    if ((last_x + 1U) * 2U + sizeof(uint32_t) > stride) {
        return false;
    }
    if ((last_y + 1U) * stride > bytes) {
        return false;
    }

    long luma_sum = 0;
    long r_sum = 0;
    long g_sum = 0;
    long b_sum = 0;
    long sat_sum = 0;
    int bright = 0;
    int taken = 0;

    for (int row = 0; row < FW_ROWS; row++) {
        const uint8_t *line = fb + (size_t)(FW_FIRST_Y + row * FW_STEP_Y) * stride;
        for (int col = 0; col < FW_COLS; col++) {
            const size_t x = (size_t)(FW_FIRST_X + col * FW_STEP_X);
            uint32_t word;
            memcpy(&word, line + x * 2U, sizeof(word));
            for (int half = 0; half < 2; half++) {
                const uint16_t px = (uint16_t)((word >> (half * 16)) & 0xFFFFU);
                const int r5 = (px >> 11) & 0x1F;
                const int g6 = (px >> 5) & 0x3F;
                const int b5 = px & 0x1F;
                const int r = (r5 * 255 + 15) / 31;
                const int g = (g6 * 255 + 31) / 63;
                const int b = (b5 * 255 + 15) / 31;
                const int luma = (77 * r + 150 * g + 29 * b) >> 8;
                const int mx = (r > g) ? ((r > b) ? r : b) : ((g > b) ? g : b);
                const int mn = (r < g) ? ((r < b) ? r : b) : ((g < b) ? g : b);
                luma_sum += luma;
                r_sum += r;
                g_sum += g;
                b_sum += b;
                sat_sum += mx - mn;
                if (luma >= 140) {
                    bright++;
                }
                if (half == 0) {
                    const int idx = row * FW_COLS + col;
                    if (cells != NULL) {
                        cells[idx] = (uint8_t)(luma > 255 ? 255 : luma);
                    }
                    if (px_out != NULL) {
                        px_out[idx] = px;
                    }
                }
                taken++;
            }
        }
    }

    if (taken == 0) {
        return false;
    }
    out->luma = (int)(luma_sum / taken);
    out->r = (int)(r_sum / taken);
    out->g = (int)(g_sum / taken);
    out->b = (int)(b_sum / taken);
    out->bright_pct = (bright * 100) / taken;
    out->sat = (int)(sat_sum / taken);
    return true;
}

/* ---- Fast pass sampling ------------------------------------------------ */

static void fw_px_rgb(uint16_t px, int *r, int *g, int *b)
{
    *r = ((px >> 11) & 0x1F) * 255 / 31;
    *g = ((px >> 5) & 0x3F) * 255 / 63;
    *b = (px & 0x1F) * 255 / 31;
}

static int fw_px_luma(int r, int g, int b)
{
    return (77 * r + 150 * g + 29 * b) >> 8;
}

/* Coarse pass: FW_FAST_SAMPLES points, one 16-bit pixel each, so the whole thing
 * is ~48 reads per framebuffer - cheap enough to run every few milliseconds on a
 * bus shared with the DSI scan-out.  The pixels are kept (px_out) because the
 * colour of the points that moved is what names the culprit; the grid and the
 * per-channel means are kept so the next pass can compare against them. */
static bool sample_fast(const uint8_t *fb, size_t bytes, fw_fast_t *out, uint16_t *px_out)
{
    const size_t stride = (size_t)APP_SCREEN_WIDTH * 2U;
    const size_t last_x = (size_t)(FW_FAST_FIRST_X + (FW_FAST_COLS - 1) * FW_FAST_STEP_X);
    const size_t last_y = (size_t)(FW_FAST_FIRST_Y + (FW_FAST_ROWS - 1) * FW_FAST_STEP_Y);

    if ((last_x + 1U) * 2U > stride) {
        return false;
    }
    if ((last_y + 1U) * stride > bytes) {
        return false;
    }

    long luma_sum = 0;
    long r_sum = 0;
    long g_sum = 0;
    long b_sum = 0;
    for (int row = 0; row < FW_FAST_ROWS; row++) {
        const uint8_t *line = fb + (size_t)(FW_FAST_FIRST_Y + row * FW_FAST_STEP_Y) * stride;
        for (int col = 0; col < FW_FAST_COLS; col++) {
            uint16_t px;
            int r;
            int g;
            int b;
            memcpy(&px, line + (size_t)(FW_FAST_FIRST_X + col * FW_FAST_STEP_X) * 2U, sizeof(px));
            fw_px_rgb(px, &r, &g, &b);
            const int luma = fw_px_luma(r, g, b);
            const int idx = row * FW_FAST_COLS + col;
            out->cells[idx] = (uint8_t)(luma > 255 ? 255 : luma);
            out->cell_r[idx] = (uint8_t)r;
            out->cell_g[idx] = (uint8_t)g;
            out->cell_b[idx] = (uint8_t)b;
            if (px_out != NULL) {
                px_out[idx] = px;
            }
            luma_sum += luma;
            r_sum += r;
            g_sum += g;
            b_sum += b;
        }
    }
    out->luma = (int)(luma_sum / FW_FAST_SAMPLES);
    out->r = (int)(r_sum / FW_FAST_SAMPLES);
    out->g = (int)(g_sum / FW_FAST_SAMPLES);
    out->b = (int)(b_sum / FW_FAST_SAMPLES);
    return true;
}

/* ---- Event notes ------------------------------------------------------- */

void display_flash_watch_note(const char *event)
{
    if (event == NULL || event[0] == '\0') {
        return;
    }

    fw_note_t note;
    note.ms = fw_now_ms();
    snprintf(note.text, sizeof(note.text), "%s", event);

    portENTER_CRITICAL(&s_note_mux);
    s_notes[s_note_next] = note;
    s_note_next = (s_note_next + 1) % FW_NOTE_COUNT;
    portEXIT_CRITICAL(&s_note_mux);
}

void display_flash_watch_note_snapshot(bool active)
{
    display_flash_watch_note(active ? "screenshot:start" : "screenshot:end");
}

static void render_notes(char *out, size_t out_len, int64_t now_ms)
{
    size_t used = 0;
    out[0] = '\0';

    portENTER_CRITICAL(&s_note_mux);
    const int next = s_note_next;
    portEXIT_CRITICAL(&s_note_mux);

    /* Oldest first, so reading the line is chronological. */
    for (int i = 0; i < FW_NOTE_COUNT && used + 1U < out_len; i++) {
        const int idx = (next + i) % FW_NOTE_COUNT;
        fw_note_t snap;
        portENTER_CRITICAL(&s_note_mux);
        snap = s_notes[idx];
        portEXIT_CRITICAL(&s_note_mux);
        if (snap.text[0] == '\0' || snap.ms == 0) {
            continue;
        }
        const int64_t age = now_ms - snap.ms;
        if (age < 0 || age > 8000) {
            continue;
        }
        /* Integer-only formatting on purpose: this runs on the small-stack
         * flashwatch task, and newlib's %f pulls in _dtoa_r, which alone needs
         * more stack than the whole task has. */
        const int n = snprintf(out + used, out_len - used, "%s%lld.%01ds %s",
                               used == 0 ? "[" : "|", (long long)(age / 1000),
                               (int)((age % 1000) / 100), snap.text);
        if (n <= 0) {
            continue;
        }
        used += (size_t)n;
        if (used >= out_len) {
            used = out_len - 1U;
            out[used] = '\0';
        }
    }
    if (used > 0 && used + 1U < out_len) {
        out[used++] = ']';
        out[used] = '\0';
    }
}

/* ---- Last event -------------------------------------------------------- */

/* Kept in a tiny record next to the counters so the health line always names the
 * newest flash - and its colour - even when the ring has already rotated. */
static void fw_last_event_set(const char *kind, int coverage_pct, int base_luma, int peak_luma,
                              int r, int g, int b, int64_t duration_ms)
{
    char text[FW_EVENT_TEXT_LEN];
    const int n = snprintf(text, sizeof(text), "%s cov=%d%% luma %d->%d rgb %d,%d,%d %lldms pg=%s",
                           kind, coverage_pct, base_luma, peak_luma, r, g, b,
                           (long long)duration_ms, ui_pages_current_id());
    if (n <= 0) {
        return;
    }

    portENTER_CRITICAL(&s_event_mux);
    s_last_event_ms = fw_now_ms();
    memcpy(s_last_event_text, text, sizeof(s_last_event_text));
    s_last_event_text[sizeof(s_last_event_text) - 1U] = '\0';
    portEXIT_CRITICAL(&s_event_mux);
}

static bool fw_last_event_get(char *out, size_t out_len, int64_t *age_ms)
{
    if (out == NULL || out_len == 0) {
        return false;
    }

    char text[FW_EVENT_TEXT_LEN];
    portENTER_CRITICAL(&s_event_mux);
    const int64_t stamp = s_last_event_ms;
    memcpy(text, s_last_event_text, sizeof(text));
    portEXIT_CRITICAL(&s_event_mux);

    if (stamp == 0) {
        return false;
    }
    snprintf(out, out_len, "%s", text);

    const int64_t now = fw_now_ms();
    *age_ms = (now > stamp) ? (now - stamp) : 0;
    return out[0] != '\0';
}

/* ---- Reporting --------------------------------------------------------- */

static void report(const fb_track_t *t, int fb_index, int action, int64_t duration_ms,
                   const fb_stats_t *extreme, int64_t gap_ms, int coverage_pct)
{
    const int64_t now = fw_now_ms();
    if (now - s_last_report_ms < FW_REPORT_COOLDOWN_MS) {
        s_suppressed++;
        return;
    }
    s_last_report_ms = now;

    render_notes(s_report_notes, sizeof(s_report_notes), now);

    const char *what = (action == 1) ? "TRANSIENT" : ((action == 2) ? "STUCK-LIGHT" : "STUCK-DARK");
    ESP_LOGW(TAG,
             "SCREEN FLASH %s fb=%d luma %d->%d (peak %d) rgb %d,%d,%d -> %d,%d,%d "
             "bright %d%%->%d%% sat %d->%d dur=%lldms sample_gap=%lldms backlight=%d%% "
             "page=%s flashes=%u washes=%u changes=%u suppressed=%u %s",
             what, fb_index,
             t->base.luma, extreme->luma, extreme->luma,
             t->base.r, t->base.g, t->base.b,
             extreme->r, extreme->g, extreme->b,
             t->base.bright_pct, extreme->bright_pct,
             t->base.sat, extreme->sat,
             (long long)duration_ms, (long long)gap_ms,
             display_get_brightness_percent(), ui_pages_current_id(),
             (unsigned)s_flash_count, (unsigned)s_wash_count, (unsigned)s_change_count, (unsigned)s_suppressed,
             s_report_notes);
    fw_last_event_set(what, coverage_pct, t->base.luma, extreme->luma,
                      extreme->r, extreme->g, extreme->b, duration_ms);
    s_suppressed = 0;
}

/* Colour class of the region that changed.  The panel's own brightest light
 * source is the screensaver wallpaper, so naming the class next to the RGB
 * triple makes the "light blue flash" self-explanatory in the log: a
 * bright-blue change is the clock face appearing, a neutral one is a normal
 * repaint. */
static const char *fw_tone_name(int luma, int r, int g, int b)
{
    (void)g;
    if (b - r >= 25) {
        return (luma >= FW_LIGHT_LUMA) ? "bright-blue" : "blue";
    }
    if (r - b >= 25) {
        return (luma >= FW_LIGHT_LUMA) ? "bright-warm" : "warm";
    }
    return (luma >= FW_LIGHT_LUMA) ? "bright-neutral" : "neutral";
}

/* A partial wash: most of the sampled grid brightened at once and fell back
 * quickly while the average luma barely moved.  This is the "light blue flash"
 * the average-luma tracker cannot see, so it gets its own report. */
static void report_wash(int fb_index, int coverage_pct, int64_t duration_ms,
                        int base_luma, int peak_luma, int r, int g, int b, int64_t gap_ms)
{
    const int64_t now = fw_now_ms();
    if (now - s_last_report_ms < FW_REPORT_COOLDOWN_MS) {
        s_suppressed++;
        return;
    }
    s_last_report_ms = now;

    render_notes(s_report_notes, sizeof(s_report_notes), now);

    display_render_stats_t rs;
    display_render_stats_get(&rs);
    ESP_LOGW(TAG,
             "SCREEN FLASH WASH fb=%d coverage=%d%% luma %d->%d flash_rgb=%d,%d,%d tone=%s dur=%lldms "
             "sample_gap=%lldms backlight=%d%% page=%s render passes=%u full=%u slow=%u "
             "dirty=%u%% last=%ums flashes=%u washes=%u changes=%u suppressed=%u %s",
             fb_index, coverage_pct, base_luma, peak_luma, r, g, b, fw_tone_name(peak_luma, r, g, b),
             (long long)duration_ms,
             (long long)gap_ms, display_get_brightness_percent(), ui_pages_current_id(),
             (unsigned)rs.passes, (unsigned)rs.full_passes, (unsigned)rs.slow_passes,
             (unsigned)rs.last_dirty_pct, (unsigned)rs.last_pass_ms,
             (unsigned)s_flash_count, (unsigned)s_wash_count, (unsigned)s_change_count,
             (unsigned)s_suppressed, s_report_notes);
    fw_last_event_set("WASH", coverage_pct, base_luma, peak_luma, r, g, b, duration_ms);
    s_suppressed = 0;
}

/* A change that brightened the screen and *stayed* bright, or a step that was
 * neither a wash-out nor a blackout.  Both were counted silently until now, and
 * those two paths are exactly the ones the panel takes for the screensaver: the
 * bright blue wallpaper sliding in was the "light blue flash" the user reported,
 * and it never reached the log.  The colour of the brightened cells is the part
 * that identifies it. */
static void report_change(const char *kind, int coverage_pct, int base_luma, int peak_luma,
                          int r, int g, int b, int64_t duration_ms, int64_t gap_ms)
{
    const int64_t now = fw_now_ms();
    if (now - s_last_report_ms < FW_REPORT_COOLDOWN_MS) {
        s_suppressed++;
        return;
    }
    s_last_report_ms = now;

    render_notes(s_report_notes, sizeof(s_report_notes), now);

    display_render_stats_t rs;
    display_render_stats_get(&rs);
    ESP_LOGW(TAG,
             "SCREEN CHANGE %s coverage=%d%% luma %d->%d flash_rgb=%d,%d,%d tone=%s dur=%lldms "
             "sample_gap=%lldms backlight=%d%% page=%s render passes=%u full=%u slow=%u "
             "dirty=%u%% last=%ums flashes=%u washes=%u changes=%u suppressed=%u %s",
             kind, coverage_pct, base_luma, peak_luma, r, g, b, fw_tone_name(peak_luma, r, g, b),
             (long long)duration_ms,
             (long long)gap_ms, display_get_brightness_percent(), ui_pages_current_id(),
             (unsigned)rs.passes, (unsigned)rs.full_passes, (unsigned)rs.slow_passes,
             (unsigned)rs.last_dirty_pct, (unsigned)rs.last_pass_ms,
             (unsigned)s_flash_count, (unsigned)s_wash_count, (unsigned)s_change_count,
             (unsigned)s_suppressed, s_report_notes);
    fw_last_event_set(kind, coverage_pct, base_luma, peak_luma, r, g, b, duration_ms);
    s_suppressed = 0;
}

/* Brightness-only change: no framebuffer involved, so this is deliberately kept
 * out of the coverage/luma reports (and out of their cooldown) - a real flash
 * must never be suppressed by a screensaver dim. Reports once per settled step,
 * e.g. "100%% -> 45%%" for a whole fade.
 *
 * One case is not a mere brightness step: a rise *while the light clock
 * wallpaper is still on the glass* (s_light_content).  Nothing in the pixel
 * domain changes there, which is exactly why "the screen flashes light blue"
 * arrived in the log as a harmless backlight line for weeks; it is reported as a
 * flash instead. */
static void flash_watch_check_backlight(int64_t now)
{
    const int bl = display_get_brightness_percent();
    if (bl < 0) {
        return;
    }
    if (bl != s_bl_seen_pct) {
        s_bl_seen_pct = bl;
        s_bl_stable_ms = now;
        return;
    }
    if (s_bl_reported_pct == bl || now - s_bl_stable_ms < (int64_t)FW_BRIGHT_STABLE_MS) {
        return;
    }

    const int from = s_bl_reported_pct;
    s_bl_reported_pct = bl;
    s_brightness_count++;

    char last[FW_EVENT_TEXT_LEN];
    int64_t age_ms = 0;
    if (!fw_last_event_get(last, sizeof(last), &age_ms)) {
        snprintf(last, sizeof(last), "none");
    }

    if (from >= 0 && bl - from >= FW_BACKLIGHT_FLASH_PCT && s_light_content) {
        s_flash_count++;
        ESP_LOGW(TAG,
                 "SCREEN FLASH backlight=light-content %d%% -> %d%% (+%d%%, clock wallpaper still up) "
                 "page=%s flashes=%u washes=%u changes=%u backlight_changes=%u light=1 last=%s",
                 from, bl, bl - from, ui_pages_current_id(), (unsigned)s_flash_count,
                 (unsigned)s_wash_count, (unsigned)s_change_count, (unsigned)s_brightness_count, last);
        /* For FLASH-BL the luma fields carry the backlight percentages. */
        fw_last_event_set("FLASH-BL", 100, from, bl, 0, 0, 0, 0);
        return;
    }

    ESP_LOGW(TAG,
             "SCREEN BACKLIGHT %d%% -> %d%% (%s) page=%s flashes=%u washes=%u changes=%u "
             "backlight_changes=%u light=%d last=%s",
             from < 0 ? 0 : from, bl, from < 0 ? "initial" : (bl > from ? "brighter" : "dimmer"),
             ui_pages_current_id(), (unsigned)s_flash_count, (unsigned)s_wash_count,
             (unsigned)s_change_count, (unsigned)s_brightness_count, s_light_content ? 1 : 0, last);
    /* No system_log_event() here on purpose: it would push a second copy into
     * the file log next to the WARN above and eat a slot in the flash-watcher
     * note ring. */
}

/* Returns the duty the backlight channel currently holds, or 0xFFFFFFFF when
 * this target has no LEDC backlight to watch. */
static uint32_t fw_backlight_duty(void)
{
#if FW_HAS_LEDC_DUTY
    return ledc_get_duty(LEDC_LOW_SPEED_MODE, (ledc_channel_t)CONFIG_BSP_DISPLAY_BRIGHTNESS_LEDC_CH);
#else
    return 0xFFFFFFFFu;
#endif
}

/* True once the brightness tracker has settled on the level it is reading: a
 * fade is over and the duty is expected to sit still. */
static bool fw_backlight_settled(int64_t now)
{
    const int bl = display_get_brightness_percent();
    if (bl < 0 || bl != s_bl_seen_pct || bl != s_bl_reported_pct) {
        return false;
    }
    return now - s_bl_stable_ms >= (int64_t)FW_BRIGHT_STABLE_MS;
}

/* ---- Fast-pass detection ----------------------------------------------- */

static void fw_fast_ring_add(const fw_fast_event_t *e)
{
    portENTER_CRITICAL(&s_fast_mux);
    s_fast_events[s_fast_next] = *e;
    s_fast_next = (s_fast_next + 1) % FW_FAST_HISTORY;
    portEXIT_CRITICAL(&s_fast_mux);
}

/* Compares one pass against its baseline and, if a large part of the screen
 * moved, files the event: counters, the ring the HTTP API reads, and one WARN
 * line per hold window (holding keeps a three-frame flash from being logged six
 * times, once per framebuffer per sample).
 *
 * "Moved" is measured per colour channel, not by luma.  The screen changes that
 * matter here - a dark menu turning into the saver's blue wallpaper, a wash-out,
 * a white page - can leave the mean luma almost unchanged while the colour moves
 * a long way, and a luma-only test would silently drop exactly those.  The
 * largest per-channel move (r/g/b) is the number that fires, and a point counts
 * as moved when its own largest channel move is at least FW_FAST_CELL_JUMP in the
 * same direction, so ordinary repaints of one area do not add up to a full-screen
 * event.
 *
 * Everything the panel can do to the glass is in the returned line: the colour
 * before and after, how much of the grid moved (coverage), the mean colour of the
 * points that moved (which names the culprit), the LEDC duty and the software
 * brightness (the backlight half), the page and the LVGL render counters (was
 * anything drawn at all?), and whether the light clock wallpaper was up.  A
 * screen change that reaches this line has nowhere left to hide. */
static bool fw_report_move(const char *tag, int kind, int fb, const fw_fast_t *prev,
                           const fw_fast_t *cur, const uint16_t *px, int64_t now,
                           int64_t hold_limit_ms, int64_t *hold_slot)
{
    const int dr = cur->r - prev->r;
    const int dg = cur->g - prev->g;
    const int db = cur->b - prev->b;

    int move = dr < 0 ? -dr : dr;
    char chan = 'r';
    if ((dg < 0 ? -dg : dg) > move) {
        move = dg < 0 ? -dg : dg;
        chan = 'g';
    }
    if ((db < 0 ? -db : db) > move) {
        move = db < 0 ? -db : db;
        chan = 'b';
    }
    if (move < FW_FAST_LUMA_JUMP) {
        return false;
    }
    /* The sign of the move decides what counts as a moved point, so a grid that is
     * merely noisy in one channel cannot fake a full-screen change. */
    const int dir = (chan == 'r') ? dr : (chan == 'g') ? dg : db;

    int moved = 0;
    long r_sum = 0;
    long g_sum = 0;
    long b_sum = 0;
    for (int k = 0; k < FW_FAST_SAMPLES; k++) {
        const int kd = dir > 0 ? ((chan == 'r') ? (int)cur->cell_r[k] - (int)prev->cell_r[k]
                                 : (chan == 'g') ? (int)cur->cell_g[k] - (int)prev->cell_g[k]
                                                 : (int)cur->cell_b[k] - (int)prev->cell_b[k])
                              : ((chan == 'r') ? (int)prev->cell_r[k] - (int)cur->cell_r[k]
                                 : (chan == 'g') ? (int)prev->cell_g[k] - (int)cur->cell_g[k]
                                                 : (int)prev->cell_b[k] - (int)cur->cell_b[k]);
        if (kd < FW_FAST_CELL_JUMP) {
            continue;
        }
        int r;
        int g;
        int b;
        fw_px_rgb(px[k], &r, &g, &b);
        r_sum += r;
        g_sum += g;
        b_sum += b;
        moved++;
    }

    if (moved * 100 < FW_FAST_SAMPLES * FW_FAST_CELL_PCT) {
        return false;
    }

    const int mr = (int)(r_sum / moved);
    const int mg = (int)(g_sum / moved);
    const int mb = (int)(b_sum / moved);
    const int coverage_pct = (moved * 100) / FW_FAST_SAMPLES;
    const uint32_t duty = fw_backlight_duty();
    const int bl = display_get_brightness_percent();
    const bool log_now = (now - *hold_slot >= hold_limit_ms);
    display_render_stats_t rs;
    display_render_stats_get(&rs);

    const bool bright = (dir > 0);
    if (kind == 0) {
        if (bright) {
            s_fast_bright++;
        } else {
            s_fast_dark++;
        }
    } else {
        if (bright) {
            s_slow_bright++;
        } else {
            s_slow_dark++;
        }
    }

    fw_fast_event_t ev;
    ev.ms = now;
    ev.fb = fb;
    ev.kind = kind;
    ev.dir = bright ? 1 : -1;
    ev.luma_from = prev->luma;
    ev.luma_to = cur->luma;
    ev.r_from = prev->r;
    ev.g_from = prev->g;
    ev.b_from = prev->b;
    ev.r_to = cur->r;
    ev.g_to = cur->g;
    ev.b_to = cur->b;
    ev.move = move;
    ev.chan = chan;
    ev.coverage_pct = coverage_pct;
    ev.r = mr;
    ev.g = mg;
    ev.b = mb;
    ev.duty = duty;
    ev.brightness_pct = bl;
    ev.light = s_light_content ? 1 : 0;
    ev.passes = rs.passes;
    fw_fast_ring_add(&ev);

    if (!log_now) {
        return false;
    }

    *hold_slot = now;
    /* WARN, because this is the line the user is asked to look for: nothing else
     * in this firmware records a screen change that did not survive until the
     * next health line. */
    ESP_LOGW(TAG,
             "%s %s fb=%d uptime=%lldms rgb %d,%d,%d->%d,%d,%d d%c=%d luma %d->%d cov=%d%% "
             "moved=%d,%d,%d tone=%s duty=%u backlight=%d%% light=%d page=%s render passes=%u "
             "last=%ums dirty=%u%% glass=%d/%d/%d %d,%d,%d fast=%u/%u slow=%u/%u",
             tag, bright ? "UP" : "DOWN", fb, (long long)now, ev.r_from, ev.g_from, ev.b_from,
             ev.r_to, ev.g_to, ev.b_to, chan, dir, ev.luma_from, ev.luma_to, coverage_pct, mr, mg,
             mb, fw_tone_name(cur->luma, mr, mg, mb), (unsigned)duty, bl, s_light_content ? 1 : 0,
             ui_pages_current_id(), (unsigned)rs.passes, (unsigned)rs.last_pass_ms,
             (unsigned)rs.last_dirty_pct, s_glass_luma, s_glass_min, s_glass_max, s_glass_r,
             s_glass_g, s_glass_b, (unsigned)s_fast_bright, (unsigned)s_fast_dark,
             (unsigned)s_slow_bright, (unsigned)s_slow_dark);
    display_flash_watch_note(ev.dir > 0 ? (kind == 0 ? "fast:up" : "slow:up")
                                        : (kind == 0 ? "fast:down" : "slow:down"));
    return true;
}

/* One coarse pass per framebuffer, compared against the pass before it. */
static bool fw_fast_pass(int64_t now)
{
    bool reported = false;

    for (int i = 0; i < s_fb_count; i++) {
        size_t bytes = 0;
        const uint8_t *fb = (const uint8_t *)display_frame_buffer_get(i, &bytes);
        if (fb == NULL) {
            continue;
        }

        fw_fast_t cur = {0};
        uint16_t px[FW_FAST_SAMPLES];
        if (!sample_fast(fb, bytes, &cur, px)) {
            continue;
        }

        if (i == 0) {
            int lo = 255;
            int hi = 0;            for (int k = 0; k < FW_FAST_SAMPLES; k++) {
                if (cur.cells[k] < lo) {
                    lo = cur.cells[k];
                }
                if (cur.cells[k] > hi) {
                    hi = cur.cells[k];
                }
            }
            s_glass_luma = cur.luma;
            s_glass_min = lo;
            s_glass_max = hi;
            s_glass_r = cur.r;
            s_glass_g = cur.g;
            s_glass_b = cur.b;
        }

        fw_fast_t *prev = &s_fast[i];
        if (!prev->valid) {
            prev->valid = true;
        } else {
            reported |= fw_report_move("FAST", 0, i, prev, &cur, px, now, FW_FAST_HOLD_MS,
                                       &s_fast_hold_ms);
        }

        *prev = cur;
        prev->valid = true;
    }

    s_fast_passes++;
    return reported;
}

/* The same grid once a second: catches the changes that grew instead of jumping
 * (fades, page turns, a wallpaper swap), which the fast pass cannot see no matter
 * how often it samples.  A one-second baseline also compares settled states
 * rather than two samples 4 ms apart, so it describes the change the user
 * actually ended up looking at. */
static bool fw_slow_pass(int64_t now)
{
    bool reported = false;

    for (int i = 0; i < s_fb_count; i++) {
        size_t bytes = 0;
        const uint8_t *fb = (const uint8_t *)display_frame_buffer_get(i, &bytes);
        if (fb == NULL) {
            continue;
        }

        fw_fast_t cur = {0};
        uint16_t px[FW_FAST_SAMPLES];
        if (!sample_fast(fb, bytes, &cur, px)) {
            continue;
        }

        fw_fast_t *prev = &s_slow[i];
        if (!prev->valid) {
            prev->valid = true;
        } else {
            reported |= fw_report_move("SLOW", 1, i, prev, &cur, px, now, FW_SLOW_HOLD_MS,
                                       &s_slow_hold_ms);
        }

        *prev = cur;
        prev->valid = true;
    }

    s_slow_passes++;
    return reported;
}

/* Hardware backlight witness - see s_hw_duty above. */
static void flash_watch_check_hw_duty(int64_t now, bool settled)
{
    const uint32_t duty = fw_backlight_duty();
    if (duty == 0xFFFFFFFFu) {
        return;
    }
    if (s_hw_duty == 0xFFFFFFFFu || duty == s_hw_duty) {
        s_hw_duty = duty;
        return;
    }

    const uint32_t from = s_hw_duty;
    s_hw_duty = duty;
    if (!settled) {
        return; /* the fade the software asked for */
    }

    s_hw_duty_changes++;
    s_hw_duty_from = from;
    s_hw_duty_to = duty;
    s_hw_duty_last_ms = now;
    if (now - s_hw_duty_last_log_ms >= (int64_t)FW_BRIGHT_STABLE_MS * 8) {
        s_hw_duty_last_log_ms = now;
        ESP_LOGW(TAG,
                 "BACKLIGHT DUTY moved %u -> %u while software level stayed %d%% page=%s "
                 "hw_duty_changes=%u",
                 (unsigned)from, (unsigned)duty, display_get_brightness_percent(), ui_pages_current_id(),
                 (unsigned)s_hw_duty_changes);
        display_flash_watch_note("bl:duty");
    }
}

/* Applies the level that was asked for while the light content was up.  Runs on
 * the moment the light content leaves the glass (the wake path) or when the hold
 * has expired. */
static void fw_held_raise_release(void)
{
    if (s_held_raise_timer != NULL && esp_timer_is_active(s_held_raise_timer)) {
        (void)esp_timer_stop(s_held_raise_timer);
    }
    const int held = s_held_raise_pct;
    s_held_raise_pct = -1;
    if (held < 0) {
        return;
    }
    ESP_LOGI(TAG, "held backlight raise %d%% released (light content left the glass)", held);
    display_flash_watch_note("bl:held raise");
    /* Faded: the raise lands on the menu that was just painted, and a ramp is the
     * one direction the eye reads as pleasant. */
    (void)display_fade_brightness_percent(held, FW_HELD_RAISE_FADE_MS);
}

static void fw_held_raise_timer_cb(void *arg)
{
    (void)arg;
    ESP_LOGW(TAG,
             "held backlight raise %d%% forced through after %u ms of light content "
             "(the light-content flag was stale)", s_held_raise_pct, (unsigned)FW_HELD_RAISE_MAX_MS);
    /* Drop the flag as well: whatever kept it set (a saver that never hid) cannot
     * be trusted to clear it, and every later raise would be held too. */
    s_light_content = false;
    fw_held_raise_release();
}

bool display_flash_watch_hold_raise(int current_pct, int target_pct)
{
    if (!s_light_content || current_pct < 0 || target_pct <= current_pct) {
        return false;
    }
    if (s_held_raise_pct == target_pct) {
        return true;   /* already waiting for this very level */
    }
    s_held_raise_pct = target_pct;
    s_held_raise_count++;
    ESP_LOGW(TAG,
             "backlight raise %d%% -> %d%% held: light clock wallpaper is on the glass "
             "(held_raises=%u)", current_pct, target_pct, (unsigned)s_held_raise_count);
    if (s_held_raise_timer == NULL) {
        const esp_timer_create_args_t args = {
            .callback = fw_held_raise_timer_cb,
            .name = "bl_hold",
        };
        if (esp_timer_create(&args, &s_held_raise_timer) != ESP_OK) {
            s_held_raise_timer = NULL;
            s_held_raise_pct = -1;
            return false;   /* no timer means no safety net: let the raise through */
        }
    }
    (void)esp_timer_stop(s_held_raise_timer);
    (void)esp_timer_start_once(s_held_raise_timer, (uint64_t)FW_HELD_RAISE_MAX_MS * 1000ULL);
    return true;
}

void display_flash_watch_set_light_content(bool light)
{
    if (s_light_content == light) {
        return;
    }
    s_light_content = light;
    if (!light) {
        /* Leaving is the moment the hold waits for: a level asked for while the
         * clock was up is applied now, on top of the menu. */
        fw_held_raise_release();
    }
    display_flash_watch_note(light ? "light content on" : "light content off");
    /* WARN + system_log_event on purpose: with log_verbosity 3 the RAM ring only
     * keeps warnings, and the light clock wallpaper going up or down is the exact
     * moment the glass turns bright blue - it has to survive in the log even when
     * nobody is watching the panel.  Correlate it with the SLOW/FAST lines: a
     * fade spreads its move over many 4 ms passes, so it lands in the slow pass,
     * while a jump lands in the fast one. */
    ESP_LOGW(TAG,
             "light content %s uptime=%lldms backlight=%d%% duty=%u page=%s glass=%d/%d/%d "
             "rgb=%d,%d,%d fast=%u/%u slow=%u/%u",
             light ? "on" : "off", (long long)fw_now_ms(), display_get_brightness_percent(),
             (unsigned)fw_backlight_duty(), ui_pages_current_id(), s_glass_luma, s_glass_min,
             s_glass_max, s_glass_r, s_glass_g, s_glass_b, (unsigned)s_fast_bright,
             (unsigned)s_fast_dark, (unsigned)s_slow_bright, (unsigned)s_slow_dark);
    system_log_event("flashwatch", "light content %s page=%s", light ? "on" : "off",
                     ui_pages_current_id());
}

void display_flash_watch_note_paint(bool full_pass, int64_t started_us, uint32_t ms,
                                    uint32_t dirty_pct)
{
    (void)ms;
    (void)dirty_pct;
    if (full_pass) {
        s_last_full_paint_start_us = started_us;
        s_full_paint_count++;
    }
}

int64_t display_flash_watch_last_full_paint_us(void)
{
    return s_last_full_paint_start_us;
}

bool display_flash_watch_paint_seen_since(int64_t since_us)
{
    const int64_t last = s_last_full_paint_start_us;
    return last != 0 && last >= since_us;
}

uint32_t display_flash_watch_early_raise_count(void)
{
    return s_early_raise_count;
}

uint32_t display_flash_watch_full_paint_count(void)
{
    return s_full_paint_count;
}

void display_flash_watch_report_wake_raise(int from_pct, int to_pct, int64_t waited_ms,
                                           bool paint_seen, int64_t paint_age_ms)
{
    if (paint_seen) {
        ESP_LOGI(TAG, "wake raise %d%% -> %d%% after %lldms (menu painted %lldms ago)",
                 from_pct < 0 ? 0 : from_pct, to_pct, (long long)waited_ms, (long long)paint_age_ms);
        return;
    }

    /* The raise is about to land on the light wallpaper: this is the light-blue
     * flash, seen from the only place that can prove it. */
    s_flash_count++;
    s_early_raise_count++;
    ESP_LOGW(TAG,
             "SCREEN FLASH backlight=raised-before-paint %d%% -> %d%% waited=%lldms "
             "last_full_paint_age=%lldms page=%s flashes=%u washes=%u changes=%u "
             "backlight_changes=%u full_paints=%u early_raises=%u",
             from_pct < 0 ? 0 : from_pct, to_pct, (long long)waited_ms, (long long)paint_age_ms,
             ui_pages_current_id(), (unsigned)s_flash_count, (unsigned)s_wash_count,
             (unsigned)s_change_count, (unsigned)s_brightness_count,
             (unsigned)s_full_paint_count, (unsigned)s_early_raise_count);
    /* The luma fields of FLASH-BL carry the two backlight percentages. */
    fw_last_event_set("FLASH-BL", 100, from_pct < 0 ? 0 : from_pct, to_pct, 0, 0, 0, 0);
    system_log_event("flashwatch", "raise before paint %d%%->%d%% waited=%lldms page=%s",
                     from_pct < 0 ? 0 : from_pct, to_pct, (long long)waited_ms,
                     ui_pages_current_id());
    display_flash_watch_note("bl:early raise");
}

static bool looks_light(const fb_stats_t *st){
    return st->luma >= FW_LIGHT_LUMA && st->bright_pct >= FW_LIGHT_BRIGHT_PCT;
}

static bool looks_dark(const fb_stats_t *st)
{
    return st->luma <= FW_DARK_LUMA && st->bright_pct <= FW_DARK_BRIGHT_PCT;
}

/* Mean 8-bit RGB of the grid points that jumped brighter, converted from their
 * RGB565 values. Returns how many points contributed. */
static int bright_rgb(const uint16_t *cur_px, const uint8_t *cur_cells,
                      const uint8_t *base_cells, int *out_r, int *out_g, int *out_b)
{
    long r_sum = 0;
    long g_sum = 0;
    long b_sum = 0;
    int n = 0;

    *out_r = 0;
    *out_g = 0;
    *out_b = 0;
    if (cur_px == NULL) {
        return 0;
    }

    for (int i = 0; i < FW_SAMPLES; i++) {
        if ((int)cur_cells[i] - (int)base_cells[i] < FW_CELL_JUMP) {
            continue;
        }
        const uint16_t px = cur_px[i];
        r_sum += (long)((((px >> 11) & 0x1FU) * 255U) / 31U);
        g_sum += (long)((((px >> 5) & 0x3FU) * 255U) / 63U);
        b_sum += (long)(((px & 0x1FU) * 255U) / 31U);
        n++;
    }

    if (n == 0) {
        return 0;
    }
    *out_r = (int)(r_sum / n);
    *out_g = (int)(g_sum / n);
    *out_b = (int)(b_sum / n);
    return n;
}

static void evaluate(int fb_index, const fb_stats_t *cur, const uint8_t *cells,
                     const uint16_t *px, int64_t gap_ms)
{
    fb_track_t *t = &s_track[fb_index];

    if (!t->valid) {
        t->valid = true;
        t->st = *cur;
        if (cells != NULL) {
            memcpy(t->cells, cells, sizeof(t->cells));
            t->cells_valid = true;
        }
        return;
    }

    const int64_t now = fw_now_ms();
    int coverage_pct = 0;

    /* Partial-wash tracker (independent grid-coverage trigger). */
    if (cells != NULL && t->cells_valid) {
        int up = 0;
        for (int i = 0; i < FW_SAMPLES; i++) {
            if ((int)cells[i] - (int)t->cells[i] >= FW_CELL_JUMP) {
                up++;
            }
        }
        const int pct = (up * 100) / FW_SAMPLES;
        coverage_pct = pct;
        if (!t->wash_pending) {
            if (pct >= FW_WASH_CELL_PCT) {
                t->wash_pending = true;
                t->wash_ms = now;
                t->wash_peak_pct = pct;
                t->wash_base_luma = t->st.luma;
                t->wash_peak_luma = cur->luma;
                t->wash_r = cur->r;
                t->wash_g = cur->g;
                t->wash_b = cur->b;
            }
        } else {
            if (pct > t->wash_peak_pct) {
                t->wash_peak_pct = pct;
                if (px != NULL) {
                    (void)bright_rgb(px, cells, t->cells, &t->wash_r, &t->wash_g, &t->wash_b);
                }
            }
            if (cur->luma > t->wash_peak_luma) {
                t->wash_peak_luma = cur->luma;
            }
            const int diff = cur->luma - t->wash_base_luma;
            const bool back = (diff >= -FW_RETURN_TOL && diff <= FW_RETURN_TOL) || pct <= FW_WASH_END_PCT;
            if (back) {
                s_wash_count++;
                report_wash(fb_index, t->wash_peak_pct, now - t->wash_ms, t->wash_base_luma,
                            t->wash_peak_luma, t->wash_r, t->wash_g, t->wash_b, gap_ms);
                t->wash_pending = false;
            } else if (now - t->wash_ms > FW_WASH_RETURN_MS) {
                /* Brightened and stayed bright.  On this panel that is the
                 * screensaver with its wallpaper or a page/theme switch; this
                 * branch only bumped a counter before, so the full-screen change
                 * the user actually sees left no trace in the log at all. */
                s_change_count++;
                report_change("BRIGHT-STAY", t->wash_peak_pct, t->wash_base_luma,
                              t->wash_peak_luma, t->wash_r, t->wash_g, t->wash_b,
                              now - t->wash_ms, gap_ms);
                t->wash_pending = false;
            }
        }
        memcpy(t->cells, cells, sizeof(t->cells));
    }

    if (t->pending) {
        if ((t->dir > 0 && cur->luma > t->peak.luma) ||
            (t->dir < 0 && cur->luma < t->peak.luma)) {
            t->peak = *cur;
            t->peak_ms = now;
        }

        const int diff = cur->luma - t->base.luma;
        if (diff >= -FW_RETURN_TOL && diff <= FW_RETURN_TOL) {
            /* Came back to where it was: a real, visible flash. */
            s_flash_count++;
            report(t, fb_index, 1, now - t->pending_ms, &t->peak, gap_ms, coverage_pct);
            t->pending = false;
        } else if (now - t->pending_ms > FW_RETURN_MS) {
            if (t->dir > 0 && looks_light(cur)) {
                s_flash_count++;
                report(t, fb_index, 2, now - t->pending_ms, &t->peak, gap_ms, coverage_pct);
            } else if (t->dir < 0 && looks_dark(cur)) {
                s_flash_count++;
                report(t, fb_index, 3, now - t->pending_ms, &t->peak, gap_ms, coverage_pct);
            } else {
                /* A step that neither washed out nor went dark: a page switch, a
                 * theme, or the screensaver at a different brightness.  Logged
                 * now instead of only counted, with the mean colour of the new
                 * state in flash_rgb. */
                s_change_count++;
                report_change("LUMA-STAY", coverage_pct, t->base.luma, cur->luma,
                              cur->r, cur->g, cur->b, now - t->pending_ms, gap_ms);
            }
            t->pending = false;
        }
    } else {
        const int d = cur->luma - t->st.luma;
        if (d >= FW_LUMA_JUMP || -d >= FW_LUMA_JUMP) {
            t->pending = true;
            t->pending_ms = now;
            t->dir = d > 0 ? 1 : -1;
            t->base = t->st;
            t->peak = *cur;
            t->peak_ms = now;
        }
    }

    t->st = *cur;
}

/* ---- Task -------------------------------------------------------------- */

static void display_flash_watch_task(void *arg)
{
    (void)arg;

    int64_t previous_ms = fw_now_ms();
    int64_t fine_previous_ms = previous_ms;
    int fine_countdown = 0;
    int slow_countdown = FW_SLOW_EVERY;
    const int64_t boot_ms = previous_ms;
    bool armed = false;

    while (true) {
        if (!armed) {
            /* Wait for the first UI paint: before it the framebuffer is black,
             * which used to look like a jump to "stuck dark" on every boot. */
            if (fw_now_ms() - boot_ms < FW_BOOT_IGNORE_MS) {
                vTaskDelay(pdMS_TO_TICKS(FW_DISABLED_PERIOD_MS));
                continue;
            }
            memset(s_track, 0, sizeof(s_track));
            memset(s_fast, 0, sizeof(s_fast));
            memset(s_slow, 0, sizeof(s_slow));
            s_sample_count = 0;
            s_last_report_ms = fw_now_ms();
            previous_ms = s_last_report_ms;
            fine_previous_ms = s_last_report_ms;
            fine_countdown = 0;
            slow_countdown = FW_SLOW_EVERY;
            s_fast_hold_ms = -100000;
            s_slow_hold_ms = -100000;
            s_glass_luma = -1;
            s_glass_min = -1;
            s_glass_max = -1;
            s_glass_r = -1;
            s_glass_g = -1;
            s_glass_b = -1;
            s_bl_reported_pct = display_get_brightness_percent();
            s_bl_seen_pct = s_bl_reported_pct;
            s_bl_stable_ms = previous_ms;
            s_hw_duty = fw_backlight_duty();
            s_hw_duty_last_log_ms = previous_ms;
            armed = true;
            display_flash_watch_note("detector:armed");
            system_log_event("flashwatch", "armed after %dms boot guard duty=%u", FW_BOOT_IGNORE_MS,
                             (unsigned)s_hw_duty);
        }

        if (!s_enabled) {
            vTaskDelay(pdMS_TO_TICKS(FW_DISABLED_PERIOD_MS));
            previous_ms = fw_now_ms();
            continue;
        }

        const int64_t now = fw_now_ms();

        /* Before the framebuffers: a brightness change is visible even when the
         * UI is not repainting at all. */
        flash_watch_check_backlight(now);
        flash_watch_check_hw_duty(now, fw_backlight_settled(now));

        /* Fast pass on every iteration, fine pass every FW_FAST_EVERY of them,
         * slow pass once a second: the coarse pass is what catches a flash that
         * lives for one or two frames, the slow pass is what catches a change
         * that grew instead of jumping, and the fine pass is what then names
         * either one in full detail.  A fast hit pulls the fine pass forward so
         * the event is measured while it is still fresh. */
        if (fw_fast_pass(now)) {
            fine_countdown = 0;
        }

        if (slow_countdown <= 0) {
            fw_slow_pass(now);
            slow_countdown = FW_SLOW_EVERY;
        }
        slow_countdown--;

        if (fine_countdown <= 0) {
            const int64_t gap_ms = now - fine_previous_ms;
            fine_previous_ms = now;
            if (gap_ms > FW_GAP_WARN_MS) {
                s_gap_count++;
            }

            for (int i = 0; i < s_fb_count; i++) {
                size_t bytes = 0;
                const uint8_t *fb = (const uint8_t *)display_frame_buffer_get(i, &bytes);
                if (fb == NULL) {
                    continue;
                }
                fb_stats_t st;
                if (!sample_stats(fb, bytes, &st, s_sample_cells, s_sample_px)) {
                    continue;
                }
                evaluate(i, &st, s_sample_cells, s_sample_px, gap_ms);
                s_sample_count++;
            }
            fine_countdown = FW_FAST_EVERY;
        }
        fine_countdown--;

        vTaskDelay(pdMS_TO_TICKS(FW_FAST_PERIOD_MS));
    }
}

esp_err_t display_flash_watch_start(void)
{
    if (s_started) {
        return ESP_OK;
    }

    s_fb_count = display_frame_buffer_count();
    if (s_fb_count <= 0) {
        ESP_LOGW(TAG, "No panel framebuffer available - flash detector disabled");
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (s_fb_count > DISPLAY_FRAME_BUFFER_MAX) {
        s_fb_count = DISPLAY_FRAME_BUFFER_MAX;
    }

    /* Priority 1 = below the LVGL port task, so sampling can never make the UI
     * sluggish; a late sample only shows up as a large sample_gap in a report.
     * The stack must cover newlib's vfprintf, which the ESP_LOGW below uses. */
    if (xTaskCreate(display_flash_watch_task, "flashwatch", 6144, NULL, 1, &s_task) != pdPASS) {
        ESP_LOGW(TAG, "Could not create the flash detector task");
        return ESP_FAIL;
    }
    s_started = true;
    ESP_LOGI(TAG,
             "Flash detector running: %d framebuffer(s), fast %d pts/%d ms, slow %d pts/%d ms, fine %d pts/%d ms",
             s_fb_count, FW_FAST_SAMPLES, FW_FAST_PERIOD_MS, FW_FAST_SAMPLES, FW_SLOW_PERIOD_MS,
             FW_SAMPLES, FW_PERIOD_MS);
    return ESP_OK;
}

void display_flash_watch_set_enabled(bool enabled)
{
    s_enabled = enabled;
    system_log_event("flashwatch", "sampling %s", enabled ? "enabled" : "disabled");
}

bool display_flash_watch_get_enabled(void)
{
    return s_enabled;
}

void display_flash_watch_summary(char *out, size_t out_len)
{
    if (out == NULL || out_len == 0) {
        return;
    }

    char last[FW_EVENT_TEXT_LEN];
    int64_t age_ms = 0;
    if (fw_last_event_get(last, sizeof(last), &age_ms)) {
        snprintf(out, out_len,
                 "flashes=%u washes=%u changes=%u bl=%u held=%u hwbl=%u duty=%u samples=%u gaps=%u enabled=%d "
                 "light=%d glass=%d/%d/%d rgb=%d,%d,%d fast=%u fastdk=%u fastpass=%u slow=%u slowdk=%u slowpass=%u last=%s age=%us",
                 (unsigned)s_flash_count, (unsigned)s_wash_count, (unsigned)s_change_count,
                 (unsigned)s_brightness_count, (unsigned)s_held_raise_count, (unsigned)s_hw_duty_changes,
                 (unsigned)s_hw_duty, (unsigned)s_sample_count, (unsigned)s_gap_count, s_enabled ? 1 : 0,
                 s_light_content ? 1 : 0,
                 s_glass_luma, s_glass_min, s_glass_max, s_glass_r, s_glass_g, s_glass_b,
                 (unsigned)s_fast_bright, (unsigned)s_fast_dark, (unsigned)s_fast_passes,
                 (unsigned)s_slow_bright, (unsigned)s_slow_dark, (unsigned)s_slow_passes,
                 last, (unsigned)(age_ms / 1000));
        return;
    }

    snprintf(out, out_len,
             "flashes=%u washes=%u changes=%u bl=%u held=%u hwbl=%u duty=%u samples=%u gaps=%u enabled=%d "
             "light=%d glass=%d/%d/%d rgb=%d,%d,%d fast=%u fastdk=%u fastpass=%u slow=%u slowdk=%u slowpass=%u last=none",
             (unsigned)s_flash_count, (unsigned)s_wash_count, (unsigned)s_change_count,
             (unsigned)s_brightness_count, (unsigned)s_held_raise_count, (unsigned)s_hw_duty_changes,
             (unsigned)s_hw_duty, (unsigned)s_sample_count, (unsigned)s_gap_count, s_enabled ? 1 : 0,
             s_light_content ? 1 : 0,
             s_glass_luma, s_glass_min, s_glass_max, s_glass_r, s_glass_g, s_glass_b,
             (unsigned)s_fast_bright, (unsigned)s_fast_dark, (unsigned)s_fast_passes,
             (unsigned)s_slow_bright, (unsigned)s_slow_dark, (unsigned)s_slow_passes);
}

/* Newest-last dump of the fast-pass history for the HTTP API: the compact form of
 * "every screen change the panel made, including the ones that lasted less than a
 * single fine-pass interval".  Multi-line on purpose - the user pastes it back
 * whole and the timestamps line up with /api/logs. */
void display_flash_watch_get_fast(char *out, size_t out_len)
{
    if (out == NULL || out_len == 0) {
        return;
    }

    int n = snprintf(out, out_len,
                     "passes=%u slow_passes=%u light=%d backlight=%d%% duty=%u page=%s "
                     "glass=%d/%d/%d rgb=%d,%d,%d fast_up=%u fast_down=%u slow_up=%u slow_down=%u "
                     "full_paints=%u early_raises=%u",
                     (unsigned)s_fast_passes, (unsigned)s_slow_passes, s_light_content ? 1 : 0,
                     display_get_brightness_percent(), (unsigned)fw_backlight_duty(),
                     ui_pages_current_id(), s_glass_luma, s_glass_min, s_glass_max, s_glass_r,
                     s_glass_g, s_glass_b, (unsigned)s_fast_bright, (unsigned)s_fast_dark,
                     (unsigned)s_slow_bright, (unsigned)s_slow_dark,
                     (unsigned)s_full_paint_count, (unsigned)s_early_raise_count);
    if (n <= 0) {
        out[0] = '\0';
        return;
    }
    size_t used = (size_t)n;
    if (used >= out_len) {
        out[out_len - 1U] = '\0';
        return;
    }

    /* Copied out of the ring in one go: the HTTP task must not hold a spinlock
     * while it formats, and the flashwatch task must not be delayed by it. */
    fw_fast_event_t events[FW_FAST_HISTORY];
    int next;
    portENTER_CRITICAL(&s_fast_mux);
    memcpy(events, s_fast_events, sizeof(events));
    next = s_fast_next;
    portEXIT_CRITICAL(&s_fast_mux);

    for (int i = 0; i < FW_FAST_HISTORY; i++) {
        const fw_fast_event_t *e = &events[(next + i) % FW_FAST_HISTORY];
        if (e->ms <= 0) {
            continue;
        }
        n = snprintf(out + used, out_len - used,
                     "\n%lldms fb%d %s %s rgb %d,%d,%d->%d,%d,%d d%c=%d luma %d->%d cov=%d%% "
                     "moved=%d,%d,%d duty=%u bl=%d%% light=%d passes=%u",
                     (long long)e->ms, e->fb, e->kind == 0 ? "fast" : "slow",
                     e->dir > 0 ? "up" : "down", e->r_from, e->g_from, e->b_from, e->r_to, e->g_to,
                     e->b_to, e->chan, e->move, e->luma_from, e->luma_to,
                     e->coverage_pct, e->r, e->g, e->b, (unsigned)e->duty,
                     e->brightness_pct, e->light, (unsigned)e->passes);
        if (n <= 0) {
            continue;
        }
        used += (size_t)n;
        if (used >= out_len) {
            out[out_len - 1U] = '\0';
            return;
        }
    }
}

#else /* !CONFIG_APP_PANEL_VARIANT_7INCH_1024 */

/* Other panel variants (Guition 4"/10.1", S3 4") keep their own firmware: the
 * detector compiles to no-ops there so the shared build stays green. */
esp_err_t display_flash_watch_start(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

void display_flash_watch_set_enabled(bool enabled)
{
    (void)enabled;
}

bool display_flash_watch_get_enabled(void)
{
    return false;
}

void display_flash_watch_note(const char *event)
{
    (void)event;
}

void display_flash_watch_note_snapshot(bool active)
{
    (void)active;
}

void display_flash_watch_set_light_content(bool light)
{
    (void)light;
}

bool display_flash_watch_hold_raise(int current_pct, int target_pct)
{
    (void)current_pct;
    (void)target_pct;
    return false;
}

void display_flash_watch_summary(char *out, size_t out_len)
{
    if (out != NULL && out_len > 0) {
        out[0] = '\0';
    }
}

void display_flash_watch_get_fast(char *out, size_t out_len)
{
    if (out != NULL && out_len > 0) {
        out[0] = '\0';
    }
}

#endif
