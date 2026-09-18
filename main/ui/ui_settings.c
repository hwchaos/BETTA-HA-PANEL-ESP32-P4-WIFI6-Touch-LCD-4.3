/* SPDX-License-Identifier: LicenseRef-FNCL-1.1
 * Copyright (c) 2026 Cpt_Kirk
 */
#include "ui/ui_settings.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_mac.h"
#include "esp_system.h"
#include "esp_timer.h"

#include "app_config.h"
#include "diag/system_log.h"
#if CONFIG_APP_FEATURE_LOCAL_CAMERA
#include "camera/local_camera.h"
#include "esp_log.h"
#endif
#include "net/wifi_mgr.h"
#include "settings/runtime_settings.h"
#include "ui/fonts/app_text_fonts.h"
#include "ui/ui_pages.h"
#include "ui/ui_screen_saver.h"
#include "ui/theme/theme_default.h"
#include "xiaozhi/xiaozhi_audio.h"

typedef enum {
    SET_CAT_SCREEN = 0,
    SET_CAT_AUDIO,
    SET_CAT_NET,
    SET_CAT_SYSTEM,
    SET_CAT_SD,
    SET_CAT_CAMERA,
    SET_CAT_COUNT
} ui_settings_cat_t;

static const char *const s_cat_names[SET_CAT_COUNT] = {
    "Ekran", "Audio", "Siec", "System", "SD", "Kamera",
};

static lv_obj_t *s_overlay = NULL;
static lv_obj_t *s_rail = NULL;
static lv_obj_t *s_content = NULL;
static lv_obj_t *s_chips[SET_CAT_COUNT] = {0};
static lv_obj_t *s_chip_labels[SET_CAT_COUNT] = {0};
static ui_settings_cat_t s_cat = SET_CAT_SCREEN;
static bool s_open = false;
static bool s_small = false;

/* The shared screensaver owns the backlight policy, so this screen edits the
 * runtime settings it consumes instead of keeping a private copy. */
static runtime_settings_t s_cfg;
static bool s_cfg_loaded = false;

static void st_cfg_load(void)
{
    if (s_cfg_loaded) {
        return;
    }
    runtime_settings_set_defaults(&s_cfg);
    if (runtime_settings_load(&s_cfg) != ESP_OK) {
        runtime_settings_set_defaults(&s_cfg);
    }
    s_cfg_loaded = true;
}

static void st_cfg_apply(void)
{
    ui_screen_saver_apply_settings(&s_cfg);
}

static void st_cfg_save(void)
{
    (void)runtime_settings_save(&s_cfg);
}

/* Display-only factory reset: the other runtime settings (network, topics,
 * themes) are left alone on purpose. */
static void st_cfg_set_display_defaults(void)
{
    s_cfg.display_brightness = APP_DISPLAY_ACTIVE_BRIGHTNESS_PERCENT;
    s_cfg.display_saver_brightness = APP_DISPLAY_SAVER_BRIGHTNESS_PERCENT;
    s_cfg.display_screensaver_enabled = true;
    s_cfg.display_screensaver_timeout_sec = APP_DISPLAY_SCREENSAVER_TIMEOUT_SEC;
    s_cfg.display_screen_off_enabled = APP_DISPLAY_SCREEN_OFF_ENABLED ? true : false;
    s_cfg.display_screen_off_timeout_sec = APP_DISPLAY_SCREEN_OFF_TIMEOUT_SEC;
    s_cfg.display_clock_24h = APP_DISPLAY_CLOCK_24H ? true : false;
    s_cfg.display_saver_show_seconds = APP_DISPLAY_SAVER_SHOW_SECONDS ? true : false;
    s_cfg.display_saver_show_date = APP_DISPLAY_SAVER_SHOW_DATE ? true : false;
    s_cfg.display_saver_clock_style = APP_DISPLAY_SAVER_CLOCK_STYLE_DEFAULT;
    s_cfg.display_saver_wallpaper_dim = APP_DISPLAY_SAVER_WALLPAPER_DIM_PERCENT;
    s_cfg.display_night_mode_enabled = APP_DISPLAY_NIGHT_MODE_ENABLED ? true : false;
    s_cfg.display_night_start_min = APP_DISPLAY_NIGHT_START_MIN;
    s_cfg.display_night_end_min = APP_DISPLAY_NIGHT_END_MIN;
    s_cfg.display_night_brightness = APP_DISPLAY_NIGHT_BRIGHTNESS_PERCENT;
    s_cfg.display_night_wake_sec = APP_DISPLAY_NIGHT_WAKE_SEC;
}

/* ------------------------------------------------------------------ */
/* Slider bookkeeping. Only one content build is alive at a time, so a */
/* small static pool is enough; entries are re-used on every rebuild.  */
/* ------------------------------------------------------------------ */
typedef enum { SET_SL_ACTIVE, SET_SL_DIM, SET_SL_NIGHT, SET_SL_VOLUME, SET_SL_WP_DIM } ui_slider_kind_t;
typedef struct {
    ui_slider_kind_t kind;
    lv_obj_t *slider;
    lv_obj_t *value_label;
} ui_slider_ctx_t;

static ui_slider_ctx_t s_sliders[5];
static uint8_t s_slider_count = 0;
static ui_slider_ctx_t *s_active_slider = NULL;

static void st_rebuild_content(void);

/* ------------------------------------------------------------------ */
/* Geometry / fonts                                                    */
/* ------------------------------------------------------------------ */
static lv_coord_t st_hdr_h(void)
{
    return s_small ? 52 : 80;
}

static lv_coord_t st_rail_w(void)
{
    return s_small ? 150 : 250;
}

static const lv_font_t *st_f_title(void)
{
    return s_small ? APP_FONT_TEXT_22 : APP_FONT_TEXT_28;
}

static const lv_font_t *st_f_cat(void)
{
    return s_small ? APP_FONT_TEXT_16 : APP_FONT_TEXT_20;
}

static const lv_font_t *st_f_row(void)
{
    return s_small ? APP_FONT_TEXT_16 : APP_FONT_TEXT_20;
}

static const lv_font_t *st_f_value(void)
{
    return s_small ? APP_FONT_TEXT_14 : APP_FONT_TEXT_20;
}

static const lv_font_t *st_f_note(void)
{
    return s_small ? APP_FONT_TEXT_12 : APP_FONT_TEXT_16;
}

static void st_label(lv_obj_t *label, const char *text, const lv_font_t *font, lv_color_t color)
{
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, font, LV_PART_MAIN);
    lv_obj_set_style_text_color(label, color, LV_PART_MAIN);
}

