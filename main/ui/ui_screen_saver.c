/* SPDX-License-Identifier: LicenseRef-FNCL-1.1
 * Copyright (c) 2026 Cpt_Kirk
 */
#include "ui/ui_screen_saver.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "lvgl.h"

#include "app_config.h"
#include "diag/display_flash_watch.h"
#include "diag/dsi_underrun_watch.h"
#include "diag/system_log.h"
#include "drivers/display_init.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "ui/fonts/app_text_fonts.h"
#include "ui/ui_i18n.h"
#include "ui/ui_cameras_page.h"
#include "ui/ui_page_style.h"

#define TAG_SCREEN_SAVER "screen_saver"

/* Poll interval: the screensaver reacts within one tick of the idle timeout. */
#define SCREEN_SAVER_TICK_MS 1000

/* Timeout for the display lock around the short LVGL part of a wallpaper
 * transaction: the frame pointer is exchanged while LVGL is not drawing. */
#define WALLPAPER_DISPLAY_LOCK_MS 1000

/* Timeout for the wallpaper transaction mutex.  A caller may have to wait for a
 * whole frame to be read from or written to the storage, which is the long part
 * of the job and deliberately runs outside the display lock. */
#define WALLPAPER_JOB_LOCK_MS 3000

/* Grace period after a real touch during which an MQTT "wake" is ignored.
 * This keeps a motion sensor from yanking the menu away the moment the user
 * touches the panel, while a wake outside this window shows the clock at
 * once (so the manual button always has an immediate effect). */
#define WAKE_GRACE_MS 30000

/* Timeout for the display lock taken in the wake path.  A touch arrives from the
 * LVGL task, which already holds the (recursive) lock, so it is free there; the
 * timeout only matters when the HTTP or audio task races the renderer. */
#define SCREEN_SAVER_WAKE_LOCK_MS 100

/* Delay between dropping the clock overlay and restoring the menu brightness.
 * The overlay is hidden immediately, but the menu has to be *painted* before the
 * panel gets brighter: raising the backlight first lit the light clock wallpaper
 * at menu level, which is exactly what the eye sees as a light-blue flash on
 * every wake.  A full 1024x600 repaint measures ~166 ms, so 250 ms leaves a
 * margin - but only when LVGL runs to schedule. */
#define SCREEN_SAVER_WAKE_BRIGHTEN_MS 250
/* The margin above is a guess about a paint this module cannot see, and LVGL can
 * be late: the render pass that replaces the wallpaper is fed to the flash
 * detector (display_flash_watch_note_paint), and the brighten waits for it to
 * complete instead of trusting the clock.  These two bound that wait: retry every
 * 50 ms, and force the raise after 1200 ms so a skipped pass cannot leave the
 * panel stuck at saver brightness. */
#define SCREEN_SAVER_WAKE_BRIGHTEN_RETRY_MS 50
#define SCREEN_SAVER_WAKE_BRIGHTEN_MAX_MS 1200

/* Reveal.  The clock comes up with the wallpaper already on the glass: the
 * backlight is stepped down and the dim recolor is written *before* the overlay
 * is unhidden, so no frame ever shows the wallpaper at menu brightness - the
 * darkening that used to happen after the reveal is now part of it.  There is
 * deliberately no black hold and no fade-in: that is what the user saw as
 * "first a black background, the wallpaper only afterwards". */
/* Reveal age at which the saver counts as settled.  A dismissal before this was
 * a blink rather than a clock somebody looked at, so it is counted and logged. */
#define SCREEN_SAVER_REVEAL_SETTLE_MS 300
/* Do not reveal the clock when something was active within this window: the wake
 * that follows would take it away again mid-reveal.  Only timeout-driven reveals
 * are guarded - a wake-to-screensaver shows the clock immediately, that is what
 * the wake asked for. */
#define SCREEN_SAVER_REVEAL_GUARD_MS 1000

static uint8_t s_brightness = APP_DISPLAY_ACTIVE_BRIGHTNESS_PERCENT;
static uint8_t s_saver_brightness = APP_DISPLAY_SAVER_BRIGHTNESS_PERCENT;
/* Darkening of the wallpaper behind the clock, in percent (0 = as uploaded). */
static uint8_t s_wallpaper_dim = APP_DISPLAY_SAVER_WALLPAPER_DIM_PERCENT;
static bool s_screensaver_enabled = true;
static uint32_t s_screensaver_timeout_sec = APP_DISPLAY_SCREENSAVER_TIMEOUT_SEC;
static bool s_screen_off_enabled = APP_DISPLAY_SCREEN_OFF_ENABLED ? true : false;
static uint32_t s_screen_off_timeout_sec = APP_DISPLAY_SCREEN_OFF_TIMEOUT_SEC;
static bool s_clock_24h = APP_DISPLAY_CLOCK_24H;
static bool s_show_seconds = APP_DISPLAY_SAVER_SHOW_SECONDS;
static bool s_show_date = APP_DISPLAY_SAVER_SHOW_DATE;
static uint32_t s_clock_color = APP_DISPLAY_SAVER_CLOCK_COLOR;
static uint32_t s_date_color = APP_DISPLAY_SAVER_DATE_COLOR;
static bool s_night_enabled = false;
static uint16_t s_night_start_min = APP_DISPLAY_NIGHT_START_MIN;
static uint16_t s_night_end_min = APP_DISPLAY_NIGHT_END_MIN;
static uint8_t s_night_brightness = APP_DISPLAY_NIGHT_BRIGHTNESS_PERCENT;
static uint16_t s_night_wake_sec = APP_DISPLAY_NIGHT_WAKE_SEC;

static bool s_screen_off = false;
static bool s_overlay_shown = false;
/* Last strings written into the clock/date labels.  lv_label_set_text() with an
 * identical string still frees and rebuilds the text and repaints the label, and
 * the saver tick calls the clock update once a second: keeping the rendered text
 * here turns that into a no-op until the minute (or the date, or the seconds
 * digit with s_show_seconds) really changes. */
static char s_clock_rendered[16];
static char s_date_rendered[32];
/* Night schedule state: s_night_active is true inside the configured window,
 * s_night_wake_until_ms holds the deadline of the temporary wake granted by a
 * touch (0 = none). */
static bool s_night_active = false;
static int64_t s_night_wake_until_ms = 0;
/* Set when an MQTT/API wake command (e.g. a motion sensor) wakes the panel:
 * the screensaver clock shows immediately and stays until the screen-off
 * timeout turns the backlight off. A real touch clears it (full UI). */
static bool s_wake_to_screensaver = false;
static lv_timer_t *s_timer = NULL;
/* Backlight hand-over on a wake: the driver asks this module first, so the light
 * clock wallpaper is off the glass before the panel gets brighter (see
 * ui_screen_saver_on_activity()).  The restore itself runs from a one-shot timer
 * so it lands after the menu has been painted. */
static esp_timer_handle_t s_wake_brighten_timer = NULL;
static bool s_wake_brighten_pending = false;
static int s_wake_brighten_from = -1;
/* When the brighten was asked for: the paint that has to land before it runs must
 * have started after this timestamp. */
static int64_t s_wake_brighten_req_us = 0;
/* Set once the wait for that paint has been reported, so a wake that needs
 * several retries logs one line instead of one per retry. */
static bool s_wake_brighten_wait_logged = false;
/* Reveal bookkeeping: when the clock was put on the glass, and how often a
 * reveal was dismissed before the wallpaper had faded in - the blink the user
 * reports.  Exposed through the diagnostics endpoint. */
static int64_t s_reveal_at_us = 0;
static uint32_t s_reveal_count = 0;
static uint32_t s_reveal_blip_count = 0;
static int64_t s_reveal_last_ms = 0;
static lv_obj_t *s_overlay = NULL;
static lv_obj_t *s_time_label = NULL;
/* AM/PM badge: the same two letters drawn on top of each other with a 1 px
 * offset. LVGL only thickens outlines for vector fonts, so stacking copies is
 * how a bitmap font gets a bold look here. Index 0 is the base copy. */
#define AMPM_COPY_COUNT 5
static lv_obj_t *s_ampm_copies[AMPM_COPY_COUNT] = {0};
static const int8_t s_ampm_copy_ofs[AMPM_COPY_COUNT][2] = {
    {0, 0}, {1, 0}, {-1, 0}, {0, 1}, {0, -1},
};
static lv_obj_t *s_date_label = NULL;
static lv_obj_t *s_bg_image = NULL;
static uint8_t *s_wallpaper_data = NULL;
static lv_image_dsc_t s_wallpaper_dsc = {0};
static bool s_wallpaper_loaded = false;
/* Frame read before the display lock is taken, published by init(). */
static uint8_t *s_wallpaper_pending = NULL;
/* Copy of the live frame the storage sync writes out; only touched by it. */
static uint8_t *s_wallpaper_stage = NULL;
/* Serialises whole wallpaper transactions (read/publish/write).  Created on
 * first use; see the lock notes on ui_screen_saver_reload_wallpaper(). */
static SemaphoreHandle_t s_wallpaper_mutex = NULL;

/* Flip clock: one card per digit (HHMM, leading zero kept) with the top half of
 * the digit drawn on its own clipped layer so it can be folded down over the
 * centre seam while the time changes. */
#define FLIP_DIGIT_COUNT 4
#define FLIP_DIGIT_UNSET 0xFF

typedef struct {
    lv_obj_t *top_clip;     /* folds down over the seam */
    lv_obj_t *top_label;    /* digit drawn inside top_clip  */
    lv_obj_t *bottom_label; /* static lower half of the digit */
    uint8_t shown;          /* digit on screen, FLIP_DIGIT_UNSET = nothing drawn yet */
} flip_digit_t;

static uint8_t s_clock_style = APP_DISPLAY_SAVER_CLOCK_STYLE_DEFAULT;
/* Set from the HTTP task, consumed by the LVGL task: the overlay must not be
 * destroyed by the request handler. */
static bool s_style_pending = false;
/* Same rule for the wallpaper dimming: only the LVGL task may touch styles. */
static bool s_wallpaper_dim_pending = false;
static uint32_t s_flip_color_applied = 0xFFFFFFFF;
static lv_obj_t *s_flip_row = NULL;
static int32_t s_flip_row_w = 0;
/* Last AM/PM state drawn: 0 = AM, 1 = PM, 2 = not drawn (24 h or unknown).
 * Keeps the badge from being re-laid out every second. */
static uint8_t s_ampm_state = 2;
static flip_digit_t s_flip_digits[FLIP_DIGIT_COUNT];

static void ui_screen_saver_flip_scale_cb(void *var, int32_t value)
{
    lv_obj_t *flap = (lv_obj_t *)var;
    lv_obj_set_style_transform_scale_y(flap, value, LV_PART_MAIN);
    /* Darken the flap a little while it folds away: without it the fold reads
     * as a shrink instead of a card turning over. */
    const uint32_t folded = (uint32_t)(LV_SCALE_NONE - value);
    lv_obj_set_style_opa(flap, (lv_opa_t)(255U - (folded * 90U) / LV_SCALE_NONE), LV_PART_MAIN);
}

/* Drops every flip handle. Must run before the objects it points at are
 * deleted: a running fold animation would otherwise write into freed memory. */
static void ui_screen_saver_flip_reset(void)
{
    for (uint32_t i = 0; i < FLIP_DIGIT_COUNT; i++) {
        if (s_flip_digits[i].top_clip != NULL) {
            lv_anim_delete(s_flip_digits[i].top_clip, ui_screen_saver_flip_scale_cb);
        }
        memset(&s_flip_digits[i], 0, sizeof(s_flip_digits[i]));
        s_flip_digits[i].shown = FLIP_DIGIT_UNSET;
    }
    s_flip_row = NULL;
    s_flip_row_w = 0;
    s_flip_color_applied = 0xFFFFFFFF;
}

static void ui_screen_saver_flip_digit_text(uint8_t digit, char *buf, size_t len)
{
    snprintf(buf, len, "%u", (unsigned)digit);
}

/* Second half of the fold: the digit has already been swapped in, the top flap
 * grows back to its full height. */
static void ui_screen_saver_flip_unfold(flip_digit_t *digit)
{
    char buf[4];
    ui_screen_saver_flip_digit_text(digit->shown, buf, sizeof(buf));
    lv_label_set_text(digit->top_label, buf);

    lv_anim_t anim;
    lv_anim_init(&anim);
    lv_anim_set_var(&anim, digit->top_clip);
    lv_anim_set_exec_cb(&anim, ui_screen_saver_flip_scale_cb);
    lv_anim_set_values(&anim, 0, LV_SCALE_NONE);
    lv_anim_set_duration(&anim, APP_DISPLAY_SAVER_FLIP_ANIM_MS);
    lv_anim_set_path_cb(&anim, lv_anim_path_ease_out);
    lv_anim_start(&anim);
}

static void ui_screen_saver_flip_folded_cb(lv_anim_t *anim)
{
    ui_screen_saver_flip_unfold((flip_digit_t *)anim->user_data);
}

static void ui_screen_saver_flip_set_digit(flip_digit_t *digit, uint8_t value)
{
    if (digit->top_clip == NULL || digit->shown == value) {
        return;
    }

    char buf[4];
    ui_screen_saver_flip_digit_text(value, buf, sizeof(buf));

    bool first_fill = (digit->shown == FLIP_DIGIT_UNSET);
    digit->shown = value;

    if (first_fill) {
        /* Nothing to fold on the first fill: show the digit straight away. */
        lv_label_set_text(digit->top_label, buf);
        lv_label_set_text(digit->bottom_label, buf);
        lv_obj_set_style_transform_scale_y(digit->top_clip, LV_SCALE_NONE, LV_PART_MAIN);
        lv_obj_set_style_opa(digit->top_clip, LV_OPA_COVER, LV_PART_MAIN);
        return;
    }

    /* The lower half shows the new digit immediately, the upper flap folds down
     * over the seam and unfolds again with the new digit on it. */
    lv_label_set_text(digit->bottom_label, buf);

    lv_anim_delete(digit->top_clip, ui_screen_saver_flip_scale_cb);

    lv_anim_t anim;
    lv_anim_init(&anim);
    lv_anim_set_var(&anim, digit->top_clip);
    lv_anim_set_exec_cb(&anim, ui_screen_saver_flip_scale_cb);
    lv_anim_set_values(&anim, LV_SCALE_NONE, 0);
    lv_anim_set_duration(&anim, APP_DISPLAY_SAVER_FLIP_ANIM_MS);
    lv_anim_set_path_cb(&anim, lv_anim_path_ease_in);
    lv_anim_set_user_data(&anim, digit);
    lv_anim_set_completed_cb(&anim, ui_screen_saver_flip_folded_cb);
    lv_anim_start(&anim);
}

static lv_obj_t *ui_screen_saver_flip_create_half(lv_obj_t *card, int32_t clip_y, int32_t label_y,
    lv_obj_t **label_out)
{
    lv_obj_t *clip = lv_obj_create(card);
    lv_obj_remove_style_all(clip);
    lv_obj_set_size(clip, APP_DISPLAY_SAVER_FLIP_CARD_W, APP_DISPLAY_SAVER_FLIP_CARD_H / 2);
    lv_obj_set_pos(clip, 0, clip_y);
    lv_obj_clear_flag(clip, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *label = lv_label_create(clip);
    lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_CLIP);
    lv_label_set_text(label, "0");
    lv_obj_set_style_text_font(label, APP_FONT_CLOCK_84, LV_PART_MAIN);
    lv_obj_set_style_text_color(label, lv_color_hex(s_clock_color), LV_PART_MAIN);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    /* A full card tall label inside a half card tall clip: the clip cuts it on
     * the seam, which is what makes the two halves of one digit line up. */
    lv_obj_set_size(label, APP_DISPLAY_SAVER_FLIP_CARD_W, APP_DISPLAY_SAVER_FLIP_CARD_H);
    lv_obj_set_pos(label, 0, label_y);
    lv_obj_clear_flag(label, LV_OBJ_FLAG_SCROLLABLE);

    *label_out = label;
    return clip;
}