/* ------------------------------------------------------------------ */
/* Content primitives                                                  */
/* ------------------------------------------------------------------ */
static lv_obj_t *st_make_card(lv_obj_t *parent)
{
    lv_obj_t *card = lv_obj_create(parent);
    theme_default_style_card(card);
    lv_obj_set_width(card, LV_PCT(100));
    lv_obj_set_height(card, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(card, 12, LV_PART_MAIN);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    return card;
}

static lv_obj_t *st_card_title(lv_obj_t *card, const char *text)
{
    lv_obj_t *title = lv_label_create(card);
    st_label(title, text, st_f_row(), lv_color_hex(APP_UI_COLOR_TEXT_PRIMARY));
    return title;
}

/* A two-column row (label left / value right). Returns the value label. */
static lv_obj_t *st_info_row(lv_obj_t *card, const char *label_text, const char *value_text)
{
    lv_obj_t *row = lv_obj_create(card);
    lv_obj_remove_style_all(row);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(row, 12, LV_PART_MAIN);
    lv_obj_set_style_pad_all(row, 0, LV_PART_MAIN);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *label = lv_label_create(row);
    st_label(label, label_text, st_f_row(), lv_color_hex(APP_UI_COLOR_TEXT_PRIMARY));
    lv_obj_set_width(label, LV_PCT(46));
    lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);

    lv_obj_t *value = lv_label_create(row);
    st_label(value, value_text, st_f_value(), lv_color_hex(APP_UI_COLOR_TEXT_MUTED));
    lv_obj_set_width(value, LV_PCT(54));
    lv_label_set_long_mode(value, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(value, LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN);
    return value;
}

static lv_obj_t *st_action_button(lv_obj_t *parent, const char *text, lv_color_t bg)
{
    lv_obj_t *btn = lv_obj_create(parent);
    lv_obj_remove_style_all(btn);
    lv_obj_set_height(btn, LV_SIZE_CONTENT);
    lv_obj_set_width(btn, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(btn, bg, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(btn, 16, LV_PART_MAIN);
    lv_obj_set_style_pad_left(btn, 24, LV_PART_MAIN);
    lv_obj_set_style_pad_right(btn, 24, LV_PART_MAIN);
    lv_obj_set_style_pad_top(btn, 10, LV_PART_MAIN);
    lv_obj_set_style_pad_bottom(btn, 10, LV_PART_MAIN);
    lv_obj_set_style_bg_color(btn, lv_color_hex(APP_UI_COLOR_CARD_BG_ON), LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *label = lv_label_create(btn);
    st_label(label, text, st_f_row(), lv_color_white());
    lv_obj_center(label);
    return btn;
}

/* ------------------------------------------------------------------ */
/* Slider block: caption row + % value + slider                        */
/* ------------------------------------------------------------------ */
static void st_slider_value_changed_cb(lv_event_t *event)
{
    ui_slider_ctx_t *ctx = (ui_slider_ctx_t *)lv_event_get_user_data(event);
    if (ctx == NULL || ctx->value_label == NULL) {
        return;
    }
    char buf[8];
    snprintf(buf, sizeof(buf), "%d%%", (int)lv_slider_get_value(ctx->slider));
    lv_label_set_text(ctx->value_label, buf);
}

static void st_slider_release_cb(lv_event_t *event)
{
    ui_slider_ctx_t *ctx = (ui_slider_ctx_t *)lv_event_get_user_data(event);
    if (ctx == NULL || ctx->slider == NULL) {
        return;
    }
    const int value = (int)lv_slider_get_value(ctx->slider);

    switch (ctx->kind) {
    case SET_SL_VOLUME:
        (void)xz_audio_set_volume(value);
        break;
    case SET_SL_ACTIVE:
        s_cfg.display_brightness = (uint8_t)value;
        st_cfg_apply();
        break;
    case SET_SL_DIM:
        s_cfg.display_saver_brightness = (uint8_t)((value > APP_DISPLAY_SAVER_BRIGHTNESS_MAX_PERCENT)
                                                       ? APP_DISPLAY_SAVER_BRIGHTNESS_MAX_PERCENT
                                                       : value);
        st_cfg_apply();
        break;
    case SET_SL_WP_DIM:
        s_cfg.display_saver_wallpaper_dim = (uint8_t)value;
        st_cfg_apply();
        break;
    case SET_SL_NIGHT:
    default:
        s_cfg.display_night_brightness = (uint8_t)value;
        st_cfg_apply();
        break;
    }
}

static void st_add_slider_block(lv_obj_t *card, const char *caption, int value, int max, ui_slider_kind_t kind)
{
    lv_obj_t *row = lv_obj_create(card);
    lv_obj_remove_style_all(row);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(row, 12, LV_PART_MAIN);
    lv_obj_set_style_pad_all(row, 0, LV_PART_MAIN);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *caption_label = lv_label_create(row);
    st_label(caption_label, caption, st_f_row(), lv_color_hex(APP_UI_COLOR_TEXT_PRIMARY));
    lv_obj_set_width(caption_label, LV_PCT(70));

    lv_obj_t *value_label = lv_label_create(row);
    st_label(value_label, "", st_f_value(), lv_color_hex(APP_UI_COLOR_STATE_ON));
    lv_obj_set_width(value_label, LV_PCT(30));
    lv_obj_set_style_text_align(value_label, LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN);

    if (s_slider_count >= (sizeof(s_sliders) / sizeof(s_sliders[0]))) {
        return;
    }
    ui_slider_ctx_t *ctx = &s_sliders[s_slider_count++];
    ctx->kind = kind;

    lv_obj_t *slider = lv_slider_create(card);
    lv_obj_set_width(slider, LV_PCT(100));
    lv_obj_set_height(slider, 28);
    lv_slider_set_range(slider, 0, max);
    lv_slider_set_value(slider, value, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(slider, lv_color_hex(APP_UI_COLOR_LIGHT_TRACK_OFF), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(slider, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(slider, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(slider, lv_color_hex(APP_UI_COLOR_LIGHT_TRACK_ON), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(slider, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_radius(slider, LV_RADIUS_CIRCLE, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(slider, lv_color_hex(APP_UI_COLOR_LIGHT_KNOB_ON), LV_PART_KNOB);
    lv_obj_set_style_bg_opa(slider, LV_OPA_COVER, LV_PART_KNOB);
    lv_obj_set_style_pad_all(slider, 8, LV_PART_KNOB);

    ctx->slider = slider;
    ctx->value_label = value_label;

    char buf[8];
    snprintf(buf, sizeof(buf), "%d%%", value);
    lv_label_set_text(value_label, buf);

    lv_obj_add_event_cb(slider, st_slider_value_changed_cb, LV_EVENT_VALUE_CHANGED, ctx);
    lv_obj_add_event_cb(slider, st_slider_release_cb, LV_EVENT_RELEASED, ctx);

    if (kind == SET_SL_ACTIVE) {
        s_active_slider = ctx;
    }
}

/* ------------------------------------------------------------------ */
/* Brightness presets                                                  */
/* ------------------------------------------------------------------ */
static void st_preset_click_cb(lv_event_t *event)
{
    const int value = (int)(uintptr_t)lv_event_get_user_data(event);
    if (s_active_slider == NULL || s_active_slider->slider == NULL) {
        return;
    }
    lv_slider_set_value(s_active_slider->slider, value, LV_ANIM_OFF);

    s_cfg.display_brightness = (uint8_t)value;
    st_cfg_apply();

    if (s_active_slider->value_label != NULL) {
        char buf[8];
        snprintf(buf, sizeof(buf), "%d%%", value);
        lv_label_set_text(s_active_slider->value_label, buf);
    }
}

static void st_add_preset_row(lv_obj_t *card)
{
    lv_obj_t *row = lv_obj_create(card);
    lv_obj_remove_style_all(row);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(row, 8, LV_PART_MAIN);
    lv_obj_set_style_pad_all(row, 0, LV_PART_MAIN);
    lv_obj_set_style_text_color(row, lv_color_hex(APP_UI_COLOR_TEXT_MUTED), LV_PART_MAIN);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *hint = lv_label_create(row);
    st_label(hint, "Skroty:", st_f_note(), lv_color_hex(APP_UI_COLOR_TEXT_MUTED));

    const int presets[] = {10, 25, 50, 100};
    for (size_t i = 0; i < (sizeof(presets) / sizeof(presets[0])); i++) {
        char buf[8];
        snprintf(buf, sizeof(buf), "%d", presets[i]);
        lv_obj_t *btn = st_action_button(row, buf, lv_color_hex(APP_UI_COLOR_NAV_BTN_BG_ACTIVE));
        lv_obj_set_style_pad_left(btn, 16, LV_PART_MAIN);
        lv_obj_set_style_pad_right(btn, 16, LV_PART_MAIN);
        lv_obj_set_style_pad_top(btn, 6, LV_PART_MAIN);
        lv_obj_set_style_pad_bottom(btn, 6, LV_PART_MAIN);
        lv_obj_set_style_radius(btn, 12, LV_PART_MAIN);
        lv_obj_set_style_text_color(lv_obj_get_child(btn, 0), lv_color_white(), LV_PART_MAIN);
        lv_obj_add_event_cb(btn, st_preset_click_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)presets[i]);
    }
}

/* ------------------------------------------------------------------ */
/* Timeout dropdowns (wygaszacz + wylaczenie ekranu)                    */
/* ------------------------------------------------------------------ */
#define SET_SAVER_TIMEOUT_OPTIONS "Nigdy\n15 s\n30 s\n1 min\n2 min\n5 min"
static const uint32_t s_saver_timeout_sec[] = {0U, 15U, 30U, 60U, 120U, 300U};

#define SET_OFF_TIMEOUT_OPTIONS "Nigdy\n1 min\n5 min\n10 min\n30 min\n1 godz"
static const uint32_t s_off_timeout_sec[] = {0U, 60U, 300U, 600U, 1800U, 3600U};

static int st_nearest_index(const uint32_t *values, size_t count, uint32_t current)
{
    int best = 0;
    uint32_t best_delta = UINT32_MAX;
    for (size_t i = 0; i < count; i++) {
        uint32_t delta = (current > values[i]) ? (current - values[i]) : (values[i] - current);
        if (delta < best_delta) {
            best_delta = delta;
            best = i;
        }
    }
    return best;
}

static void st_saver_timeout_change_cb(lv_event_t *event)
{
    lv_obj_t *dropdown = lv_event_get_target(event);
    const int sel = lv_dropdown_get_selected(dropdown);
    if (sel < 0 || sel >= (int)(sizeof(s_saver_timeout_sec) / sizeof(s_saver_timeout_sec[0]))) {
        return;
    }
    s_cfg.display_screensaver_timeout_sec = s_saver_timeout_sec[sel];
    s_cfg.display_screensaver_enabled = (s_saver_timeout_sec[sel] > 0U);
    st_cfg_apply();
}

static void st_off_timeout_change_cb(lv_event_t *event)
{
    lv_obj_t *dropdown = lv_event_get_target(event);
    const int sel = lv_dropdown_get_selected(dropdown);
    if (sel < 0 || sel >= (int)(sizeof(s_off_timeout_sec) / sizeof(s_off_timeout_sec[0]))) {
        return;
    }
    s_cfg.display_screen_off_timeout_sec = s_off_timeout_sec[sel];
    s_cfg.display_screen_off_enabled = (s_off_timeout_sec[sel] > 0U);
    st_cfg_apply();
}

/* Shared dropdown row builder (caption left / dropdown right). */
static lv_obj_t *st_add_dropdown_row(lv_obj_t *card, const char *caption, const char *options,
    int selected, lv_event_cb_t cb, void *user_data)
{
    lv_obj_t *row = lv_obj_create(card);
    lv_obj_remove_style_all(row);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(row, 12, LV_PART_MAIN);
    lv_obj_set_style_pad_all(row, 0, LV_PART_MAIN);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *caption_label = lv_label_create(row);
    st_label(caption_label, caption, st_f_row(), lv_color_hex(APP_UI_COLOR_TEXT_PRIMARY));
    lv_obj_set_width(caption_label, LV_PCT(55));

    lv_obj_t *dropdown = lv_dropdown_create(row);
    lv_dropdown_set_options(dropdown, options);
    lv_dropdown_set_selected(dropdown, selected);
    lv_obj_set_width(dropdown, LV_PCT(45));
    lv_obj_set_height(dropdown, 40);
    lv_obj_set_style_text_font(dropdown, st_f_value(), LV_PART_MAIN);
    lv_obj_set_style_text_color(dropdown, lv_color_hex(APP_UI_COLOR_TEXT_PRIMARY), LV_PART_MAIN);
    lv_obj_set_style_border_width(dropdown, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(dropdown, lv_color_hex(APP_UI_COLOR_CONTENT_BORDER), LV_PART_MAIN);
    lv_obj_set_style_radius(dropdown, 12, LV_PART_MAIN);
    lv_obj_set_style_pad_left(dropdown, 12, LV_PART_MAIN);
    lv_obj_set_style_pad_right(dropdown, 12, LV_PART_MAIN);
    lv_obj_add_event_cb(dropdown, cb, LV_EVENT_VALUE_CHANGED, user_data);
    return dropdown;
}

static void st_add_off_timeout_dropdown(lv_obj_t *card)
{
    uint32_t current = s_cfg.display_screen_off_enabled ? s_cfg.display_screen_off_timeout_sec : 0U;
    (void)st_add_dropdown_row(card, "Wylacz ekran po", SET_OFF_TIMEOUT_OPTIONS,
        st_nearest_index(s_off_timeout_sec, sizeof(s_off_timeout_sec) / sizeof(s_off_timeout_sec[0]), current),
        st_off_timeout_change_cb, NULL);
}

static void st_add_saver_timeout_dropdown(lv_obj_t *card)
{
    uint32_t current = s_cfg.display_screensaver_enabled ? s_cfg.display_screensaver_timeout_sec : 0U;
    (void)st_add_dropdown_row(card, "Wygaszacz po", SET_SAVER_TIMEOUT_OPTIONS,
        st_nearest_index(s_saver_timeout_sec, sizeof(s_saver_timeout_sec) / sizeof(s_saver_timeout_sec[0]), current),
        st_saver_timeout_change_cb, NULL);
}

static const char *st_hour_options(void)
{
    static char buf[128];
    if (buf[0] != '\0') {
        return buf;
    }
    size_t off = 0;
    for (int i = 0; i < 24; i++) {
        int n = snprintf(buf + off, sizeof(buf) - off, (i == 23) ? "%d" : "%d\n", i);
        if (n <= 0) {
            break;
        }
        off += (size_t)n;
        if (off >= sizeof(buf)) {
            break;
        }
    }
    return buf;
}

static void st_night_hour_change_cb(lv_event_t *event)
{
    lv_obj_t *dropdown = lv_event_get_target(event);
    const int sel = lv_dropdown_get_selected(dropdown);
    if (sel < 0 || sel > 23) {
        return;
    }
    const bool is_end = (lv_event_get_user_data(event) != NULL);
    if (is_end) {
        s_cfg.display_night_end_min = (uint16_t)(sel * 60);
    } else {
        s_cfg.display_night_start_min = (uint16_t)(sel * 60);
    }
    st_cfg_apply();
}

typedef enum {
    SET_SW_SAVER,
    SET_SW_OFF,
    SET_SW_NIGHT,
    SET_SW_CLOCK24,
    SET_SW_SECONDS,
    SET_SW_DATE,
} ui_switch_field_t;

static void st_switch_row_cb(lv_event_t *event)
{
    lv_obj_t *sw = lv_event_get_target(event);
    const bool on = lv_obj_has_state(sw, LV_STATE_CHECKED);
    switch ((ui_switch_field_t)(intptr_t)lv_event_get_user_data(event)) {
    case SET_SW_SAVER:
        s_cfg.display_screensaver_enabled = on;
        break;
    case SET_SW_OFF:
        s_cfg.display_screen_off_enabled = on;
        break;
    case SET_SW_NIGHT:
        s_cfg.display_night_mode_enabled = on;
        break;
    case SET_SW_CLOCK24:
        s_cfg.display_clock_24h = on;
        break;
    case SET_SW_SECONDS:
        s_cfg.display_saver_show_seconds = on;
        break;
    case SET_SW_DATE:
    default:
        s_cfg.display_saver_show_date = on;
        break;
    }
    st_cfg_apply();
}

static void st_add_switch_row(lv_obj_t *card, const char *caption, bool checked, ui_switch_field_t field)
{
    lv_obj_t *row = lv_obj_create(card);
    lv_obj_remove_style_all(row);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(row, 12, LV_PART_MAIN);
    lv_obj_set_style_pad_all(row, 0, LV_PART_MAIN);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *caption_label = lv_label_create(row);
    st_label(caption_label, caption, st_f_row(), lv_color_hex(APP_UI_COLOR_TEXT_PRIMARY));
    lv_obj_set_width(caption_label, LV_PCT(55));

    lv_obj_t *sw = lv_switch_create(row);
    lv_obj_set_width(sw, 64);
    lv_obj_set_height(sw, 34);
    if (checked) {
        lv_obj_add_state(sw, LV_STATE_CHECKED);
    } else {
        lv_obj_remove_state(sw, LV_STATE_CHECKED);
    }
    lv_obj_add_event_cb(sw, st_switch_row_cb, LV_EVENT_VALUE_CHANGED, (void *)(intptr_t)field);
}

#define SET_CLOCK_STYLE_OPTIONS "Klasyczny\nAnimowany"

static void st_clock_style_change_cb(lv_event_t *event)
{
    lv_obj_t *dropdown = lv_event_get_target(event);
    const int sel = lv_dropdown_get_selected(dropdown);
    s_cfg.display_saver_clock_style = (sel == 1) ? APP_DISPLAY_SAVER_CLOCK_STYLE_FLIP
                                                 : APP_DISPLAY_SAVER_CLOCK_STYLE_CLASSIC;
    st_cfg_apply();
    st_cfg_save();
}



/* ------------------------------------------------------------------ */
/* Category content builders                                           */
/* ------------------------------------------------------------------ */
static void st_build_screen(lv_obj_t *parent)
{
    st_cfg_load();

    lv_obj_t *card = st_make_card(parent);
    st_card_title(card, "Jasnosc");
    st_add_slider_block(card, "Jasnosc aktywna", s_cfg.display_brightness, 100, SET_SL_ACTIVE);
    st_add_preset_row(card);

    lv_obj_t *card2 = st_make_card(parent);
    st_card_title(card2, "Wygaszacz");
    st_add_switch_row(card2, "Wlaczony", s_cfg.display_screensaver_enabled, SET_SW_SAVER);
    st_add_slider_block(card2, "Jasnosc wygaszacza", s_cfg.display_saver_brightness,
                        APP_DISPLAY_SAVER_BRIGHTNESS_MAX_PERCENT, SET_SL_DIM);
    st_add_slider_block(card2, "Przyciemnienie tapety", s_cfg.display_saver_wallpaper_dim,
                        APP_DISPLAY_SAVER_WALLPAPER_DIM_MAX, SET_SL_WP_DIM);
    st_add_saver_timeout_dropdown(card2);

    lv_obj_t *card3 = st_make_card(parent);
    st_card_title(card3, "Wylaczanie ekranu");
    st_add_switch_row(card3, "Wylaczaj ekran", s_cfg.display_screen_off_enabled, SET_SW_OFF);
    st_add_off_timeout_dropdown(card3);

    lv_obj_t *card4 = st_make_card(parent);
    st_card_title(card4, "Zegar na wygaszaczu");
    st_add_switch_row(card4, "Format 24 h", s_cfg.display_clock_24h, SET_SW_CLOCK24);
    st_add_switch_row(card4, "Pokaz sekundy", s_cfg.display_saver_show_seconds, SET_SW_SECONDS);
    st_add_switch_row(card4, "Pokaz date", s_cfg.display_saver_show_date, SET_SW_DATE);
    (void)st_add_dropdown_row(card4, "Styl zegara", SET_CLOCK_STYLE_OPTIONS,
        (s_cfg.display_saver_clock_style == APP_DISPLAY_SAVER_CLOCK_STYLE_FLIP) ? 1 : 0,
        st_clock_style_change_cb, NULL);

    lv_obj_t *card5 = st_make_card(parent);
    st_card_title(card5, "Tryb nocny");
    st_add_switch_row(card5, "Tryb nocny", s_cfg.display_night_mode_enabled, SET_SW_NIGHT);
    (void)st_add_dropdown_row(card5, "Poczatek (godz)", st_hour_options(),
        s_cfg.display_night_start_min / 60, st_night_hour_change_cb, NULL);
    (void)st_add_dropdown_row(card5, "Koniec (godz)", st_hour_options(),
        s_cfg.display_night_end_min / 60, st_night_hour_change_cb, (void *)(uintptr_t)1);
    st_add_slider_block(card5, "Jasnosc w nocy (0 = off)", s_cfg.display_night_brightness, 100, SET_SL_NIGHT);

    lv_obj_t *card6 = st_make_card(parent);
    st_card_title(card6, "Uwaga");
    lv_obj_t *note = lv_label_create(card6);
    st_label(note, "Ekran wygasza sie po okresie bez dotyku, a po dluzszym czasie calkowicie sie wylacza. "
        "W nocy ekran wylacza sie od razu po wygaszeniu. Dotkniecie ekranu przywraca jasnosc.",
        st_f_note(), lv_color_hex(APP_UI_COLOR_TEXT_MUTED));
    lv_label_set_long_mode(note, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(note, LV_PCT(100));
}

static void st_beep_cb(lv_event_t *event)
{
    LV_UNUSED(event);
    (void)xz_audio_test_beep();
}

static void st_build_audio(lv_obj_t *parent)
{
    lv_obj_t *card = st_make_card(parent);
    st_card_title(card, "Glosnosc");
    st_add_slider_block(card, "Poziom glosnosci", xz_audio_get_volume(), 100, SET_SL_VOLUME);

    lv_obj_t *card2 = st_make_card(parent);
    st_card_title(card2, "Test");
    lv_obj_t *row = lv_obj_create(card2);
    lv_obj_remove_style_all(row);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_all(row, 0, LV_PART_MAIN);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *beep = st_action_button(row, "Odtworz test", lv_color_hex(APP_UI_COLOR_OK));
    lv_obj_add_event_cb(beep, st_beep_cb, LV_EVENT_CLICKED, NULL);
}

static void st_net_refresh_cb(lv_event_t *event)
{
    LV_UNUSED(event);
    if (s_cat == SET_CAT_NET) {
        st_rebuild_content();
    }
}

static void st_build_net(lv_obj_t *parent)
{
    bool connected = wifi_mgr_is_connected();
    bool setup_ap = wifi_mgr_is_setup_ap_active();

    char ssid[APP_WIFI_SSID_MAX_LEN] = "-";
    int8_t rssi = 0;
    wifi_mgr_sta_ap_info_t ap = {0};
    if (wifi_mgr_get_sta_ap_info(&ap) == ESP_OK && ap.ssid[0] != '\0') {
        snprintf(ssid, sizeof(ssid), "%s", ap.ssid);
        rssi = ap.rssi;
    }

    char ip[64] = "-";
    (void)wifi_mgr_get_sta_ip(ip, sizeof(ip));

    lv_obj_t *card = st_make_card(parent);
    st_card_title(card, "Status polaczenia");

    const char *status_text = "Brak polaczenia";
    lv_color_t status_color = lv_color_hex(APP_UI_COLOR_ERROR);
    if (setup_ap) {
        status_text = "Tryb AP (konfiguracja)";
        status_color = lv_color_hex(APP_UI_COLOR_STATE_ON);
    } else if (connected) {
        status_text = "Polaczono";
        status_color = lv_color_hex(APP_UI_COLOR_OK);
    }

    lv_obj_t *status_value = st_info_row(card, "Stan", status_text);
    st_label(status_value, status_text, st_f_value(), status_color);

    st_info_row(card, "SSID", ssid);
    {
        char buf[32];
        snprintf(buf, sizeof(buf), "%d dBm", (int)rssi);
        st_info_row(card, "Sygnal", buf);
    }
    st_info_row(card, "IP", ip);

    lv_obj_t *card2 = st_make_card(parent);
    lv_obj_t *refresh = st_action_button(card2, "Odswiez", lv_color_hex(APP_UI_COLOR_NAV_BTN_BG_ACTIVE));
    lv_obj_add_event_cb(refresh, st_net_refresh_cb, LV_EVENT_CLICKED, NULL);
}

static void st_restart_cb(lv_event_t *event)
{
    LV_UNUSED(event);
    esp_restart();
}

static void st_reset_display_cb(lv_event_t *event)
{
    LV_UNUSED(event);
    st_cfg_load();
    st_cfg_set_display_defaults();
    st_cfg_apply();
    st_cfg_save();
    st_rebuild_content();
}

static void st_build_system(lv_obj_t *parent)
{
    const esp_app_desc_t *desc = esp_app_get_description();

    char lvgl_ver[32];
    snprintf(lvgl_ver, sizeof(lvgl_ver), "%d.%d.%d", (int)LVGL_VERSION_MAJOR, (int)LVGL_VERSION_MINOR,
        (int)LVGL_VERSION_PATCH);

    char mac[20];
    {
        uint8_t raw[6];
        esp_read_mac(raw, ESP_MAC_BASE);
        snprintf(mac, sizeof(mac), "%02X:%02X:%02X:%02X:%02X:%02X", raw[0], raw[1], raw[2], raw[3], raw[4], raw[5]);
    }

    int64_t uptime_secs = esp_timer_get_time() / 1000000LL;
    char uptime[64];
    {
        int64_t days = uptime_secs / 86400;
        int64_t rem = uptime_secs % 86400;
        int h = (int)(rem / 3600);
        int m = (int)((rem % 3600) / 60);
        int sec = (int)(rem % 60);
        snprintf(uptime, sizeof(uptime), "%lldd %02d:%02d:%02d", (long long)days, h, m, sec);
    }

    size_t psram_total = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
    size_t psram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    size_t ram_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);

    char psram[40];
    snprintf(psram, sizeof(psram), "%.1f / %.1f MB", (double)psram_free / (1024.0 * 1024.0),
        (double)psram_total / (1024.0 * 1024.0));
    char ram[40];
    snprintf(ram, sizeof(ram), "%.1f MB wolne", (double)ram_free / (1024.0 * 1024.0));

    lv_obj_t *card = st_make_card(parent);
    st_card_title(card, "Informacje");

    char buf[96];
    snprintf(buf, sizeof(buf), "%s (%s)", desc->version, desc->project_name);
    st_info_row(card, "Firmware", buf);
    st_info_row(card, "IDF", esp_get_idf_version());
    st_info_row(card, "LVGL", lvgl_ver);
    st_info_row(card, "MAC", mac);
    st_info_row(card, "Dzialanie", uptime);
    st_info_row(card, "PSRAM", psram);
    st_info_row(card, "RAM", ram);

    lv_obj_t *card2 = st_make_card(parent);
    st_card_title(card2, "Akcje");
    lv_obj_t *btn_row = lv_obj_create(card2);
    lv_obj_remove_style_all(btn_row);
    lv_obj_set_width(btn_row, LV_PCT(100));
    lv_obj_set_height(btn_row, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(btn_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(btn_row, 12, LV_PART_MAIN);
    lv_obj_set_style_pad_all(btn_row, 0, LV_PART_MAIN);
    lv_obj_clear_flag(btn_row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *reset = st_action_button(btn_row, "Przywroc ekran", lv_color_hex(APP_UI_COLOR_NAV_BTN_BG_ACTIVE));
    lv_obj_add_event_cb(reset, st_reset_display_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *restart = st_action_button(btn_row, "Restart urzadzenia", lv_color_hex(APP_UI_COLOR_ERROR));
    lv_obj_add_event_cb(restart, st_restart_cb, LV_EVENT_CLICKED, NULL);
}

static void st_build_sd(lv_obj_t *parent)
{
    lv_obj_t *card = st_make_card(parent);
    st_card_title(card, "Karta SD");

    lv_obj_t *note = lv_label_create(card);
    st_label(note, "Ten firmware nie posiada sterownika karty SD.", st_f_row(),
        lv_color_hex(APP_UI_COLOR_TEXT_PRIMARY));
    lv_label_set_long_mode(note, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(note, LV_PCT(100));

    lv_obj_t *note2 = lv_label_create(card);
    st_label(note2, "Obslugi karty mozna dodac w kolejnej wersji firmware. Wszystkie dane sa obecnie "
                    "przechowywane w pamieci wewnetrznej (littlefs).",
        st_f_note(), lv_color_hex(APP_UI_COLOR_TEXT_MUTED));
    lv_label_set_long_mode(note2, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(note2, LV_PCT(100));
}

#if CONFIG_APP_FEATURE_LOCAL_CAMERA
/* ------------------------------------------------------------------ */
/* Kamera (wbudowany modul MIPI-CSI OV5647)                            */
/* ------------------------------------------------------------------ */
#define SET_CAM_PREVIEW_W 320
/* 4:3, matching the default 1280x960 MIPI binning sensor mode, so the preview
 * is not stretched.  The sensor mode can be changed in Kconfig. */
#define SET_CAM_PREVIEW_H 240
#define SET_CAM_RESOLUTION_OPTIONS "Pelna\nPolowa 2x"

/* Value labels of the camera rows (refreshed after every change). */
static lv_obj_t *s_cam_status_value = NULL;
static lv_obj_t *s_cam_res_value = NULL;
static lv_obj_t *s_cam_jpeg_value = NULL;

/* The camera sliders work on raw values (1..64 / 10..95), so they keep their
 * own tiny context pool instead of the percent-only s_sliders[] one. */
typedef struct {
    lv_obj_t *slider;
    lv_obj_t *value_label;
} st_cam_slider_ctx_t;

static st_cam_slider_ctx_t s_cam_sliders[2];
static uint8_t s_cam_slider_count = 0;

/* Live preview: RGB565 PSRAM buffer copied by a one-second timer.  The buffer
 * is allocated on the first build and freed when the overlay is torn down. */
static lv_obj_t *s_cam_preview_img = NULL;
static lv_obj_t *s_cam_preview_note = NULL;
static uint8_t *s_cam_preview_buf = NULL;
static lv_timer_t *s_cam_preview_timer = NULL;
static lv_image_dsc_t s_cam_preview_dsc;

static void st_cam_apply(void)
{
    (void)local_camera_apply_settings(s_cfg.camera_enabled,
                                      s_cfg.camera_motion_wake,
                                      (uint8_t)s_cfg.camera_motion_threshold,
                                      (uint8_t)s_cfg.camera_jpeg_quality,
                                      s_cfg.camera_hflip,
                                      s_cfg.camera_vflip,
                                      s_cfg.camera_resolution);
}

static void st_cam_rows_refresh(void)
{
    if (s_cam_status_value != NULL) {
        const char *text = "Wylaczona";
        lv_color_t color = lv_color_hex(APP_UI_COLOR_TEXT_MUTED);
        if (local_camera_is_running()) {
            text = "Wlaczona";
            color = lv_color_hex(APP_UI_COLOR_STATE_ON);
        } else if (local_camera_width() <= 0) {
            /* The pipeline was never brought up: no sensor/CSI module. */
            text = "Brak modulu";
            color = lv_color_hex(APP_UI_COLOR_ERROR);
        }
        st_label(s_cam_status_value, text, st_f_value(), color);
    }

    if (s_cam_res_value != NULL) {
        char buf[32];
        if (local_camera_width() > 0) {
            snprintf(buf, sizeof(buf), "%d x %d", local_camera_width(), local_camera_height());
        } else {
            /* The frame size follows the sensor mode selected in Kconfig, so
             * report the mode instead of a hard-coded resolution. */
            snprintf(buf, sizeof(buf), "%s", (s_cfg.camera_resolution == 0) ? "Pelna" : "Polowa 2x");
        }
        lv_label_set_text(s_cam_res_value, buf);
    }

    if (s_cam_jpeg_value != NULL) {
        char buf[24];
        snprintf(buf, sizeof(buf), "Q %d", (int)local_camera_get_jpeg_quality());
        lv_label_set_text(s_cam_jpeg_value, buf);
    }
}

static void st_cam_refresh_cb(lv_event_t *event)
{
    LV_UNUSED(event);
    if (s_cat == SET_CAT_CAMERA) {
        st_rebuild_content();
    }
}

static void st_cam_enabled_cb(lv_event_t *event)
{
    lv_obj_t *sw = lv_event_get_target(event);
    s_cfg.camera_enabled = lv_obj_has_state(sw, LV_STATE_CHECKED);
    st_cam_apply();
    st_cfg_save();
    st_cam_rows_refresh();
}

static void st_cam_resolution_cb(lv_event_t *event)
{
    lv_obj_t *dropdown = lv_event_get_target(event);
    /* A resolution change restarts the pipeline inside apply_settings(). */
    s_cfg.camera_resolution = (lv_dropdown_get_selected(dropdown) == 1) ? 1 : 0;
    st_cam_apply();
    st_cfg_save();
    st_cam_rows_refresh();
}

static void st_cam_jpeg_quality_cb(lv_event_t *event)
{
    int value = (int)lv_slider_get_value(lv_event_get_target(event));
    if (value < 10) {
        value = 10;
    }
    if (value > 95) {
        value = 95;
    }
    s_cfg.camera_jpeg_quality = (uint8_t)value;
    local_camera_set_jpeg_quality((uint8_t)value);
    st_cfg_save();
    st_cam_rows_refresh();
}

static void st_cam_hflip_cb(lv_event_t *event)
{
    lv_obj_t *sw = lv_event_get_target(event);
    s_cfg.camera_hflip = lv_obj_has_state(sw, LV_STATE_CHECKED);
    local_camera_set_flip(s_cfg.camera_hflip, s_cfg.camera_vflip);
    st_cfg_save();
}

static void st_cam_vflip_cb(lv_event_t *event)
{
    lv_obj_t *sw = lv_event_get_target(event);
    s_cfg.camera_vflip = lv_obj_has_state(sw, LV_STATE_CHECKED);
    local_camera_set_flip(s_cfg.camera_hflip, s_cfg.camera_vflip);
    st_cfg_save();
}

static void st_cam_motion_wake_cb(lv_event_t *event)
{
    lv_obj_t *sw = lv_event_get_target(event);
    s_cfg.camera_motion_wake = lv_obj_has_state(sw, LV_STATE_CHECKED);
    local_camera_set_motion_wake(s_cfg.camera_motion_wake);
    st_cfg_save();
    st_cam_rows_refresh();
}

static void st_cam_motion_threshold_cb(lv_event_t *event)
{
    int value = (int)lv_slider_get_value(lv_event_get_target(event));
    if (value < 1) {
        value = 1;
    }
    if (value > 64) {
        value = 64;
    }
    s_cfg.camera_motion_threshold = (uint8_t)value;
    local_camera_set_motion_threshold((uint8_t)value);
    st_cfg_save();
}

/* Camera specific rows: st_add_switch_row()/st_add_slider_block() are enum
 * driven, so the camera builds its own switch/slider rows. */
static lv_obj_t *st_cam_switch_row(lv_obj_t *card, const char *caption, bool checked, lv_event_cb_t cb,
    void *user)
{
    lv_obj_t *row = lv_obj_create(card);
    lv_obj_remove_style_all(row);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(row, 12, LV_PART_MAIN);
    lv_obj_set_style_pad_all(row, 0, LV_PART_MAIN);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *caption_label = lv_label_create(row);
    st_label(caption_label, caption, st_f_row(), lv_color_hex(APP_UI_COLOR_TEXT_PRIMARY));
    lv_obj_set_width(caption_label, LV_PCT(55));

    lv_obj_t *sw = lv_switch_create(row);
    lv_obj_set_width(sw, 64);
    lv_obj_set_height(sw, 34);
    if (checked) {
        lv_obj_add_state(sw, LV_STATE_CHECKED);
    } else {
        lv_obj_remove_state(sw, LV_STATE_CHECKED);
    }
    lv_obj_add_event_cb(sw, cb, LV_EVENT_VALUE_CHANGED, user);
    return sw;
}

static void st_cam_slider_label_cb(lv_event_t *event)
{
    st_cam_slider_ctx_t *ctx = (st_cam_slider_ctx_t *)lv_event_get_user_data(event);
    if (ctx == NULL || ctx->slider == NULL || ctx->value_label == NULL) {
        return;
    }
    char buf[16];
    snprintf(buf, sizeof(buf), "%d", (int)lv_slider_get_value(ctx->slider));
    lv_label_set_text(ctx->value_label, buf);
}

/* cb is called on LV_EVENT_RELEASED (target = slider), so dragging does not
 * write the settings back to flash on every step. */
static void st_cam_slider_row(lv_obj_t *card, const char *caption, int value, int min, int max,
    lv_event_cb_t cb, void *user)
{
    lv_obj_t *row = lv_obj_create(card);
    lv_obj_remove_style_all(row);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(row, 12, LV_PART_MAIN);
    lv_obj_set_style_pad_all(row, 0, LV_PART_MAIN);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *caption_label = lv_label_create(row);
    st_label(caption_label, caption, st_f_row(), lv_color_hex(APP_UI_COLOR_TEXT_PRIMARY));
    lv_obj_set_width(caption_label, LV_PCT(70));

    lv_obj_t *value_label = lv_label_create(row);
    st_label(value_label, "", st_f_value(), lv_color_hex(APP_UI_COLOR_STATE_ON));
    lv_obj_set_width(value_label, LV_PCT(30));
    lv_obj_set_style_text_align(value_label, LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN);

    lv_obj_t *slider = lv_slider_create(card);
    lv_obj_set_width(slider, LV_PCT(100));
    lv_obj_set_height(slider, 28);
    lv_slider_set_range(slider, min, max);
    lv_slider_set_value(slider, value, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(slider, lv_color_hex(APP_UI_COLOR_LIGHT_TRACK_OFF), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(slider, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(slider, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(slider, lv_color_hex(APP_UI_COLOR_LIGHT_TRACK_ON), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(slider, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_radius(slider, LV_RADIUS_CIRCLE, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(slider, lv_color_hex(APP_UI_COLOR_LIGHT_KNOB_ON), LV_PART_KNOB);
    lv_obj_set_style_bg_opa(slider, LV_OPA_COVER, LV_PART_KNOB);
    lv_obj_set_style_pad_all(slider, 8, LV_PART_KNOB);

    char buf[16];
    snprintf(buf, sizeof(buf), "%d", value);
    lv_label_set_text(value_label, buf);

    if (s_cam_slider_count < (sizeof(s_cam_sliders) / sizeof(s_cam_sliders[0]))) {
        st_cam_slider_ctx_t *ctx = &s_cam_sliders[s_cam_slider_count++];
        ctx->slider = slider;
        ctx->value_label = value_label;
        lv_obj_add_event_cb(slider, st_cam_slider_label_cb, LV_EVENT_VALUE_CHANGED, ctx);
    }
    if (cb != NULL) {
        lv_obj_add_event_cb(slider, cb, LV_EVENT_RELEASED, user);
    }
}

static void st_cam_preview_timer_cb(lv_timer_t *timer)
{
    system_log_note_lvgl_cb("st_cam_preview_timer_cb");
    LV_UNUSED(timer);
    if (s_cam_preview_buf == NULL || s_cam_preview_img == NULL) {
        return;
    }
    if (local_camera_copy_scaled_rgb565(s_cam_preview_buf, SET_CAM_PREVIEW_W, SET_CAM_PREVIEW_H) != ESP_OK) {
        /* Kamera wylaczona albo brak gotowej klatki. */
        lv_obj_add_flag(s_cam_preview_img, LV_OBJ_FLAG_HIDDEN);
        if (s_cam_preview_note != NULL) {
            lv_obj_remove_flag(s_cam_preview_note, LV_OBJ_FLAG_HIDDEN);
        }
        return;
    }
    if (s_cam_preview_note != NULL) {
        lv_obj_add_flag(s_cam_preview_note, LV_OBJ_FLAG_HIDDEN);
    }
    lv_obj_remove_flag(s_cam_preview_img, LV_OBJ_FLAG_HIDDEN);
    lv_obj_invalidate(s_cam_preview_img);
}

/* Drop the timer / widget pointers, but keep the PSRAM buffer so switching
 * categories back and forth does not reallocate it. */
static void st_cam_preview_detach(void)
{
    if (s_cam_preview_timer != NULL) {
        lv_timer_delete(s_cam_preview_timer);
        s_cam_preview_timer = NULL;
    }
    s_cam_preview_img = NULL;
    s_cam_preview_note = NULL;
}

static void st_cam_preview_release(void)
{
    st_cam_preview_detach();
    if (s_cam_preview_buf != NULL) {
        heap_caps_free(s_cam_preview_buf);
        s_cam_preview_buf = NULL;
    }
    s_cam_preview_dsc.data = NULL;
    s_cam_preview_dsc.data_size = 0;
}

static void st_cam_preview_start(lv_obj_t *parent)
{
    st_cam_preview_detach();

    const size_t buf_size = (size_t)SET_CAM_PREVIEW_W * SET_CAM_PREVIEW_H * 2;
    if (s_cam_preview_buf == NULL) {
        s_cam_preview_buf = (uint8_t *)heap_caps_malloc(buf_size, MALLOC_CAP_SPIRAM);
        if (s_cam_preview_buf == NULL) {
            ESP_LOGW("UI_SET", "camera preview: PSRAM alloc failed (%u bytes)", (unsigned)buf_size);
        } else {
            memset(s_cam_preview_buf, 0, buf_size);
        }
    }

    s_cam_preview_note = lv_label_create(parent);
    st_label(s_cam_preview_note, "Podglad niedostepny", st_f_note(), lv_color_hex(APP_UI_COLOR_TEXT_MUTED));
    lv_label_set_long_mode(s_cam_preview_note, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_cam_preview_note, LV_PCT(100));
    lv_obj_add_flag(s_cam_preview_note, LV_OBJ_FLAG_HIDDEN);

    if (s_cam_preview_buf == NULL) {
        lv_obj_remove_flag(s_cam_preview_note, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    s_cam_preview_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    s_cam_preview_dsc.header.cf = LV_COLOR_FORMAT_RGB565;
    s_cam_preview_dsc.header.flags = 0;
    s_cam_preview_dsc.header.w = SET_CAM_PREVIEW_W;
    s_cam_preview_dsc.header.h = SET_CAM_PREVIEW_H;
    s_cam_preview_dsc.header.stride = SET_CAM_PREVIEW_W * 2;
    s_cam_preview_dsc.data_size = buf_size;
    s_cam_preview_dsc.data = s_cam_preview_buf;

    s_cam_preview_img = lv_image_create(parent);
    lv_obj_set_width(s_cam_preview_img, SET_CAM_PREVIEW_W);
    lv_obj_set_height(s_cam_preview_img, SET_CAM_PREVIEW_H);
    lv_image_set_src(s_cam_preview_img, &s_cam_preview_dsc);
    lv_image_set_inner_align(s_cam_preview_img, LV_IMAGE_ALIGN_STRETCH);
    lv_obj_set_style_bg_color(s_cam_preview_img, lv_color_hex(APP_UI_COLOR_CARD_BG_OFF), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_cam_preview_img, LV_OPA_COVER, LV_PART_MAIN);

    /* Paint the first frame right away so the card is never blank for a
     * whole timer period after switching to the category. */
    st_cam_preview_timer_cb(NULL);
    s_cam_preview_timer = lv_timer_create(st_cam_preview_timer_cb, 1000, NULL);
}

static void st_build_camera(lv_obj_t *parent)
{
    st_cfg_load();

    s_cam_status_value = NULL;
    s_cam_res_value = NULL;
    s_cam_jpeg_value = NULL;
    s_cam_slider_count = 0;

    lv_obj_t *card = st_make_card(parent);
    st_card_title(card, "Stan kamery");
    s_cam_status_value = st_info_row(card, "Stan", "-");
    s_cam_res_value = st_info_row(card, "Rozdzielczosc", "-");
    s_cam_jpeg_value = st_info_row(card, "Jakosc JPEG", "-");

    lv_obj_t *btn_row = lv_obj_create(card);
    lv_obj_remove_style_all(btn_row);
    lv_obj_set_width(btn_row, LV_PCT(100));
    lv_obj_set_height(btn_row, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(btn_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(btn_row, 12, LV_PART_MAIN);
    lv_obj_set_style_pad_all(btn_row, 0, LV_PART_MAIN);
    lv_obj_clear_flag(btn_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *refresh = st_action_button(btn_row, "Odswiez", lv_color_hex(APP_UI_COLOR_NAV_BTN_BG_ACTIVE));
    lv_obj_add_event_cb(refresh, st_cam_refresh_cb, LV_EVENT_CLICKED, NULL);

    st_cam_rows_refresh();

    lv_obj_t *card2 = st_make_card(parent);
    st_card_title(card2, "Podglad");
    st_cam_preview_start(card2);

    lv_obj_t *card3 = st_make_card(parent);
    st_card_title(card3, "Ustawienia");
    (void)st_cam_switch_row(card3, "Kamera wlaczona", s_cfg.camera_enabled, st_cam_enabled_cb, NULL);
    (void)st_add_dropdown_row(card3, "Rozdzielczosc", SET_CAM_RESOLUTION_OPTIONS,
        (s_cfg.camera_resolution == 0) ? 0 : 1, st_cam_resolution_cb, NULL);
    st_cam_slider_row(card3, "Jakosc JPEG", (int)s_cfg.camera_jpeg_quality, 10, 95,
        st_cam_jpeg_quality_cb, NULL);
    (void)st_cam_switch_row(card3, "Odbicie poziome", s_cfg.camera_hflip, st_cam_hflip_cb, NULL);
    (void)st_cam_switch_row(card3, "Odbicie pionowe", s_cfg.camera_vflip, st_cam_vflip_cb, NULL);
    (void)st_cam_switch_row(card3, "Wybudzanie ruchem", s_cfg.camera_motion_wake, st_cam_motion_wake_cb, NULL);
    st_cam_slider_row(card3, "Czulosc ruchu", (int)s_cfg.camera_motion_threshold, 1, 64,
        st_cam_motion_threshold_cb, NULL);

    lv_obj_t *note = lv_label_create(card3);
    st_label(note, "Zmiany dzialaja od razu i sa zapisywane w pamieci panelu. Zmiana rozdzielczosci "
                   "restartuje strumien kamery.",
        st_f_note(), lv_color_hex(APP_UI_COLOR_TEXT_MUTED));
    lv_label_set_long_mode(note, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(note, LV_PCT(100));
}
#endif

/* ------------------------------------------------------------------ */
/* Content / category rendering                                        */
/* ------------------------------------------------------------------ */
static void st_style_chips(void)
{
    for (int i = 0; i < SET_CAT_COUNT; i++) {
        lv_obj_t *chip = s_chips[i];
        lv_obj_t *label = s_chip_labels[i];
        if (chip == NULL || label == NULL) {
            continue;
        }
        bool active = (i == (int)s_cat);
        if (active) {
            lv_obj_set_style_bg_color(chip, lv_color_hex(APP_UI_COLOR_NAV_BTN_BG_ACTIVE), LV_PART_MAIN);
            lv_obj_set_style_bg_opa(chip, LV_OPA_COVER, LV_PART_MAIN);
            lv_obj_set_style_text_color(label, lv_color_hex(APP_UI_COLOR_NAV_TAB_ACTIVE), LV_PART_MAIN);
        } else {
            lv_obj_set_style_bg_color(chip, lv_color_hex(APP_UI_COLOR_CONTENT_BG), LV_PART_MAIN);
            lv_obj_set_style_bg_opa(chip, LV_OPA_COVER, LV_PART_MAIN);
            lv_obj_set_style_text_color(label, lv_color_hex(APP_UI_COLOR_TEXT_MUTED), LV_PART_MAIN);
        }
    }
}

static void st_chip_click_cb(lv_event_t *event)
{
    const int idx = (int)(uintptr_t)lv_event_get_user_data(event);
    if (idx < 0 || idx >= SET_CAT_COUNT || idx == (int)s_cat) {
        return;
    }
    s_cat = (ui_settings_cat_t)idx;
    st_style_chips();
    st_rebuild_content();
}

static void st_rebuild_content(void)
{
    if (s_content == NULL) {
        return;
    }
    s_slider_count = 0;
    s_active_slider = NULL;
#if CONFIG_APP_FEATURE_LOCAL_CAMERA
    /* The preview widget belongs to the content we are about to clean; the
     * PSRAM buffer survives so quick category switches stay cheap. */
    st_cam_preview_detach();
    s_cam_slider_count = 0;
#endif

    lv_obj_clean(s_content);
    lv_obj_scroll_to_y(s_content, 0, LV_ANIM_OFF);

    switch (s_cat) {
    case SET_CAT_AUDIO:
        st_build_audio(s_content);
        break;
    case SET_CAT_NET:
        st_build_net(s_content);
        break;
    case SET_CAT_SYSTEM:
        st_build_system(s_content);
        break;
    case SET_CAT_SD:
        st_build_sd(s_content);
        break;
#if CONFIG_APP_FEATURE_LOCAL_CAMERA
    case SET_CAT_CAMERA:
        st_build_camera(s_content);
        break;
#endif
    case SET_CAT_SCREEN:
    default:
        st_build_screen(s_content);
        break;
    }
}

static void st_close_cb(lv_event_t *event)
{
    LV_UNUSED(event);
#if CONFIG_APP_FEATURE_LOCAL_CAMERA
    st_cam_preview_release();
#endif
    if (s_open) {
        st_cfg_apply();
        st_cfg_save();
    }
    if (s_overlay != NULL) {
        lv_obj_del(s_overlay);
        s_overlay = NULL;
    }
    s_rail = NULL;
    s_content = NULL;
    s_open = false;
    s_active_slider = NULL;
    s_slider_count = 0;
}

/* ------------------------------------------------------------------ */
/* Overlay creation                                                    */
/* ------------------------------------------------------------------ */
static void st_open(void)
{
    if (s_open || s_overlay != NULL) {
        return;
    }
    s_open = true;
    s_small = (APP_SCREEN_WIDTH < 700);
    s_cat = SET_CAT_SCREEN;
    s_slider_count = 0;
    s_active_slider = NULL;
    st_cfg_load();
    memset(s_chips, 0, sizeof(s_chips));
    memset(s_chip_labels, 0, sizeof(s_chip_labels));

    lv_obj_t *screen = lv_scr_act();
    s_overlay = lv_obj_create(screen);
    lv_obj_remove_style_all(s_overlay);
    lv_obj_set_size(s_overlay, APP_SCREEN_WIDTH, APP_SCREEN_HEIGHT);
    lv_obj_set_pos(s_overlay, 0, 0);
    lv_obj_set_style_bg_color(s_overlay, lv_color_hex(APP_UI_COLOR_CONTENT_BG), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_overlay, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_clear_flag(s_overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_move_foreground(s_overlay);

    /* Header */
    lv_obj_t *header = lv_obj_create(s_overlay);
    lv_obj_remove_style_all(header);
    lv_obj_set_size(header, APP_SCREEN_WIDTH, st_hdr_h());
    lv_obj_set_pos(header, 0, 0);
    lv_obj_clear_flag(header, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(header, lv_color_hex(APP_UI_COLOR_TOPBAR_BG), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(header, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(header, 1, LV_PART_MAIN);
    lv_obj_set_style_border_side(header, LV_BORDER_SIDE_BOTTOM, LV_PART_MAIN);
    lv_obj_set_style_border_color(header, lv_color_hex(APP_UI_COLOR_TOPBAR_BORDER), LV_PART_MAIN);
    lv_obj_set_style_border_opa(header, LV_OPA_80, LV_PART_MAIN);

    lv_obj_t *title = lv_label_create(header);
    st_label(title, "Ustawienia", st_f_title(), lv_color_hex(APP_UI_COLOR_TOPBAR_TEXT));
    lv_obj_align(title, LV_ALIGN_LEFT_MID, 24, 0);

    lv_obj_t *close = lv_obj_create(header);
    lv_obj_remove_style_all(close);
    lv_coord_t close_size = s_small ? 44 : 56;
    lv_obj_set_size(close, close_size, close_size);
    lv_obj_align(close, LV_ALIGN_RIGHT_MID, -16, 0);
    lv_obj_set_style_radius(close, close_size / 2, LV_PART_MAIN);
    lv_obj_set_style_bg_color(close, lv_color_hex(APP_UI_COLOR_TOPBAR_CHIP_BG), LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(close, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_add_flag(close, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(close, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(close, st_close_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *close_label = lv_label_create(close);
    st_label(close_label, LV_SYMBOL_CLOSE, st_f_title(), lv_color_hex(APP_UI_COLOR_TOPBAR_TEXT));
    lv_obj_center(close_label);

    /* Left category rail */
    lv_obj_t *rail = lv_obj_create(s_overlay);
    lv_obj_remove_style_all(rail);
    lv_obj_set_size(rail, st_rail_w(), APP_SCREEN_HEIGHT - st_hdr_h());
    lv_obj_set_pos(rail, 0, st_hdr_h());
    lv_obj_set_style_bg_color(rail, lv_color_hex(APP_UI_COLOR_CONTENT_BG), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(rail, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(rail, 1, LV_PART_MAIN);
    lv_obj_set_style_border_side(rail, LV_BORDER_SIDE_RIGHT, LV_PART_MAIN);
    lv_obj_set_style_border_color(rail, lv_color_hex(APP_UI_COLOR_CONTENT_BORDER), LV_PART_MAIN);
    lv_obj_set_style_pad_all(rail, 10, LV_PART_MAIN);
    lv_obj_set_style_pad_row(rail, 6, LV_PART_MAIN);
    lv_obj_set_flex_flow(rail, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(rail, LV_OBJ_FLAG_SCROLLABLE);
    s_rail = rail;

    for (int i = 0; i < SET_CAT_COUNT; i++) {
        lv_obj_t *chip = lv_obj_create(rail);
        lv_obj_remove_style_all(chip);
        lv_obj_set_width(chip, LV_PCT(100));
        lv_obj_set_height(chip, s_small ? 42 : 52);
        lv_obj_set_style_radius(chip, 14, LV_PART_MAIN);
        lv_obj_add_flag(chip, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_clear_flag(chip, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_event_cb(chip, st_chip_click_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)i);

        lv_obj_t *label = lv_label_create(chip);
        st_label(label, s_cat_names[i], st_f_cat(), lv_color_hex(APP_UI_COLOR_TEXT_MUTED));
        lv_obj_align(label, LV_ALIGN_LEFT_MID, 14, 0);

        s_chips[i] = chip;
        s_chip_labels[i] = label;
    }
    st_style_chips();

    /* Right content panel */
    lv_obj_t *content = lv_obj_create(s_overlay);
    lv_obj_remove_style_all(content);
    lv_obj_set_size(content, APP_SCREEN_WIDTH - st_rail_w(), APP_SCREEN_HEIGHT - st_hdr_h());
    lv_obj_set_pos(content, st_rail_w(), st_hdr_h());
    lv_obj_set_style_pad_all(content, 18, LV_PART_MAIN);
    lv_obj_set_style_pad_row(content, 14, LV_PART_MAIN);
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_bg_color(content, lv_color_hex(APP_UI_COLOR_CONTENT_BG), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(content, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_scrollbar_mode(content, LV_SCROLLBAR_MODE_OFF);
    s_content = content;

    st_rebuild_content();
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */
static void st_gear_cb(void)
{
    if (s_open || s_overlay != NULL) {
        st_close_cb(NULL);
    } else {
        st_open();
    }
}

static void st_screen_built_cb(void)
{
    /* ui_pages_init() cleaned the active screen: any overlay we owned is gone. */
#if CONFIG_APP_FEATURE_LOCAL_CAMERA
    st_cam_preview_release();
#endif
    s_overlay = NULL;
    s_rail = NULL;
    s_content = NULL;
    s_open = false;
    s_active_slider = NULL;
    s_slider_count = 0;
    memset(s_chips, 0, sizeof(s_chips));
    memset(s_chip_labels, 0, sizeof(s_chip_labels));
}

void ui_settings_init(void)
{
    ui_pages_set_gear_callback(st_gear_cb);
    ui_pages_set_screen_built_callback(st_screen_built_cb);
}

void ui_settings_toggle(void)
{
    st_gear_cb();
}

bool ui_settings_is_open(void)
{
    return s_open;
}