static void ui_screen_saver_flip_create_card(uint32_t index, int32_t x)
{
    const int32_t half_h = APP_DISPLAY_SAVER_FLIP_CARD_H / 2;

    lv_obj_t *card = lv_obj_create(s_flip_row);
    lv_obj_remove_style_all(card);
    lv_obj_set_size(card, APP_DISPLAY_SAVER_FLIP_CARD_W, APP_DISPLAY_SAVER_FLIP_CARD_H);
    lv_obj_set_pos(card, x, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_radius(card, APP_DISPLAY_SAVER_FLIP_CARD_RADIUS, LV_PART_MAIN);
    lv_obj_set_style_bg_color(card, lv_color_hex(APP_DISPLAY_SAVER_FLIP_CARD_COLOR), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, LV_PART_MAIN);

    flip_digit_t *digit = &s_flip_digits[index];
    digit->shown = FLIP_DIGIT_UNSET; /* the first fill must not animate */
    digit->top_clip = ui_screen_saver_flip_create_half(card, 0, APP_DISPLAY_SAVER_FLIP_LABEL_Y,
        &digit->top_label);
    /* Fold line: the bottom edge of the upper flap. */
    lv_obj_set_style_transform_pivot_x(digit->top_clip, APP_DISPLAY_SAVER_FLIP_CARD_W / 2, LV_PART_MAIN);
    lv_obj_set_style_transform_pivot_y(digit->top_clip, half_h, LV_PART_MAIN);
    ui_screen_saver_flip_create_half(card, half_h, APP_DISPLAY_SAVER_FLIP_LABEL_Y - half_h,
        &digit->bottom_label);

    /* Seam between the two halves, like the hinge of a real flip card. */
    lv_obj_t *seam = lv_obj_create(card);
    lv_obj_remove_style_all(seam);
    lv_obj_set_size(seam, APP_DISPLAY_SAVER_FLIP_CARD_W, 2);
    lv_obj_set_pos(seam, 0, half_h - 1);
    lv_obj_clear_flag(seam, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(seam, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(seam, LV_OPA_40, LV_PART_MAIN);
}

static void ui_screen_saver_flip_build(lv_obj_t *parent)
{
    const int32_t card_w = APP_DISPLAY_SAVER_FLIP_CARD_W;
    const int32_t gap = APP_DISPLAY_SAVER_FLIP_CARD_GAP;
    const int32_t row_w = (card_w * FLIP_DIGIT_COUNT) + (gap * (FLIP_DIGIT_COUNT - 1));
    const int32_t row_y = APP_SCREEN_HEIGHT - APP_DISPLAY_SAVER_CLOCK_BOTTOM_GAP -
        APP_DISPLAY_SAVER_FLIP_CARD_H;

    s_flip_row = lv_obj_create(parent);
    lv_obj_remove_style_all(s_flip_row);
    lv_obj_set_size(s_flip_row, row_w, APP_DISPLAY_SAVER_FLIP_CARD_H);
    s_flip_row_w = row_w;
    lv_obj_set_pos(s_flip_row, (APP_SCREEN_WIDTH - row_w) / 2, row_y);
    lv_obj_clear_flag(s_flip_row, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    for (uint32_t i = 0; i < FLIP_DIGIT_COUNT; i++) {
        ui_screen_saver_flip_create_card(i, (int32_t)i * (card_w + gap));
    }

    s_flip_color_applied = s_clock_color;
}

static void ui_screen_saver_flip_update(int hour, int minute)
{
    if (s_flip_digits[0].top_clip == NULL) {
        return;
    }

    if (s_clock_color != s_flip_color_applied) {
        for (uint32_t i = 0; i < FLIP_DIGIT_COUNT; i++) {
            lv_obj_set_style_text_color(s_flip_digits[i].top_label, lv_color_hex(s_clock_color), LV_PART_MAIN);
            lv_obj_set_style_text_color(s_flip_digits[i].bottom_label, lv_color_hex(s_clock_color), LV_PART_MAIN);
        }
        s_flip_color_applied = s_clock_color;
    }

    const uint8_t digits[FLIP_DIGIT_COUNT] = {
        (uint8_t)(hour / 10), (uint8_t)(hour % 10),
        (uint8_t)(minute / 10), (uint8_t)(minute % 10),
    };
    for (uint32_t i = 0; i < FLIP_DIGIT_COUNT; i++) {
        ui_screen_saver_flip_set_digit(&s_flip_digits[i], digits[i]);
    }
}

/* Height of the clock block on the overlay, used to stack the date caption
 * above it. */
static uint32_t ui_screen_saver_clock_block_height(void)
{
    if (s_clock_style == APP_DISPLAY_SAVER_CLOCK_STYLE_FLIP) {
        return APP_DISPLAY_SAVER_FLIP_CARD_H;
    }
    return lv_font_get_line_height(APP_FONT_CLOCK_84);
}

/* Backlight target for the current screensaver state. While the clock overlay
 * is up the panel drops to the configured screensaver brightness (readable but
 * far darker than the menu); hiding it restores the menu brightness. Never
 * brighter than the active setting, so a high saver value cannot surprise the
 * user with a brighter screen than the menu. */
static int ui_screen_saver_backlight_target(void)
{
    if (s_overlay_shown) {
        return (s_saver_brightness < s_brightness) ? s_saver_brightness : s_brightness;
    }
    return s_brightness;
}

/* Clamps a configured screensaver brightness to APP_DISPLAY_SAVER_BRIGHTNESS_MAX_PERCENT.
 * A saver as bright as the menu reveals the light wallpaper at menu level, which
 * is the light blue flash reported on this panel, so the value is capped here as
 * well as in the settings validation: this also covers values already stored in
 * NVS by an older build. Logged once per rejected value - the setting arrives
 * from the web UI, the on-panel screen and MQTT, and a silent clamp would leave
 * the user wondering why the value does not stick. */
static uint8_t ui_screen_saver_clamp_brightness(uint8_t value)
{
    if (value <= APP_DISPLAY_SAVER_BRIGHTNESS_MAX_PERCENT) {
        return value;
    }

    static int warned_for = -1;
    if (warned_for != (int)value) {
        warned_for = (int)value;
        ESP_LOGW(TAG_SCREEN_SAVER,
                 "saver brightness %u%% clamped to %d%% (the saver wallpaper is lighter than the menu)",
                 (unsigned)value, APP_DISPLAY_SAVER_BRIGHTNESS_MAX_PERCENT);
    }
    return APP_DISPLAY_SAVER_BRIGHTNESS_MAX_PERCENT;
}

static void ui_screen_saver_apply_backlight(void)
{
    if (s_screen_off) {
        return; /* the screen-off path owns the backlight */
    }

    /* Faded, not stepped: waking is the direction where the change is pleasant
     * (the light wallpaper brightens back to menu level, see
     * display_fade_brightness_percent). */
    (void)display_fade_brightness_percent(ui_screen_saver_backlight_target(), APP_DISPLAY_SAVER_BACKLIGHT_FADE_MS);
}

/* Steps the backlight down instantly, without a fade. Used when the clock
 * appears: the saver wallpaper is much lighter than the dark menu pages, so
 * fading *after* revealing it shows a bright light-blue frame for the whole
 * fade (the "screen flashes light blue" report). Dimming first removes the
 * flash; the fade is reserved for the wake-up direction. */
static void ui_screen_saver_apply_backlight_step(void)
{
    if (s_screen_off) {
        return;
    }
    (void)display_set_brightness_percent(ui_screen_saver_backlight_target());
}

/* Opacity of the black recolor layer that dims the wallpaper (see
 * ui_screen_saver_apply_wallpaper_dim).  255 hides the wallpaper completely. */
static lv_opa_t ui_screen_saver_wallpaper_dim_opa(void)
{
    return (lv_opa_t)(((int)s_wallpaper_dim * 255) / 100);
}

/* Cancels a brighten that is still waiting for the menu to be painted. */
static void ui_screen_saver_cancel_wake_brighten(void)
{
    s_wake_brighten_wait_logged = false;
    if (!s_wake_brighten_pending) {
        return;
    }
    s_wake_brighten_pending = false;
    if (s_wake_brighten_timer != NULL) {
        (void)esp_timer_stop(s_wake_brighten_timer);
    }
}

/* Restores the menu brightness once the menu is on the glass.  Runs from the
 * esp_timer task: it only touches the backlight (never LVGL), which is the same
 * context the brightness fade is driven from.
 *
 * The delay that brought us here is only a floor: the raise itself waits for the
 * render pass that replaces the light clock wallpaper, because the LVGL task can
 * be late (a camera frame is decoded on it, SD writes stall it) and a raise that
 * beats that pass lights the wallpaper up at menu level - the light-blue flash
 * seen on every wake.  The pass is reported by the display driver, so this is a
 * measurement, not a second guess: it is retried every 50 ms and forced through
 * after 1.2 s, and a raise that still beats the paint is reported as a flash. */
static void ui_screen_saver_wake_brighten_cb(void *arg)
{
    (void)arg;

    if (s_overlay_shown) {
        /* The clock came back while we waited: it owns the backlight again. */
        s_wake_brighten_pending = false;
        return;
    }

    const int64_t now_us = esp_timer_get_time();
    if (s_wake_brighten_req_us != 0 &&
        !display_flash_watch_paint_seen_since(s_wake_brighten_req_us)) {
        const int64_t waited = (now_us - s_wake_brighten_req_us) / 1000;
        if (waited < SCREEN_SAVER_WAKE_BRIGHTEN_MAX_MS && s_wake_brighten_timer != NULL) {
            if (!s_wake_brighten_wait_logged) {
                s_wake_brighten_wait_logged = true;
                ESP_LOGW(TAG_SCREEN_SAVER,
                         "wake: the menu has not been painted yet %lldms after the overlay was "
                         "hidden - holding the brighten for the render pass",
                         (long long)waited);
            }
            if (esp_timer_start_once(s_wake_brighten_timer,
                                     SCREEN_SAVER_WAKE_BRIGHTEN_RETRY_MS * 1000ULL) == ESP_OK) {
                return;
            }
        }
    }

    const bool paint_seen = s_wake_brighten_req_us == 0 ||
                            display_flash_watch_paint_seen_since(s_wake_brighten_req_us);
    const int64_t waited_ms = s_wake_brighten_req_us == 0
                                  ? (int64_t)SCREEN_SAVER_WAKE_BRIGHTEN_MS
                                  : (now_us - s_wake_brighten_req_us) / 1000;
    const int64_t last_paint_us = display_flash_watch_last_full_paint_us();
    const int64_t paint_age_ms = last_paint_us == 0 ? -1 : (now_us - last_paint_us) / 1000;

    s_wake_brighten_pending = false;
    s_wake_brighten_wait_logged = false;

    const int current = display_get_brightness_percent();
    if (s_wake_brighten_from >= 0 && current != s_wake_brighten_from) {
        /* The night schedule, a settings change or the screen-off path took the
         * backlight over during the delay: its value wins, and the light-content
         * flag only has to come down. */
        display_flash_watch_set_light_content(false);
        ESP_LOGI(TAG_SCREEN_SAVER, "wake: backlight left at %d%% (was %d%%, changed while the menu painted)",
                 current, s_wake_brighten_from);
        return;
    }

    /* The menu is painted by now, so no light content is left on the glass. */
    display_flash_watch_set_light_content(false);
    if (s_screen_off) {
        return;
    }
    /* Reported before the raise: the detector can then tell a raise that landed on
     * the menu apart from one that beat the paint, which is the difference
     * between a pleasant brighten and the flash the user reports. */
    display_flash_watch_report_wake_raise(current, ui_screen_saver_backlight_target(), waited_ms,
                                          paint_seen, paint_age_ms);
    ui_screen_saver_apply_backlight();
    ESP_LOGI(TAG_SCREEN_SAVER, "wake: backlight %d%% -> %d%% %lld ms after the overlay (paint %lld ms old)",
             current, ui_screen_saver_backlight_target(), (long long)waited_ms,
             (long long)paint_age_ms);
    system_log_event("backlight", "wake brighten %d%% -> %d%% (deferred %lld ms, paint %lld ms)",
                     current, ui_screen_saver_backlight_target(), (long long)waited_ms,
                     (long long)paint_age_ms);
}

static void ui_screen_saver_schedule_wake_brighten(void)
{
    if (s_wake_brighten_timer == NULL) {
        /* No timer (init has not run yet): rather than leaving the panel dark,
         * fall back to the immediate restore. */
        ui_screen_saver_apply_backlight();
        return;
    }

    s_wake_brighten_from = display_get_brightness_percent();
    s_wake_brighten_req_us = esp_timer_get_time();
    s_wake_brighten_wait_logged = false;
    if (s_wake_brighten_pending) {
        (void)esp_timer_stop(s_wake_brighten_timer);
    }
    s_wake_brighten_pending = true;
    if (esp_timer_start_once(s_wake_brighten_timer, SCREEN_SAVER_WAKE_BRIGHTEN_MS * 1000) != ESP_OK) {
        s_wake_brighten_pending = false;
        s_wake_brighten_req_us = 0;
        display_flash_watch_set_light_content(false);
        ui_screen_saver_apply_backlight();
    }
}

/* The light clock wallpaper has left the glass (the overlay was hidden and the
 * paint that follows shows dark menu content, the screen went off, or the overlay
 * was deleted by a style change).  A brighten that is still waiting for a paint
 * is meaningless from here on, and the detector flag would otherwise stay set and
 * turn the next wake-up into a false flash report. */
static void ui_screen_saver_drop_light_content(void)
{
    ui_screen_saver_cancel_wake_brighten();
    display_flash_watch_set_light_content(false);
}

static void ui_screen_saver_hide_overlay(void)
{
    const bool was_shown = s_overlay_shown;
    /* How long the clock was on the glass: a reveal dismissed straight away is
     * the blink the user reports, so its duration travels in the log. */
    const int64_t shown_ms = (was_shown && s_reveal_at_us != 0)
                                 ? (esp_timer_get_time() - s_reveal_at_us) / 1000
                                 : 0;
    if (s_overlay != NULL) {
        lv_obj_add_flag(s_overlay, LV_OBJ_FLAG_HIDDEN);
    }
    s_overlay_shown = false;
    /* The menu is back on screen: camera snapshots may run again. */
    ui_cameras_page_set_suspended(false);
    if (was_shown && !s_screen_off) {
        /* The light wallpaper is still what the panel is showing while the menu
         * is painted, so the brighten waits for that paint.  Raising the level
         * here is what the user saw as a light-blue flash on every wake; the
         * flash detector keeps its light-content flag until the deferred restore
         * runs, so a brighten that still beats the paint is reported instead of
         * passing as an innocent "backlight" line. */
        ui_screen_saver_schedule_wake_brighten();
    } else if (!s_wake_brighten_pending) {
        /* No hide of ours owns the backlight right now: apply the level the
         * current state asks for (settings changes, night schedule, ...). */
        ui_screen_saver_apply_backlight();
    }
    if (was_shown) {
        ESP_LOGI(TAG_SCREEN_SAVER, "screensaver overlay hidden (brightness=%d%%, restored in %d ms)",
                 (int)s_brightness, SCREEN_SAVER_WAKE_BRIGHTEN_MS);
        display_flash_watch_note("saver:off");
        system_log_event("saver", "overlay hidden brightness=%d shown=%lldms", (int)s_brightness,
                         (long long)shown_ms);
        if (shown_ms > 0) {
            s_reveal_last_ms = shown_ms;
            if (shown_ms < SCREEN_SAVER_REVEAL_SETTLE_MS) {
                s_reveal_blip_count++;
                /* Note for the flash detector: this line lands next to whatever
                 * the detector reports, so a flash at this moment is explained by
                 * the reveal it interrupted. */
                display_flash_watch_note("saver:blip");
                ESP_LOGW(TAG_SCREEN_SAVER,
                         "saver: reveal blip - the clock was dismissed %lld ms after it appeared "
                         "(blip %u of %u reveals)",
                         (long long)shown_ms, (unsigned)s_reveal_blip_count, (unsigned)s_reveal_count);
                system_log_event("saver", "BLIP reveal dismissed after %lld ms (#%u of %u reveals)",
                                 (long long)shown_ms, (unsigned)s_reveal_blip_count,
                                 (unsigned)s_reveal_count);
            }
        }
        s_reveal_at_us = 0;
    }
}

/* Reveal counters for the diagnostics endpoint: how often the clock was revealed
 * and how often a wake cut the reveal short (the light-frame blink). */
void ui_screen_saver_get_reveal_stats(uint32_t *reveals, uint32_t *blips, int64_t *last_ms)
{
    if (reveals != NULL) {
        *reveals = s_reveal_count;
    }
    if (blips != NULL) {
        *blips = s_reveal_blip_count;
    }
    if (last_ms != NULL) {
        *last_ms = s_reveal_last_ms;
    }
}

void ui_screen_saver_handle_screen_clean(void)
{
    /* The overlay and its children are attached to the active screen and are
     * deleted by ui_pages_init()'s lv_obj_clean(). Drop every handle here so the
     * next tick rebuilds the overlay instead of writing into freed memory.
     * A running flip animation is cancelled first: it holds pointers into the
     * objects that are about to disappear.
     * The wallpaper buffer/dsc survive: they are owned by this module. */
    ui_screen_saver_flip_reset();
    s_overlay = NULL;
    s_time_label = NULL;
    s_date_label = NULL;
    s_bg_image = NULL;
    s_overlay_shown = false;
    /* The cleaned screen took the labels with it: forget the rendered text so
     * the rebuilt overlay writes the clock again. */
    s_clock_rendered[0] = '\0';
    s_date_rendered[0] = '\0';
    /* The cleaned screen starts as the dark menu: drop a brighten that is still
     * waiting for a paint and clear the light-content flag with it. */
    ui_screen_saver_cancel_wake_brighten();
    s_reveal_at_us = 0;
    display_flash_watch_set_light_content(false);
    /* A rebuilt menu may show a camera page again. */
    ui_cameras_page_set_suspended(false);
}

/* Centres the clock block horizontally, optionally pulled left by half of the
 * AM/PM badge so that the "HH:MM PM" group as a whole stays centred. */
static void ui_screen_saver_align_clock(int32_t shift)
{
    if (s_clock_style == APP_DISPLAY_SAVER_CLOCK_STYLE_FLIP) {
        if (s_flip_row != NULL) {
            lv_obj_set_x(s_flip_row, (APP_SCREEN_WIDTH - s_flip_row_w) / 2 - shift);
        }
    } else if (s_time_label != NULL) {
        lv_obj_align(s_time_label, LV_ALIGN_BOTTOM_MID, -shift, -APP_DISPLAY_SAVER_CLOCK_BOTTOM_GAP);
    }
}

/* One badge copy: same black plate, so the five of them overlap into a single
 * plate with visibly thicker letters. */
static void ui_screen_saver_ampm_style(lv_obj_t *label)
{
    lv_obj_set_style_text_font(label, APP_FONT_DISPLAY_28, LV_PART_MAIN);
    lv_obj_set_style_text_color(label, lv_color_hex(s_clock_color), LV_PART_MAIN);
    lv_obj_set_style_bg_color(label, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(label, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(label, APP_DISPLAY_SAVER_AMPM_RADIUS, LV_PART_MAIN);
    lv_obj_set_style_pad_all(label, APP_DISPLAY_SAVER_AMPM_PAD, LV_PART_MAIN);
    lv_obj_set_width(label, LV_SIZE_CONTENT);
    lv_obj_clear_flag(label, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
}

/* The badge always hangs off the clock base, so it survives both clock styles
 * and every re-centring of the digits. */
static lv_obj_t *ui_screen_saver_ampm_base(void)
{
    if (s_clock_style == APP_DISPLAY_SAVER_CLOCK_STYLE_FLIP) {
        return s_flip_row;
    }
    return s_time_label;
}

/* Small AM/PM badge for the 12 hour format, right next to the digits on a black
 * plate of its own, so the clock reads like any digital clock even over a bright
 * wallpaper. Hidden in the 24 hour format. */
static void ui_screen_saver_update_ampm(int hour24)
{
    if (s_ampm_copies[0] == NULL) {
        return;
    }

    uint8_t state = s_clock_24h ? 2 : (hour24 < 12 ? 0 : 1);
    if (state == s_ampm_state) {
        for (int i = 0; i < AMPM_COPY_COUNT; i++) {
            lv_obj_set_style_text_color(s_ampm_copies[i], lv_color_hex(s_clock_color), LV_PART_MAIN);
        }
        return;
    }
    s_ampm_state = state;

    if (state == 2) {
        for (int i = 0; i < AMPM_COPY_COUNT; i++) {
            lv_obj_add_flag(s_ampm_copies[i], LV_OBJ_FLAG_HIDDEN);
        }
        ui_screen_saver_align_clock(0);
        return;
    }

    lv_obj_t *base = ui_screen_saver_ampm_base();
    if (base == NULL) {
        return;
    }

    for (int i = 0; i < AMPM_COPY_COUNT; i++) {
        lv_label_set_text(s_ampm_copies[i], state == 0 ? "AM" : "PM");
        ui_screen_saver_ampm_style(s_ampm_copies[i]);
        lv_obj_clear_flag(s_ampm_copies[i], LV_OBJ_FLAG_HIDDEN);
    }

    /* The badge width is only known once the new text has been laid out. */
    lv_obj_update_layout(s_ampm_copies[0]);
    ui_screen_saver_align_clock(
        (lv_obj_get_width(s_ampm_copies[0]) + 2 * APP_DISPLAY_SAVER_AMPM_WEIGHT +
            APP_DISPLAY_SAVER_AMPM_GAP) / 2);

    for (int i = 0; i < AMPM_COPY_COUNT; i++) {
        lv_obj_align_to(s_ampm_copies[i], base, LV_ALIGN_OUT_RIGHT_MID,
            APP_DISPLAY_SAVER_AMPM_GAP + s_ampm_copy_ofs[i][0] * APP_DISPLAY_SAVER_AMPM_WEIGHT,
            s_ampm_copy_ofs[i][1] * APP_DISPLAY_SAVER_AMPM_WEIGHT);
    }
}

static void ui_screen_saver_update_clock(void)
{
    if (s_overlay == NULL || s_date_label == NULL) {
        return;
    }

    time_t now = time(NULL);
    struct tm info = {0};
    localtime_r(&now, &info);

    int hour = info.tm_hour;
    if (!s_clock_24h) {
        hour = hour % 12;
        if (hour == 0) {
            hour = 12;
        }
    }

    if (s_clock_style == APP_DISPLAY_SAVER_CLOCK_STYLE_FLIP) {
        /* Flipping cards show HH:MM only: a folding seconds digit would repaint
         * the panel once a second and cost more than it is worth. */
        ui_screen_saver_flip_update(hour, info.tm_min);
    } else if (s_time_label != NULL) {
        char time_buf[16] = {0};
        if (s_show_seconds) {
            snprintf(time_buf, sizeof(time_buf), "%02d:%02d:%02d", hour, info.tm_min, info.tm_sec);
        } else {
            snprintf(time_buf, sizeof(time_buf), "%02d:%02d", hour, info.tm_min);
        }

        if (strcmp(time_buf, s_clock_rendered) != 0) {
            snprintf(s_clock_rendered, sizeof(s_clock_rendered), "%s", time_buf);
            lv_label_set_text(s_time_label, time_buf);
        }
        lv_obj_set_style_text_color(s_time_label, lv_color_hex(s_clock_color), LV_PART_MAIN);
    }

    if (s_show_date) {
        char date_buf[32] = {0};
        snprintf(
            date_buf,
            sizeof(date_buf),
            "%02d.%02d.%04d",
            info.tm_mday,
            info.tm_mon + 1,
            info.tm_year + 1900);
        if (strcmp(date_buf, s_date_rendered) != 0) {
            snprintf(s_date_rendered, sizeof(s_date_rendered), "%s", date_buf);
            lv_label_set_text(s_date_label, date_buf);
        }
        lv_obj_clear_flag(s_date_label, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_date_label, LV_OBJ_FLAG_HIDDEN);
    }

    if (s_time_label != NULL) {
        /* A NULL label happens in flip mode, where the digits live in cards. */
        lv_obj_set_style_text_color(s_time_label, lv_color_hex(s_clock_color), LV_PART_MAIN);
    }
    lv_obj_set_style_text_color(s_date_label, lv_color_hex(s_date_color), LV_PART_MAIN);
    ui_screen_saver_update_ampm(info.tm_hour);
}

/* Deletes the whole clock overlay. Only call from the LVGL task. */
static void ui_screen_saver_destroy_overlay(void)
{
    ui_screen_saver_flip_reset();
    if (s_overlay != NULL) {
        lv_obj_delete(s_overlay);
    }
    s_overlay = NULL;
    s_time_label = NULL;
    /* The labels are gone: the next overlay must be written from scratch. */
    s_clock_rendered[0] = '\0';
    s_date_rendered[0] = '\0';
    for (int i = 0; i < AMPM_COPY_COUNT; i++) {
        s_ampm_copies[i] = NULL;
    }
    s_ampm_state = 2;
    s_date_label = NULL;
    s_bg_image = NULL;
    s_overlay_shown = false;
}

static void ui_screen_saver_free_wallpaper(void)
{
    if (s_wallpaper_data != NULL) {
        heap_caps_free(s_wallpaper_data);
        s_wallpaper_data = NULL;
    }
    memset(&s_wallpaper_dsc, 0, sizeof(s_wallpaper_dsc));
    s_wallpaper_loaded = false;

    if (s_bg_image != NULL) {
        lv_image_set_src(s_bg_image, NULL);
        lv_obj_add_flag(s_bg_image, LV_OBJ_FLAG_HIDDEN);
    }

    /* Page backgrounds crop the frame above: without it they must fall back to
     * their plain background colour. */
    ui_page_style_reload_wallpaper();
}

const char *ui_screen_saver_wallpaper_path(void)
{
    return sd_card_is_mounted() ? APP_SD_WALLPAPER_PATH : APP_WALLPAPER_PATH;
}

const char *ui_screen_saver_wallpaper_tmp_path(void)
{
    return sd_card_is_mounted() ? APP_SD_WALLPAPER_TMP_PATH : APP_WALLPAPER_TMP_PATH;
}

bool ui_screen_saver_wallpaper_on_card(void)
{
    return sd_card_is_mounted();
}

bool ui_screen_saver_wallpaper_present(void)
{
    return s_wallpaper_loaded;
}

/* ---------------------------------------------------------------------------
 * Wallpaper locks
 *
 * Two locks are involved and they guard different things:
 *
 *   - s_wallpaper_mutex serialises whole transactions (read a frame from the
 *     storage, publish it, copy it to the other store).  It is held across the
 *     file I/O on purpose: 1.2 MB to or from a slow card takes hundreds of
 *     milliseconds and two transactions must not interleave on the same file.
 *   - the display lock only covers the LVGL side of a transaction, where the
 *     frame pointer is exchanged and the image objects are re-pointed.  That
 *     part takes microseconds, and keeping it separate is the whole point: the
 *     panel used to hold the LVGL lock for the entire 1.2 MB read, so a slow
 *     card froze the interface for as long as the read took.
 *
 * Order is always mutex -> display lock, never the other way round.
 * ------------------------------------------------------------------------- */

/* One full RGB565 frame, preferably from PSRAM: internal RAM cannot hold it
 * next to the LVGL draw buffers. */
static uint8_t *wallpaper_alloc_frame(bool allow_internal)
{
    uint8_t *data = NULL;
#if defined(CONFIG_SPIRAM) && CONFIG_SPIRAM
    data = (uint8_t *)heap_caps_malloc(APP_WALLPAPER_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#endif
    if (data == NULL && allow_internal) {
        data = (uint8_t *)heap_caps_malloc(APP_WALLPAPER_BYTES, MALLOC_CAP_8BIT);
    }
    return data;
}

static void wallpaper_mutex_release(void)
{
    if (s_wallpaper_mutex != NULL) {
        xSemaphoreGive(s_wallpaper_mutex);
    }
}

/* Takes the transaction mutex, creating it on first use.  The display lock
 * doubles as the creation guard, because it exists long before the first
 * wallpaper job does. */
static bool wallpaper_mutex_acquire(uint32_t timeout_ms)
{
    if (s_wallpaper_mutex == NULL) {
        if (!display_lock(WALLPAPER_DISPLAY_LOCK_MS)) {
            return false;
        }
        if (s_wallpaper_mutex == NULL) {
            s_wallpaper_mutex = xSemaphoreCreateMutex();
        }
        const bool created = (s_wallpaper_mutex != NULL);
        display_unlock();
        if (!created) {
            ESP_LOGE(TAG_SCREEN_SAVER, "wallpaper mutex alloc failed");
            return false;
        }
    }

    return xSemaphoreTake(s_wallpaper_mutex, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}

/* True when the file exists and holds exactly one full frame. */
static bool wallpaper_file_valid(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        return false;
    }
    bool valid = false;
    if (fseek(f, 0, SEEK_END) == 0 && ftell(f) == (long)APP_WALLPAPER_BYTES) {
        valid = true;
    }
    fclose(f);
    return valid;
}

/* Writes `frame` to `path` through a temporary file: used both for the first
 * copy onto a card and for the rescue copy back to flash.  `frame` is a
 * snapshot, so the caller does not have to hold the display lock here. */
static bool wallpaper_store_buffer(const uint8_t *frame, const char *tmp_path, const char *path)
{
    if (frame == NULL) {
        return false;
    }

    FILE *f = fopen(tmp_path, "wb");
    if (f == NULL) {
        return false;
    }

    bool ok = fwrite(frame, 1, APP_WALLPAPER_BYTES, f) == APP_WALLPAPER_BYTES;
    if (fclose(f) != 0) {
        ok = false;
    }

    /* The rename target must not exist: FatFs refuses to replace a file, while
     * LittleFS happens to overwrite. The pre-check limits when this runs, so
     * the unlink is only insurance against a racing writer. */
    if (ok) {
        remove(path);
        ok = (rename(tmp_path, path) == 0);
    }
    if (!ok) {
        remove(tmp_path);
        return false;
    }
    return true;
}

/* Reads one frame from the active store, falling back to the other one, into a
 * fresh buffer.  Pure file I/O: no lock is needed and none is taken, which is
 * what lets the caller keep the display lock out of the read. */
static uint8_t *wallpaper_read_frame(void)
{
    /* The frame is normally on the card, but the storage sync may not have moved
     * it there yet when the UI comes up, so both places are tried. */
    const bool on_card = sd_card_is_mounted();
    FILE *f = fopen(on_card ? APP_SD_WALLPAPER_PATH : APP_WALLPAPER_PATH, "rb");
    if (f == NULL) {
        f = fopen(on_card ? APP_WALLPAPER_PATH : APP_SD_WALLPAPER_PATH, "rb");
    }
    if (f == NULL) {
        return NULL;
    }

    uint8_t *data = NULL;
    do {
        if (fseek(f, 0, SEEK_END) != 0) {
            break;
        }
        long len = ftell(f);
        if (len != (long)APP_WALLPAPER_BYTES) {
            ESP_LOGW(TAG_SCREEN_SAVER, "wallpaper size %ld != expected %u", len, (unsigned)APP_WALLPAPER_BYTES);
            break;
        }
        rewind(f);

        data = wallpaper_alloc_frame(true);
        if (data == NULL) {
            ESP_LOGW(TAG_SCREEN_SAVER, "wallpaper buffer alloc failed");
            break;
        }

        /* Reads a whole 1024x600 frame off the filesystem straight into PSRAM -
         * the largest single burst the UI itself performs, so it is bracketed
         * for the DSI monitor. */
        dsi_bus_activity_begin(DSI_BUS_WALLPAPER);
        size_t got = fread(data, 1, APP_WALLPAPER_BYTES, f);
        dsi_bus_activity_end(DSI_BUS_WALLPAPER);
        if (got != APP_WALLPAPER_BYTES) {
            heap_caps_free(data);
            data = NULL;
            ESP_LOGW(TAG_SCREEN_SAVER, "wallpaper read short: %u/%u", (unsigned)got, (unsigned)APP_WALLPAPER_BYTES);
            break;
        }
    } while (0);
    fclose(f);

    return data;
}

/* Points the screensaver and every page background at `data`; a NULL frame
 * clears the picture.  LVGL side of a transaction: the caller must hold the
 * display lock and takes ownership of `data`. */
static void wallpaper_publish(uint8_t *data)
{
    if (data == NULL) {
        ui_screen_saver_free_wallpaper();
        return;
    }

    /* Publish the freshly read frame first and only then release the previous
     * buffer: the old frame is still referenced by the LVGL objects (screensaver
     * overlay and page backgrounds) until they are re-pointed below, and freeing
     * it up-front left the render task drawing freed memory. */
    uint8_t *previous = s_wallpaper_data;
    s_wallpaper_data = data;

    s_wallpaper_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    s_wallpaper_dsc.header.cf = LV_COLOR_FORMAT_RGB565;
    s_wallpaper_dsc.header.flags = 0;
    s_wallpaper_dsc.header.w = APP_SCREEN_WIDTH;
    s_wallpaper_dsc.header.h = APP_SCREEN_HEIGHT;
    s_wallpaper_dsc.header.stride = APP_SCREEN_WIDTH * 2;
    s_wallpaper_dsc.data_size = APP_WALLPAPER_BYTES;
    s_wallpaper_dsc.data = s_wallpaper_data;
    s_wallpaper_loaded = true;

    if (s_bg_image != NULL) {
        lv_image_set_src(s_bg_image, &s_wallpaper_dsc);
        lv_obj_clear_flag(s_bg_image, LV_OBJ_FLAG_HIDDEN);
    }

    /* Page backgrounds that use the wallpaper point at a crop of the frame. */
    ui_page_style_reload_wallpaper();

    if (previous != NULL) {
        heap_caps_free(previous);
    }
}

void ui_screen_saver_wallpaper_preload(void)
{
    if (s_wallpaper_pending == NULL) {
        s_wallpaper_pending = wallpaper_read_frame();
    }
}

/* Copy of the live frame for the storage sync.  The copy itself is taken under
 * the display lock, so a concurrent publish cannot free the buffer mid-memcpy,
 * and the caller works on the copy afterwards without holding any lock. */
static const uint8_t *wallpaper_snapshot(void)
{
    if (!display_lock(WALLPAPER_DISPLAY_LOCK_MS)) {
        ESP_LOGW(TAG_SCREEN_SAVER, "wallpaper snapshot skipped: display lock busy");
        return NULL;
    }

    const uint8_t *frame = NULL;
    if (s_wallpaper_loaded && s_wallpaper_data != NULL) {
        if (s_wallpaper_stage == NULL) {
            s_wallpaper_stage = wallpaper_alloc_frame(false);
        }
        if (s_wallpaper_stage == NULL) {
            ESP_LOGW(TAG_SCREEN_SAVER, "wallpaper snapshot alloc failed");
        } else {
            dsi_bus_activity_begin(DSI_BUS_WALLPAPER);
            memcpy(s_wallpaper_stage, s_wallpaper_data, APP_WALLPAPER_BYTES);
            dsi_bus_activity_end(DSI_BUS_WALLPAPER);
            frame = s_wallpaper_stage;
        }
    }

    display_unlock();
    return frame;
}

static void wallpaper_snapshot_release(void)
{
    if (s_wallpaper_stage != NULL) {
        heap_caps_free(s_wallpaper_stage);
        s_wallpaper_stage = NULL;
    }
}

const lv_image_dsc_t *ui_screen_saver_wallpaper_dsc(void)
{
    if (!s_wallpaper_loaded || s_wallpaper_data == NULL) {
        return NULL;
    }
    return &s_wallpaper_dsc;
}

void ui_screen_saver_reload_wallpaper(void)
{
    /* The API handlers run on the HTTP task.  The frame is read from the
     * storage first, with no lock held, and only the swap of the wallpaper
     * pointer happens under the display lock. */
    if (!wallpaper_mutex_acquire(WALLPAPER_JOB_LOCK_MS)) {
        ESP_LOGW(TAG_SCREEN_SAVER, "wallpaper reload skipped: busy");
        return;
    }

    uint8_t *data = wallpaper_read_frame();
    if (display_lock(WALLPAPER_DISPLAY_LOCK_MS)) {
        wallpaper_publish(data);
        display_unlock();
    } else {
        if (data != NULL) {
            heap_caps_free(data);
        }
        ESP_LOGW(TAG_SCREEN_SAVER, "wallpaper reload: display lock busy");
    }

    wallpaper_mutex_release();
}

void ui_screen_saver_wallpaper_sync(sd_card_event_t event)
{
    /* Serialised against ui_screen_saver_reload_wallpaper(), which replaces the
     * very frame that is handed to the card here. */
    if (!wallpaper_mutex_acquire(WALLPAPER_JOB_LOCK_MS)) {
        ESP_LOGW(TAG_SCREEN_SAVER, "wallpaper storage sync skipped: busy");
        return;
    }

    /* Keep a copy to write out; from here on the card I/O runs with no lock
     * held at all, so a full frame write no longer stalls the interface. */
    const uint8_t *frame = wallpaper_snapshot();

    if (event == SD_CARD_EVENT_MOUNTED && sd_card_is_mounted()) {
        if (!wallpaper_file_valid(APP_SD_WALLPAPER_PATH) && frame != NULL) {
            if (wallpaper_store_buffer(frame, APP_SD_WALLPAPER_TMP_PATH, APP_SD_WALLPAPER_PATH)) {
                ESP_LOGI(TAG_SCREEN_SAVER, "wallpaper copied to the microSD card");
            } else {
                ESP_LOGW(TAG_SCREEN_SAVER, "wallpaper copy to the microSD card failed");
            }
        }
        /* Single copy: while a card is in, the frame lives on it. */
        if (wallpaper_file_valid(APP_SD_WALLPAPER_PATH) &&
            remove(APP_WALLPAPER_PATH) == 0) {
            ESP_LOGI(TAG_SCREEN_SAVER, "wallpaper dropped from internal flash: the card holds the frame");
        }
    } else if (event == SD_CARD_EVENT_RELEASING && frame != NULL) {
        /* The card is still readable here, which is the point of this event:
         * without the copy back a card-less reboot would lose the frame. */
        if (!wallpaper_file_valid(APP_WALLPAPER_PATH) &&
            wallpaper_store_buffer(frame, APP_WALLPAPER_TMP_PATH, APP_WALLPAPER_PATH)) {
            ESP_LOGI(TAG_SCREEN_SAVER, "wallpaper copied back to internal flash");
        }
    }

    wallpaper_snapshot_release();
    wallpaper_mutex_release();
}

/* Dims the picture behind the clock by exactly the same mechanism a page
 * background uses: a black recolor layer on top of the image itself.  The clock
 * and date labels are separate objects, so they keep their own colour and stay
 * readable while the wallpaper gets darker. */
static void ui_screen_saver_apply_wallpaper_dim(void)
{
    if (s_bg_image == NULL) {
        return;
    }
    lv_obj_set_style_image_recolor(s_bg_image, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_image_recolor_opa(s_bg_image, ui_screen_saver_wallpaper_dim_opa(), LV_PART_MAIN);
}

static void ui_screen_saver_ensure_overlay(void)
{
    if (s_overlay != NULL) {
        return;
    }

    lv_obj_t *screen = lv_scr_act();
    if (screen == NULL) {
        return;
    }

    const int64_t build_start_ms = esp_timer_get_time() / 1000;

    s_overlay = lv_obj_create(screen);
    lv_obj_remove_style_all(s_overlay);
    lv_obj_set_size(s_overlay, APP_SCREEN_WIDTH, APP_SCREEN_HEIGHT);
    lv_obj_set_pos(s_overlay, 0, 0);
    lv_obj_clear_flag(s_overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_overlay, LV_OBJ_FLAG_CLICKABLE);
    /* Built hidden: the overlay is normally created ahead of time at start-up
     * (ui_screen_saver_init) so the first timeout only has to flip a flag, and
     * ui_screen_saver_show_overlay() is the only thing that makes it visible. */
    lv_obj_add_flag(s_overlay, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_bg_color(s_overlay, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_overlay, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_overlay, 0, LV_PART_MAIN);

    s_bg_image = lv_image_create(s_overlay);
    lv_obj_remove_style_all(s_bg_image);
    lv_obj_set_size(s_bg_image, APP_SCREEN_WIDTH, APP_SCREEN_HEIGHT);
    lv_obj_set_pos(s_bg_image, 0, 0);
    lv_obj_add_flag(s_bg_image, LV_OBJ_FLAG_HIDDEN);
    ui_screen_saver_apply_wallpaper_dim();
    if (s_wallpaper_loaded) {
        lv_image_set_src(s_bg_image, &s_wallpaper_dsc);
        lv_obj_clear_flag(s_bg_image, LV_OBJ_FLAG_HIDDEN);
    }

    if (s_clock_style == APP_DISPLAY_SAVER_CLOCK_STYLE_FLIP) {
        ui_screen_saver_flip_build(s_overlay);
    } else {
        s_time_label = lv_label_create(s_overlay);
        lv_obj_set_style_text_font(s_time_label, APP_FONT_CLOCK_84, LV_PART_MAIN);
        lv_obj_set_style_text_color(s_time_label, lv_color_hex(s_clock_color), LV_PART_MAIN);
        lv_obj_set_style_text_align(s_time_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
        lv_obj_set_width(s_time_label, LV_SIZE_CONTENT);
        /* Clock sits near the bottom edge with the date stacked above it. */
        lv_obj_align(s_time_label, LV_ALIGN_BOTTOM_MID, 0, -APP_DISPLAY_SAVER_CLOCK_BOTTOM_GAP);
    }

    /* 12 hour format badge; positioned and shown by the clock tick. Five copies
     * of the same text, each on its own black plate, one pixel apart: the result
     * is a bold "PM" on a single plate (see ui_screen_saver_ampm_style). */
    for (int i = 0; i < AMPM_COPY_COUNT; i++) {
        lv_obj_t *copy = lv_label_create(s_overlay);
        ui_screen_saver_ampm_style(copy);
        lv_label_set_text(copy, "PM");
        lv_obj_add_flag(copy, LV_OBJ_FLAG_HIDDEN);
        s_ampm_copies[i] = copy;
    }
    s_ampm_state = 2;

    s_date_label = lv_label_create(s_overlay);
    lv_obj_set_style_text_font(s_date_label, APP_FONT_TEXT_22, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_date_label, lv_color_hex(s_date_color), LV_PART_MAIN);
    lv_obj_set_style_text_align(s_date_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_width(s_date_label, LV_SIZE_CONTENT);
    lv_obj_align(s_date_label, LV_ALIGN_BOTTOM_MID, 0,
        -APP_DISPLAY_SAVER_CLOCK_BOTTOM_GAP - ui_screen_saver_clock_block_height() -
            APP_DISPLAY_SAVER_DATE_GAP);

    /* Building the overlay is the expensive part (objects, fonts, the 1.2 MB
     * wallpaper source).  It used to happen inside the saver timer callback on
     * the first timeout and held the LVGL lock for up to a second, so the
     * duration is logged here: it belongs to whichever call built it. */
    ESP_LOGI(TAG_SCREEN_SAVER, "screensaver overlay built in %lldms (wallpaper=%d style=%d)",
             (long long)(esp_timer_get_time() / 1000 - build_start_ms),
             s_wallpaper_loaded ? 1 : 0, (int)s_clock_style);
}

static void ui_screen_saver_show_overlay(void)
{
    const bool was_shown = s_overlay_shown;
    ui_screen_saver_ensure_overlay();
    if (s_overlay == NULL) {
        return;
    }

    /* The activity posters run on other tasks, so a wake can land between the
     * idle timeout check in the tick and this call.  Revealing anyway would put
     * the light wallpaper on the glass for a blink before that wake paints the
     * menu over it; leaving the menu where it is costs one tick instead. */
    if (!was_shown && !s_wake_to_screensaver &&
        display_ms_since_activity() < SCREEN_SAVER_REVEAL_GUARD_MS) {
        ESP_LOGI(TAG_SCREEN_SAVER, "saver: reveal postponed, activity %lld ms ago",
                 (long long)display_ms_since_activity());
        return;
    }

    ui_screen_saver_update_clock();
    /* Darken first, reveal second: the saver wallpaper is light, so revealing
     * it at menu brightness is what produces the full-screen flash. The
     * backlight is a whole-screen property, so the order of these two lines is
     * the whole difference between a flash and a clean transition. */
    s_overlay_shown = true;
    /* Nobody can see the camera tiles while the clock covers them. */
    ui_cameras_page_set_suspended(true);
    ui_screen_saver_apply_backlight_step();
    ui_screen_saver_cancel_wake_brighten();
    /* From here on the glass holds light content: a backlight rise before the
     * menu is painted again is a flash, and the detector reports it as such. */
    display_flash_watch_set_light_content(true);
    if (!was_shown) {
        /* The wallpaper is on the glass from the first frame: the dim recolor
         * (in force, in case the setting changed) is written before the overlay
         * is unhidden, so the clock never appears over a black page. */
        ui_screen_saver_apply_wallpaper_dim();
        s_reveal_at_us = esp_timer_get_time();
        s_reveal_count++;
    }
    lv_obj_clear_flag(s_overlay, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_overlay);
    if (!was_shown) {
        /* Overlay + backlight step at once: the closest thing to a full-screen
         * change this firmware does on its own, so it is worth a log line and a
         * note for the flash detector. */
        ESP_LOGI(TAG_SCREEN_SAVER,
                 "screensaver overlay shown (brightness=%d%% wallpaper=%d dim=%d%% reveal=immediate)",
                 (int)s_saver_brightness, s_wallpaper_loaded ? 1 : 0, (int)s_wallpaper_dim);
        display_flash_watch_note("saver:on");
        system_log_event("saver", "overlay shown brightness=%d wallpaper=%d dim=%d reveal=immediate",
                         (int)s_saver_brightness, s_wallpaper_loaded ? 1 : 0, (int)s_wallpaper_dim);
    }
}

/* Runs from display_note_activity(), i.e. on the very touch that wakes the panel
 * and *before* the driver restores the active brightness.  Clears
 * wake-to-screensaver so a touch dismisses the clock, and - the important part -
 * takes the backlight over: the light clock wallpaper has to be off the glass
 * before the panel gets brighter, otherwise the panel is raised to menu level
 * with the clock still displayed, which is the light-blue flash the user
 * reported on every wake.  Returning true tells the driver to skip its own
 * brightness restore; this module restores it once the menu is painted. */
static bool ui_screen_saver_on_activity(void)
{
    s_wake_to_screensaver = false;

    if (!s_overlay_shown || s_screen_off) {
        return false; /* nothing light on the glass: the driver may restore at once */
    }

    /* The lock is recursive and a touch calls this with the LVGL lock already
     * held, so the hide happens right here in the touch handler; wakes from the
     * HTTP or audio task wait for the renderer instead of tearing the overlay. */
    if (display_lock(SCREEN_SAVER_WAKE_LOCK_MS)) {
        ui_screen_saver_hide_overlay();
        display_unlock();
    } else {
        /* Lock busy: the clock stays up at saver brightness (dim, not a flash)
         * for at most one tick, which hides it and schedules the brighten. */
        ESP_LOGW(TAG_SCREEN_SAVER, "wake: display lock busy, overlay hidden on the next tick");
    }
    return true; /* the backlight is ours from here, dimmer only */
}

/* Minutes since midnight, or -1 while the clock has not been synced yet (the
 * night schedule never engages with an unknown time). */
static int ui_screen_saver_minutes_of_day(void)
{
    time_t now = time(NULL);
    struct tm info = {0};
    localtime_r(&now, &info);
    if (info.tm_year < 120) {
        return -1;
    }
    return (info.tm_hour * 60) + info.tm_min;
}

/* True inside [start, end). The window may cross midnight (22:00 -> 06:00). */
static bool ui_screen_saver_night_window_active(int minutes_of_day)
{
    if (!s_night_enabled || minutes_of_day < 0) {
        return false;
    }
    if (s_night_start_min == s_night_end_min) {
        return false;
    }
    if (s_night_start_min < s_night_end_min) {
        return minutes_of_day >= (int)s_night_start_min && minutes_of_day < (int)s_night_end_min;
    }
    return minutes_of_day >= (int)s_night_start_min || minutes_of_day < (int)s_night_end_min;
}

bool ui_screen_saver_night_active(void)
{
    return s_night_active;
}

static void ui_screen_saver_timer_cb(lv_timer_t *timer)
{
    system_log_note_lvgl_cb("ui_screen_saver_timer_cb");
    (void)timer;

    if (s_style_pending) {
        /* The clock style changed: rebuild the overlay here and not in the
         * request handler, LVGL objects are only touched from this task. */
        s_style_pending = false;
        ui_screen_saver_destroy_overlay();
        ui_screen_saver_drop_light_content();
        ui_screen_saver_apply_backlight();
    }

    if (s_wallpaper_dim_pending) {
        s_wallpaper_dim_pending = false;
        ui_screen_saver_apply_wallpaper_dim();
    }

    int64_t idle_ms = display_ms_since_activity();
    bool activity_recent = idle_ms < SCREEN_SAVER_TICK_MS;

    bool night_now = ui_screen_saver_night_window_active(ui_screen_saver_minutes_of_day());
    if (night_now != s_night_active) {
        s_night_active = night_now;
        s_night_wake_until_ms = 0;
        ESP_LOGI(TAG_SCREEN_SAVER, "night schedule %s (brightness=%u%%, wake=%us)",
                 night_now ? "on" : "off", (unsigned)s_night_brightness, (unsigned)s_night_wake_sec);
        if (!night_now) {
            /* Leaving the window: hand the backlight back to the normal logic. */
            s_screen_off = false;
            display_note_activity();
            ui_screen_saver_apply_backlight();
        }
    }

    if (s_night_active) {
        int64_t now_ms = esp_timer_get_time() / 1000;
        if (activity_recent) {
            s_night_wake_until_ms = now_ms + ((int64_t)s_night_wake_sec * 1000);
        }
        if (now_ms < s_night_wake_until_ms) {
            /* Temporary touch wake: the full UI stays usable, then the panel
             * drops back to the night level once the window expires. */
            s_screen_off = false;
            ui_screen_saver_hide_overlay();
            return;
        }

        s_night_wake_until_ms = 0;
        if (s_night_brightness == 0) {
            s_screen_off = true;
            ui_screen_saver_drop_light_content();
            ui_screen_saver_hide_overlay();
            (void)display_set_brightness_percent(0);
        } else {
            s_screen_off = false;
            if (s_screensaver_enabled) {
                ui_screen_saver_show_overlay();
            } else {
                ui_screen_saver_hide_overlay();
            }
            (void)display_set_brightness_percent((int)s_night_brightness);
        }
        return;
    }

    if (s_screen_off) {
        if (activity_recent) {
            s_screen_off = false;
            ui_screen_saver_apply_backlight();
            ESP_LOGI(TAG_SCREEN_SAVER, "wake (activity) brightness=%d%%", (int)s_brightness);
        } else {
            return;
        }
    }

    if (s_screen_off_enabled && s_screen_off_timeout_sec > 0 &&
        idle_ms >= (int64_t)s_screen_off_timeout_sec * 1000) {
        s_screen_off = true;
        s_wake_to_screensaver = false;
        ui_screen_saver_drop_light_content();
        ui_screen_saver_hide_overlay();
        (void)display_set_brightness_percent(0);
        ESP_LOGI(TAG_SCREEN_SAVER, "screen off (idle=%d ms)", (int)idle_ms);
        return;
    }

    if (!s_screensaver_enabled) {
        /* Screensaver fully disabled: hide the clock and never auto-show it. */
        ui_screen_saver_hide_overlay();
        s_wake_to_screensaver = false;
        return;
    }

    if (s_wake_to_screensaver) {
        /* Motion-sensor wake: keep the clock overlay visible until the
         * screen-off timeout above turns the backlight off. */
        if (!s_overlay_shown) {
            ESP_LOGI(TAG_SCREEN_SAVER, "wake-to-screensaver shown (idle=%d ms)", (int)idle_ms);
        }
        ui_screen_saver_show_overlay();
        return;
    }

    if (s_screensaver_enabled && s_screensaver_timeout_sec > 0 &&
        idle_ms >= (int64_t)s_screensaver_timeout_sec * 1000) {
        ui_screen_saver_show_overlay();
    } else {
        ui_screen_saver_hide_overlay();
    }
}

void ui_screen_saver_apply_settings(const runtime_settings_t *settings)
{
    if (settings == NULL) {
        return;
    }

    bool brightness_changed = (settings->display_brightness != s_brightness);
    bool saver_brightness_changed = (settings->display_saver_brightness != s_saver_brightness);
    bool off_just_disabled = (s_screen_off_enabled && !settings->display_screen_off_enabled);
    bool style_changed = (settings->display_saver_clock_style != s_clock_style);
    const bool wallpaper_dim_changed = (settings->display_saver_wallpaper_dim != s_wallpaper_dim);

    s_brightness = settings->display_brightness;
    s_saver_brightness = ui_screen_saver_clamp_brightness(settings->display_saver_brightness);
    s_screensaver_enabled = settings->display_screensaver_enabled;
    s_screensaver_timeout_sec = settings->display_screensaver_timeout_sec;
    s_screen_off_enabled = settings->display_screen_off_enabled;
    s_screen_off_timeout_sec = settings->display_screen_off_timeout_sec;
    s_clock_24h = settings->display_clock_24h;
    s_show_seconds = settings->display_saver_show_seconds;
    s_show_date = settings->display_saver_show_date;
    s_clock_style = settings->display_saver_clock_style;
    s_clock_color = settings->display_saver_clock_color;
    s_date_color = settings->display_saver_date_color;
    s_wallpaper_dim = (settings->display_saver_wallpaper_dim > APP_DISPLAY_SAVER_WALLPAPER_DIM_MAX)
        ? APP_DISPLAY_SAVER_WALLPAPER_DIM_MAX
        : settings->display_saver_wallpaper_dim;
    s_night_enabled = settings->display_night_mode_enabled;
    s_night_start_min = settings->display_night_start_min;
    s_night_end_min = settings->display_night_end_min;
    s_night_brightness = settings->display_night_brightness;
    s_night_wake_sec = settings->display_night_wake_sec;

    if (style_changed) {
        /* Rebuild on the next LVGL tick; the overlay may be on screen now. */
        s_style_pending = true;
    }

    if (wallpaper_dim_changed) {
        /* Restyle on the next LVGL tick, this is called from HTTP/MQTT too. */
        s_wallpaper_dim_pending = true;
    }

    /* Keep the wake/touch restore brightness in sync with the user setting. */
    display_set_active_brightness_percent((int)s_brightness);

    if (s_screen_off && (brightness_changed || saver_brightness_changed || off_just_disabled)) {
        /* Wake the panel so the user sees the effect of the change. */
        s_screen_off = false;
        display_note_activity();
    } else if (!s_screen_off) {
        /* Screen is on: re-apply the menu or screensaver backlight level. */
        ui_screen_saver_apply_backlight();
    }

    if (!s_screensaver_enabled) {
        /* Clock disabled: stop forcing it and let the timer hide the overlay. */
        s_wake_to_screensaver = false;
    }
}

void ui_screen_saver_wake(void)
{
    if (s_screen_off) {
        /* Panel is off: wake it to the screensaver clock and restart the idle
         * timer so the screen-off timeout counts from this moment. */
        s_screen_off = false;
        display_note_activity(); /* restores brightness + clears flag via cb */
        s_wake_to_screensaver = s_screensaver_enabled;
        ESP_LOGI(TAG_SCREEN_SAVER, "wake: off -> %s (brightness=%d%%)",
                 s_wake_to_screensaver ? "screensaver" : "menu", (int)s_brightness);
        return;
    }

    /* Panel already on. */
    if (!s_screensaver_enabled) {
        /* No clock configured: nothing to force on top of the UI. */
        ESP_LOGI(TAG_SCREEN_SAVER, "wake: ignored (screensaver disabled)");
        return;
    }

    int64_t idle_ms = display_ms_since_activity();

    /* Ignore only during the short grace period right after a real touch so
     * a motion sensor cannot yank the menu away. Outside that window every
     * wake (manual button or PIR) shows the clock immediately. */
    if (idle_ms < WAKE_GRACE_MS) {
        ESP_LOGI(TAG_SCREEN_SAVER, "wake: ignored (just touched, idle=%d ms)", (int)idle_ms);
        return;
    }

    if (s_wake_to_screensaver) {
        /* Clock already showing: keep it. Do NOT reset the idle timer so
         * repeated PIR motion cannot postpone the auto screen-off. */
        ESP_LOGI(TAG_SCREEN_SAVER, "wake: idle refresh (idle=%d ms)", (int)idle_ms);
    } else {
        s_wake_to_screensaver = true;
        ESP_LOGI(TAG_SCREEN_SAVER, "wake: show screensaver now (idle=%d ms)", (int)idle_ms);
    }
}

void ui_screen_saver_init(void)
{
    runtime_settings_t settings;
    runtime_settings_set_defaults(&settings);
    if (runtime_settings_load(&settings) != ESP_OK) {
        runtime_settings_set_defaults(&settings);
    }

    s_brightness = settings.display_brightness;
    s_saver_brightness = ui_screen_saver_clamp_brightness(settings.display_saver_brightness);
    s_screensaver_enabled = settings.display_screensaver_enabled;
    s_screensaver_timeout_sec = settings.display_screensaver_timeout_sec;
    s_screen_off_enabled = settings.display_screen_off_enabled;
    s_screen_off_timeout_sec = settings.display_screen_off_timeout_sec;
    s_clock_24h = settings.display_clock_24h;
    s_show_seconds = settings.display_saver_show_seconds;
    s_show_date = settings.display_saver_show_date;
    s_clock_style = settings.display_saver_clock_style;
    s_clock_color = settings.display_saver_clock_color;
    s_date_color = settings.display_saver_date_color;
    s_night_enabled = settings.display_night_mode_enabled;
    s_night_start_min = settings.display_night_start_min;
    s_night_end_min = settings.display_night_end_min;
    s_night_brightness = settings.display_night_brightness;
    s_night_wake_sec = settings.display_night_wake_sec;

    /* This module owns the backlight policy: stop the display driver's own
     * dim/off timer so the two policies cannot fight over the brightness. */
    display_set_power_policy_enabled(false);

    /* The deferred wake brighten runs from the esp_timer task (backlight only,
     * no LVGL) so it can land after the menu repaint without holding a lock. */
    if (s_wake_brighten_timer == NULL) {
        const esp_timer_create_args_t wake_brighten_args = {
            .callback = ui_screen_saver_wake_brighten_cb,
            .arg = NULL,
            .name = "saver_wake",
        };
        if (esp_timer_create(&wake_brighten_args, &s_wake_brighten_timer) != ESP_OK) {
            s_wake_brighten_timer = NULL;
            ESP_LOGW(TAG_SCREEN_SAVER, "wake brighten timer unavailable, restoring brightness immediately");
        }
    }

    /* Sync wake/touch restore brightness and register the activity callback
     * so a real touch dismisses a wake-to-screensaver overlay - and hands the
     * backlight to this module, which is what keeps the light wallpaper from
     * being lit up at menu level (see ui_screen_saver_on_activity). */
    display_set_active_brightness_percent((int)s_brightness);
    display_set_activity_callback(ui_screen_saver_on_activity);

    /* The frame was read by ui_screen_saver_wallpaper_preload() before the
     * display lock was taken; only the LVGL part is done here. */
    wallpaper_publish(s_wallpaper_pending);
    s_wallpaper_pending = NULL;

    /* Build the overlay now and keep it hidden.  Creating it lazily on the first
     * timeout held the LVGL lock for 113-971 ms inside
     * ui_screen_saver_timer_cb, which froze the panel at exactly the moment the
     * screensaver appeared - the moment the user sees as a screen flash.  With
     * the objects already in place, showing it is a flag flip plus the normal
     * full-screen repaint. */
    ui_screen_saver_ensure_overlay();

    if (s_timer == NULL) {
        s_timer = lv_timer_create(ui_screen_saver_timer_cb, SCREEN_SAVER_TICK_MS, NULL);
    }
}
