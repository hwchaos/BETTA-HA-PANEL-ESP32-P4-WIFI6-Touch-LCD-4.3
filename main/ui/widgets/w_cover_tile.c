/* SPDX-License-Identifier: LicenseRef-FNCL-1.1
 * Copyright (c) 2026 Cpt_Kirk
 *
 * Cover tile - roller shutters, blinds, awnings, gates (cover.* entities).
 * Shows the opening position (current_position) when the entity reports it,
 * plus the mapped HA state. Tapping the tile toggles open/close and a second
 * tap while the cover is travelling stops it; the button row sends the explicit
 * open / stop / close command. The progress rail under the value doubles as a
 * position slider: press or drag it to pick any value from 0 to 100 % and the
 * target goes out as cover.set_cover_position when the finger lifts. The button
 * row and the rail are dropped on tiles that are too small to hold them, so the
 * tile stays readable in any size.
 */
#include "ui/ui_widget_factory.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_log.h"

#include "ha/ha_client.h"
#include "ui/fonts/app_text_fonts.h"
#include "ui/fonts/mdi_font_registry.h"
#include "ui/theme/theme_default.h"
#include "ui/ui_i18n.h"
#include "ui/ui_memory.h"
#include "diag/system_log.h"

static const char *TAG = "w_cover_tile";

#define W_COVER_BUSY_TIMEOUT_MS 8000
#define W_COVER_MIN_BUTTON_H    150
#define W_COVER_MIN_BUTTON_W    170
#define W_COVER_GLYPH_OPEN      0xF05B1U /* window-open */
#define W_COVER_GLYPH_CLOSED    0xF05AEU /* window-closed */
/* The rail is only a few pixels tall, so the touch band around it is widened. */
#define W_COVER_SLIDER_TOUCH_PAD        12
#define W_COVER_SLIDER_TOUCH_MIN_HEIGHT 30
#define W_COVER_SLIDER_TOUCH_SIDE_PAD   10
/* A stop settles much faster than a full travel, so it frees the tile sooner. */
#define W_COVER_STOP_BUSY_MS            2000
/* cover supported_features bit set by entities that accept set_cover_position. */
#define W_COVER_FEATURE_SET_POSITION    4U
/* Colour of the rail preview while a finger is on it, i.e. not sent yet. */
#define W_COVER_PREVIEW_ACCENT          (APP_UI_COLOR_CARD_ICON_ON)

typedef enum {
    W_COVER_CMD_OPEN = 0,
    W_COVER_CMD_STOP,
    W_COVER_CMD_CLOSE,
    W_COVER_CMD_COUNT,
} w_cover_cmd_t;

typedef struct {
    w_cover_cmd_t cmd;
    lv_obj_t *button;
    lv_obj_t *label;
} w_cover_button_t;

typedef struct {
    char entity_id[APP_MAX_ENTITY_ID_LEN];
    char value_text[APP_MAX_NAME_LEN];   /* big label: "45 %" or the state text */
    char caption_text[APP_MAX_NAME_LEN]; /* small label: mapped HA state */
    lv_obj_t *card;
    lv_obj_t *icon;
    lv_obj_t *title_label;
    lv_obj_t *caption_label;
    lv_obj_t *value_label;
    lv_obj_t *track;
    lv_obj_t *fill;
    w_cover_button_t buttons[W_COVER_CMD_COUNT];
    int button_count;
    lv_timer_t *timer;
    uint32_t accent;
    uint32_t busy_until_ms;
    int position;
    int drag_position;
    lv_coord_t value_height; /* room the big label has, used to fit its font */
    w_cover_cmd_t pending_cmd; /* W_COVER_CMD_COUNT when nothing is in flight */
    bool have_position;
    bool supports_position;
    bool is_open;
    bool unavailable;
    bool busy;
    bool have_state;
    bool dragging;
    bool slider_click_guard;
    bool state_opening;
    bool state_closing;
    /* Long-press popup with a vertical position slider. */
    lv_obj_t *popup_overlay;
    lv_obj_t *popup_slider;
    lv_obj_t *popup_value;
    bool long_press_guard; /* swallow the click that ends a long press */
} w_cover_tile_t;

static const char *cover_i18n(const char *key, const char *fallback)
{
    const char *text = ui_i18n_get(key, NULL);
    return (text != NULL && text[0] != '\0') ? text : fallback;
}

static const lv_font_t *cover_font_px(int px)
{
    if (px >= 34) {
        return APP_FONT_TEXT_34;
    }
    if (px >= 28) {
        return APP_FONT_TEXT_28;
    }
    if (px >= 24) {
        return APP_FONT_TEXT_24;
    }
    if (px >= 20) {
        return APP_FONT_TEXT_20;
    }
    if (px >= 18) {
        return APP_FONT_TEXT_18;
    }
    if (px >= 16) {
        return APP_FONT_TEXT_16;
    }
    return APP_FONT_TEXT_14;
}

static const lv_font_t *cover_fit_font(const char *text, int max_width, int max_height)
{
    static const int sizes[] = {34, 28, 24, 20, 18, 16, 14};
    const lv_font_t *result = cover_font_px(14);

    for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
        const lv_font_t *font = cover_font_px(sizes[i]);
        if (lv_font_get_line_height(font) > max_height) {
            continue;
        }
        lv_point_t size = {0, 0};
        lv_text_get_size(&size, text, font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
        if (size.x <= max_width) {
            result = font;
            break;
        }
    }
    return result;
}

static bool cover_state_is_unavailable(const char *state)
{
    if (state == NULL) {
        return true;
    }
    return strcmp(state, "unavailable") == 0 || strcmp(state, "unknown") == 0;
}

static const char *cover_cmd_service(w_cover_cmd_t cmd)
{
    switch (cmd) {
    case W_COVER_CMD_OPEN:
        return "open_cover";
    case W_COVER_CMD_CLOSE:
        return "close_cover";
    case W_COVER_CMD_STOP:
    default:
        return "stop_cover";
    }
}

static const char *cover_cmd_label_key(w_cover_cmd_t cmd)
{
    switch (cmd) {
    case W_COVER_CMD_OPEN:
        return "cover.btn_open";
    case W_COVER_CMD_CLOSE:
        return "cover.btn_close";
    case W_COVER_CMD_STOP:
    default:
        return "cover.btn_stop";
    }
}

static const char *cover_cmd_label_fallback(w_cover_cmd_t cmd)
{
    switch (cmd) {
    case W_COVER_CMD_OPEN:
        return "Open";
    case W_COVER_CMD_CLOSE:
        return "Close";
    case W_COVER_CMD_STOP:
    default:
        return "Stop";
    }
}

/* Colours follow the active theme so the tile matches every palette. */
static uint32_t cover_cmd_accent(w_cover_cmd_t cmd)
{
    switch (cmd) {
    case W_COVER_CMD_OPEN:
        return APP_UI_COLOR_CARD_ICON_ON;
    case W_COVER_CMD_CLOSE:
        return APP_UI_COLOR_LIGHT_IND_ON;
    case W_COVER_CMD_STOP:
    default:
        return APP_UI_COLOR_TEXT_MUTED;
    }
}

/* Maps the HA cover state to a localised caption plus an accent colour. */
static void cover_state_style(const char *state, const char **out_text, uint32_t *out_accent)
{
    if (strcmp(state, "open") == 0) {
        /* Open paints the tile in its "on" colours: use the bright state
         * colour so icon and value stay readable on it. */
        *out_text = cover_i18n("cover.open", "Open");
        *out_accent = APP_UI_COLOR_STATE_ON;
    } else if (strcmp(state, "closed") == 0) {
        *out_text = cover_i18n("cover.closed", "Closed");
        *out_accent = APP_UI_COLOR_STATE_OFF;
    } else if (strcmp(state, "opening") == 0) {
        *out_text = cover_i18n("cover.opening", "Opening...");
        *out_accent = APP_UI_COLOR_LIGHT_ICON_ON;
    } else if (strcmp(state, "closing") == 0) {
        *out_text = cover_i18n("cover.closing", "Closing...");
        *out_accent = APP_UI_COLOR_LIGHT_ICON_ON;
    } else if (strcmp(state, "stopped") == 0) {
        *out_text = cover_i18n("cover.stopped", "Stopped");
        *out_accent = APP_UI_COLOR_TEXT_SOFT;
    } else {
        *out_text = state;
        *out_accent = APP_UI_COLOR_TEXT_MUTED;
    }
}

static bool cover_glyph_available(uint32_t cp)
{
    const lv_font_t *font = mdi_font_icon_56();
    if (font == NULL) {
        return false;
    }
    lv_font_glyph_dsc_t dsc;
    return lv_font_get_glyph_dsc(font, &dsc, cp, 0);
}

static void cover_set_glyph(lv_obj_t *label, uint32_t cp)
{
    if (label == NULL) {
        return;
    }
    char text[8] = {0};
    text[0] = (char)(0xF0U | (cp >> 18));
    text[1] = (char)(0x80U | ((cp >> 12) & 0x3FU));
    text[2] = (char)(0x80U | ((cp >> 6) & 0x3FU));
    text[3] = (char)(0x80U | (cp & 0x3FU));
    lv_label_set_text(label, text);
}

static void cover_style_button(lv_obj_t *button, uint32_t bg, uint32_t border, uint32_t text, lv_opa_t opa)
{
    if (button == NULL) {
        return;
    }
    lv_obj_set_style_bg_color(button, lv_color_hex(bg), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(button, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(button, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(button, lv_color_hex(border), LV_PART_MAIN);
    lv_obj_set_style_shadow_width(button, 0, LV_PART_MAIN);
    lv_obj_set_style_opa(button, opa, LV_PART_MAIN);
    lv_obj_set_style_text_color(button, lv_color_hex(text), LV_PART_MAIN);
}

static void cover_set_value_text(w_cover_tile_t *ctx, const char *text, uint32_t accent)
{
    if (ctx == NULL || ctx->value_label == NULL) {
        return;
    }
    strlcpy(ctx->value_text, text != NULL ? text : "", sizeof(ctx->value_text));
    ctx->accent = accent;
    ctx->have_state = true;

    lv_obj_set_style_text_color(ctx->value_label, lv_color_hex(accent), LV_PART_MAIN);
    lv_label_set_text(ctx->value_label, ctx->value_text);
    if (ctx->icon != NULL) {
        lv_obj_set_style_text_color(ctx->icon, lv_color_hex(accent), LV_PART_MAIN);
    }
}

/* While a command is in flight only open/close are muted; Stop stays live so a
 * second tap can always abort the travel. */
static void cover_update_buttons(w_cover_tile_t *ctx)
{
    if (ctx == NULL) {
        return;
    }

    for (int i = 0; i < ctx->button_count; i++) {
        const w_cover_cmd_t cmd = ctx->buttons[i].cmd;
        const uint32_t accent = cover_cmd_accent(cmd);
        const bool disabled = ctx->unavailable || (ctx->busy && cmd != W_COVER_CMD_STOP);
        cover_style_button(ctx->buttons[i].button, APP_UI_COLOR_NAV_BTN_BG_IDLE,
                           disabled ? APP_UI_COLOR_CARD_BORDER : accent,
                           disabled ? APP_UI_COLOR_TEXT_MUTED : APP_UI_COLOR_TEXT_PRIMARY,
                           disabled ? LV_OPA_50 : LV_OPA_COVER);
        if (ctx->buttons[i].label != NULL) {
            lv_obj_set_style_text_color(ctx->buttons[i].label,
                lv_color_hex(disabled ? APP_UI_COLOR_TEXT_MUTED : APP_UI_COLOR_TEXT_PRIMARY), LV_PART_MAIN);
        }
    }
}

/* Resizes the filled part of the rail. `percent` below 0 hides the fill. */
static void cover_update_track_fill(w_cover_tile_t *ctx, int percent)
{
    if (ctx == NULL || ctx->track == NULL || ctx->fill == NULL) {
        return;
    }

    lv_coord_t fill_w = 0;
    if (percent > 0) {
        const lv_coord_t track_w = lv_obj_get_width(ctx->track);
        fill_w = (lv_coord_t)(((int32_t)track_w * (percent > 100 ? 100 : percent)) / 100);
    }
    if (fill_w <= 0) {
        lv_obj_add_flag(ctx->fill, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    const lv_coord_t track_h = lv_obj_get_height(ctx->track);
    lv_obj_clear_flag(ctx->fill, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_size(ctx->fill, fill_w, track_h);
    lv_obj_align(ctx->fill, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_style_radius(ctx->fill, track_h / 2, LV_PART_MAIN);
}

static void cover_layout(w_cover_tile_t *ctx)
{
    if (ctx == NULL || ctx->card == NULL) {
        return;
    }

    const lv_coord_t card_w = lv_obj_get_width(ctx->card);
    const lv_coord_t card_h = lv_obj_get_height(ctx->card);
    if (card_w <= 0 || card_h <= 0) {
        return;
    }

    const lv_coord_t pad = (card_w < 240 || card_h < 180) ? 6 : 10;
    lv_obj_set_style_pad_all(ctx->card, pad, LV_PART_MAIN);

    const bool buttons_fit = (card_h >= W_COVER_MIN_BUTTON_H && card_w >= W_COVER_MIN_BUTTON_W);
    const lv_coord_t row_h = buttons_fit ? LV_CLAMP((lv_coord_t)(card_h / 5), 32, 50) : 0;

    lv_coord_t header_h = 0;
    if (ctx->title_label != NULL) {
        const lv_font_t *title_font = cover_font_px((card_h >= 150) ? 16 : 14);
        header_h = (lv_coord_t)lv_font_get_line_height(title_font);
        lv_obj_set_style_text_font(ctx->title_label, title_font, LV_PART_MAIN);
        lv_obj_set_width(ctx->title_label, card_w - (2 * pad));
        lv_label_set_long_mode(ctx->title_label, LV_LABEL_LONG_DOT);
        lv_obj_align(ctx->title_label, LV_ALIGN_TOP_MID, 0, 0);
    }

    const lv_coord_t top = pad + header_h;
    lv_coord_t bottom = pad + (row_h > 0 ? (row_h + 6) : 0);
    lv_coord_t area_h = card_h - top - bottom;
    if (area_h < 24) {
        area_h = 24;
    }

    const bool show_track = (area_h >= 54 && card_w >= 90);
    lv_coord_t track_h = 0;
    if (show_track) {
        track_h = (card_h >= 180) ? 10 : 8;
    }

    /* The caption line needs its own room under the value, or it runs into the
     * rail. */
    const lv_coord_t caption_h =
        (ctx->caption_label != NULL) ? (lv_coord_t)lv_font_get_line_height(cover_font_px(14)) + 2 : 0;
    lv_coord_t text_area_h = area_h - caption_h - (show_track ? (track_h + 6) : 0);
    if (text_area_h < 18) {
        text_area_h = 18;
    }
    const lv_coord_t text_area_w = card_w - (2 * pad);
    ctx->value_height = text_area_h;

    /* The big icon only stays when there is room for it next to the value. */
    const bool show_icon = (ctx->icon != NULL) && (text_area_h >= 44) && (text_area_w >= 150);
    if (show_icon) {
        lv_obj_clear_flag(ctx->icon, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(ctx->icon, LV_OBJ_FLAG_HIDDEN);
    }

    const lv_coord_t icon_size = show_icon ? LV_MIN(text_area_h, (lv_coord_t)(text_area_w / 3)) : 0;
    if (show_icon) {
        lv_obj_set_style_text_font(ctx->icon, mdi_font_icon_56(), LV_PART_MAIN);
        lv_obj_align(ctx->icon, LV_ALIGN_TOP_LEFT, 0, header_h + (text_area_h - icon_size) / 2);
    }

    /* Everything below the title starts at header_h; aligning at 0 put the
     * big value straight onto the title. */
    if (ctx->value_label != NULL) {
        const lv_coord_t value_w = show_icon ? (text_area_w - icon_size - 8) : text_area_w;
        lv_obj_set_width(ctx->value_label, value_w);
        lv_obj_set_style_text_font(ctx->value_label,
            cover_fit_font(ctx->value_text, value_w, text_area_h), LV_PART_MAIN);
        if (show_icon) {
            lv_obj_align(ctx->value_label, LV_ALIGN_TOP_RIGHT, 0, header_h);
        } else {
            lv_obj_align(ctx->value_label, LV_ALIGN_TOP_MID, 0, header_h);
        }
    }

    if (ctx->caption_label != NULL) {
        lv_obj_set_width(ctx->caption_label, text_area_w);
        lv_obj_set_style_text_font(ctx->caption_label, cover_font_px(14), LV_PART_MAIN);
        lv_label_set_long_mode(ctx->caption_label, LV_LABEL_LONG_DOT);
        lv_obj_align(ctx->caption_label, LV_ALIGN_TOP_MID, 0, header_h + text_area_h + 2);
    }

    if (ctx->track != NULL) {
        if (show_track) {
            lv_obj_clear_flag(ctx->track, LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_size(ctx->track, text_area_w, track_h);
            lv_obj_align(ctx->track, LV_ALIGN_BOTTOM_MID, 0, -(row_h > 0 ? (row_h + 6) : 0));
            lv_obj_set_style_radius(ctx->track, track_h / 2, LV_PART_MAIN);
            cover_update_track_fill(ctx, ctx->have_position ? ctx->position : -1);
        } else {
            lv_obj_add_flag(ctx->track, LV_OBJ_FLAG_HIDDEN);
            if (ctx->fill != NULL) {
                lv_obj_add_flag(ctx->fill, LV_OBJ_FLAG_HIDDEN);
            }
        }
    }

    if (ctx->button_count > 0) {
        const lv_coord_t row_w = card_w - (2 * pad);
        const lv_coord_t gap = 6;
        const lv_coord_t button_w = (row_w - (gap * (ctx->button_count - 1))) / ctx->button_count;
        for (int i = 0; i < ctx->button_count; i++) {
            lv_obj_t *button = ctx->buttons[i].button;
            if (button == NULL) {
                continue;
            }
            if (row_h <= 0) {
                lv_obj_add_flag(button, LV_OBJ_FLAG_HIDDEN);
                continue;
            }
            lv_obj_clear_flag(button, LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_size(button, button_w, row_h);
            lv_obj_align(button, LV_ALIGN_BOTTOM_LEFT, pad + (i * (button_w + gap)), -pad);
            if (ctx->buttons[i].label != NULL) {
                lv_obj_set_style_text_font(ctx->buttons[i].label, cover_font_px(row_h >= 44 ? 16 : 14), LV_PART_MAIN);
            }
        }
    }
}

/* True while a travel is expected to be in progress: HA reports opening/closing
 * or a command we sent has not settled yet. Second tap then means "stop". */
static bool cover_is_moving(const w_cover_tile_t *ctx)
{
    return ctx->state_opening || ctx->state_closing || ctx->pending_cmd != W_COVER_CMD_COUNT;
}

/* Paints the tile from the cached fields. Split out of apply_state() so the
 * position slider can repaint its live preview without waiting for HA. */
static void cover_render(w_cover_tile_t *ctx)
{
    if (ctx == NULL || ctx->card == NULL) {
        return;
    }

    if (ctx->unavailable) {
        cover_set_value_text(ctx, ctx->caption_text, APP_UI_COLOR_TEXT_MUTED);
        if (ctx->caption_label != NULL) {
            lv_obj_add_flag(ctx->caption_label, LV_OBJ_FLAG_HIDDEN);
        }
        lv_obj_set_style_bg_color(ctx->card, lv_color_hex(APP_UI_COLOR_CARD_BG_OFF), LV_PART_MAIN);
        lv_obj_set_style_border_color(ctx->card, lv_color_hex(APP_UI_COLOR_CARD_BORDER), LV_PART_MAIN);
        cover_update_track_fill(ctx, -1);
        return;
    }

    if (ctx->have_position) {
        char value[16];
        snprintf(value, sizeof(value), "%d%%", ctx->position);
        cover_set_value_text(ctx, value, ctx->accent);
        if (ctx->caption_label != NULL) {
            lv_label_set_text(ctx->caption_label, ctx->caption_text);
            lv_obj_clear_flag(ctx->caption_label, LV_OBJ_FLAG_HIDDEN);
        }
    } else {
        cover_set_value_text(ctx, ctx->caption_text, ctx->accent);
        if (ctx->caption_label != NULL) {
            lv_obj_add_flag(ctx->caption_label, LV_OBJ_FLAG_HIDDEN);
        }
    }
    cover_update_track_fill(ctx, ctx->have_position ? ctx->position : -1);

    if (ctx->icon != NULL) {
        cover_set_glyph(ctx->icon, ctx->is_open ? W_COVER_GLYPH_OPEN : W_COVER_GLYPH_CLOSED);
    }
    if (ctx->fill != NULL) {
        lv_obj_set_style_bg_color(ctx->fill, lv_color_hex(ctx->accent), LV_PART_MAIN);
    }
    lv_obj_set_style_bg_color(ctx->card,
        lv_color_hex(ctx->is_open ? APP_UI_COLOR_CARD_BG_ON : APP_UI_COLOR_CARD_BG_OFF), LV_PART_MAIN);
    lv_obj_set_style_border_color(ctx->card, lv_color_hex(ctx->accent), LV_PART_MAIN);
}

static void cover_send(w_cover_tile_t *ctx, w_cover_cmd_t cmd)
{
    if (ctx == NULL || ctx->entity_id[0] == '\0') {
        return;
    }

    char payload[APP_MAX_ENTITY_ID_LEN + 32];
    snprintf(payload, sizeof(payload), "{\"entity_id\":\"%s\"}", ctx->entity_id);

    const char *service = cover_cmd_service(cmd);
    esp_err_t err = ha_client_call_service("cover", service, payload);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "%s failed for %s: %s", service, ctx->entity_id, esp_err_to_name(err));
        cover_set_value_text(ctx, cover_i18n("cover.send_failed", "Command failed"), 0xFF5252);
        cover_layout(ctx);
        return;
    }

    ESP_LOGI(TAG, "%s -> %s", service, ctx->entity_id);
    const bool is_stop = (cmd == W_COVER_CMD_STOP);
    ctx->busy = true;
    ctx->busy_until_ms = lv_tick_get() + (is_stop ? W_COVER_STOP_BUSY_MS : W_COVER_BUSY_TIMEOUT_MS);
    ctx->pending_cmd = is_stop ? W_COVER_CMD_COUNT : cmd;
    if (is_stop) {
        ctx->state_opening = false;
        ctx->state_closing = false;
    }
    cover_set_value_text(ctx, cover_i18n("cover.busy", "Sending..."), cover_cmd_accent(cmd));
    cover_update_buttons(ctx);
    cover_layout(ctx);
}

/* Sends an absolute target, i.e. "open or close to N percent". */
static void cover_send_position(w_cover_tile_t *ctx, int percent)
{
    if (ctx == NULL || ctx->entity_id[0] == '\0') {
        return;
    }
    if (percent < 0) {
        percent = 0;
    }
    if (percent > 100) {
        percent = 100;
    }

    if (ctx->have_position && percent == ctx->position) {
        ESP_LOGI(TAG, "set_cover_position %d%% skipped, %s already there", percent, ctx->entity_id);
        cover_render(ctx);
        cover_layout(ctx);
        return;
    }

    char payload[APP_MAX_ENTITY_ID_LEN + 48];
    snprintf(payload, sizeof(payload), "{\"entity_id\":\"%s\",\"position\":%d}", ctx->entity_id, percent);

    esp_err_t err = ha_client_call_service("cover", "set_cover_position", payload);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "set_cover_position %d%% failed for %s: %s", percent, ctx->entity_id, esp_err_to_name(err));
        cover_set_value_text(ctx, cover_i18n("cover.send_failed", "Command failed"), 0xFF5252);
        cover_layout(ctx);
        return;
    }

    ESP_LOGI(TAG, "set_cover_position %d%% -> %s", percent, ctx->entity_id);
    ctx->have_position = true;
    ctx->position = percent;
    ctx->is_open = (percent > 0);
    ctx->pending_cmd = W_COVER_CMD_COUNT;
    ctx->accent = 0xFFA726;
    ctx->busy = true;
    ctx->busy_until_ms = lv_tick_get() + W_COVER_BUSY_TIMEOUT_MS;
    cover_render(ctx);
    cover_update_buttons(ctx);
    cover_layout(ctx);
}

static void cover_toggle(w_cover_tile_t *ctx)
{
    if (ctx == NULL || ctx->unavailable || ctx->dragging) {
        return;
    }
    if (cover_is_moving(ctx)) {
        cover_send(ctx, W_COVER_CMD_STOP);
        return;
    }
    cover_send(ctx, ctx->is_open ? W_COVER_CMD_CLOSE : W_COVER_CMD_OPEN);
}

static void cover_button_event_cb(lv_event_t *event)
{
    w_cover_tile_t *ctx = (w_cover_tile_t *)lv_event_get_user_data(event);
    lv_obj_t *button = lv_event_get_target(event);
    if (ctx == NULL || button == NULL || ctx->unavailable) {
        return;
    }
    const w_cover_cmd_t cmd = (w_cover_cmd_t)(intptr_t)lv_obj_get_user_data(button);
    if (ctx->busy && cmd != W_COVER_CMD_STOP) {
        return;
    }
    cover_send(ctx, cmd);
}

static void cover_timer_cb(lv_timer_t *timer)
{
    system_log_note_lvgl_cb("cover_timer_cb");
    w_cover_tile_t *ctx = (w_cover_tile_t *)lv_timer_get_user_data(timer);
    if (ctx == NULL) {
        return;
    }
    if (ctx->busy && (int32_t)(lv_tick_get() - ctx->busy_until_ms) >= 0) {
        ctx->busy = false;
        /* HA never confirmed, so drop the "in flight" flag and show the truth. */
        ctx->pending_cmd = W_COVER_CMD_COUNT;
        if (ctx->have_state) {
            cover_render(ctx);
        }
        cover_update_buttons(ctx);
        cover_layout(ctx);
    }
}

/* True when the press landed in the widened touch band around the rail. */
static bool cover_slider_band_contains(lv_obj_t *card, lv_obj_t *track, const lv_point_t *point)
{
    if (card == NULL || track == NULL || point == NULL) {
        return false;
    }

    lv_area_t track_area = {0};
    lv_area_t card_area = {0};
    lv_obj_get_coords(track, &track_area);
    lv_obj_get_coords(card, &card_area);

    const lv_coord_t track_h = (track_area.y2 - track_area.y1) + 1;
    if (track_h <= 0) {
        return false;
    }
    lv_coord_t band_h = track_h + (W_COVER_SLIDER_TOUCH_PAD * 2);
    if (band_h < W_COVER_SLIDER_TOUCH_MIN_HEIGHT) {
        band_h = W_COVER_SLIDER_TOUCH_MIN_HEIGHT;
    }

    lv_coord_t band_top = track_area.y1 - ((band_h - track_h) / 2);
    lv_coord_t band_bottom = band_top + band_h - 1;
    if (band_top < card_area.y1 + 2) {
        band_top = card_area.y1 + 2;
    }
    if (band_bottom > card_area.y2 - 2) {
        band_bottom = card_area.y2 - 2;
    }

    if (point->y < band_top || point->y > band_bottom) {
        return false;
    }
    return point->x >= (track_area.x1 - W_COVER_SLIDER_TOUCH_SIDE_PAD) &&
           point->x <= (track_area.x2 + W_COVER_SLIDER_TOUCH_SIDE_PAD);
}

static int cover_position_at_x(lv_obj_t *track, lv_coord_t x)
{
    lv_area_t track_area = {0};
    lv_obj_get_coords(track, &track_area);
    const lv_coord_t width = (track_area.x2 - track_area.x1) + 1;
    if (width <= 0) {
        return 0;
    }

    lv_coord_t rel = x - track_area.x1;
    if (rel < 0) {
        rel = 0;
    }
    if (rel >= width) {
        rel = width - 1;
    }
    return (int)(((rel * 100) + (width / 2)) / width);
}

/* Lightweight repaint used on every drag step: only the value and the rail.
 * The cached accent is restored so a cancelled drag cannot tint the card. */
static void cover_show_drag_value(w_cover_tile_t *ctx, int percent)
{
    char value[16];
    snprintf(value, sizeof(value), "%d%%", percent);

    const uint32_t settled_accent = ctx->accent;
    cover_set_value_text(ctx, value, W_COVER_PREVIEW_ACCENT);
    ctx->accent = settled_accent;

    if (ctx->value_label != NULL && ctx->value_height > 0) {
        lv_obj_set_style_text_font(ctx->value_label,
            cover_fit_font(value, lv_obj_get_width(ctx->value_label), ctx->value_height), LV_PART_MAIN);
    }
    if (ctx->caption_label != NULL) {
        lv_label_set_text(ctx->caption_label, cover_i18n("cover.set_position", "Set position"));
        lv_obj_clear_flag(ctx->caption_label, LV_OBJ_FLAG_HIDDEN);
    }
    if (ctx->icon != NULL) {
        cover_set_glyph(ctx->icon, (percent > 0) ? W_COVER_GLYPH_OPEN : W_COVER_GLYPH_CLOSED);
        lv_obj_set_style_text_color(ctx->icon, lv_color_hex(W_COVER_PREVIEW_ACCENT), LV_PART_MAIN);
    }
    if (ctx->fill != NULL) {
        lv_obj_set_style_bg_color(ctx->fill, lv_color_hex(W_COVER_PREVIEW_ACCENT), LV_PART_MAIN);
    }
    cover_update_track_fill(ctx, percent);
}

static void cover_slider_begin_drag(w_cover_tile_t *ctx, const lv_point_t *point)
{
    ctx->dragging = true;
    ctx->slider_click_guard = true;
    ctx->drag_position = cover_position_at_x(ctx->track, point->x);
    cover_show_drag_value(ctx, ctx->drag_position);
}

static void cover_slider_update_drag(w_cover_tile_t *ctx, const lv_point_t *point)
{
    const int percent = cover_position_at_x(ctx->track, point->x);
    if (percent == ctx->drag_position) {
        return;
    }
    ctx->drag_position = percent;
    cover_show_drag_value(ctx, percent);
}

static void cover_slider_end_drag(w_cover_tile_t *ctx)
{
    const int percent = ctx->drag_position;
    ctx->dragging = false;
    cover_send_position(ctx, percent);
}

/* ---- Long-press popup ---------------------------------------------------- */

static void cover_popup_set_value_text(w_cover_tile_t *ctx, int percent)
{
    if (ctx->popup_value == NULL) {
        return;
    }
    char text[16];
    snprintf(text, sizeof(text), "%d %%", percent);
    lv_label_set_text(ctx->popup_value, text);
}

/* Keeps the popup in step with HA unless the finger is on the slider. */
static void cover_popup_sync(w_cover_tile_t *ctx)
{
    if (ctx == NULL || ctx->popup_slider == NULL || !ctx->have_position) {
        return;
    }
    if (lv_obj_has_state(ctx->popup_slider, LV_STATE_PRESSED)) {
        return;
    }
    lv_slider_set_value(ctx->popup_slider, ctx->position, LV_ANIM_OFF);
    cover_popup_set_value_text(ctx, ctx->position);
}

static void cover_popup_close(w_cover_tile_t *ctx)
{
    if (ctx != NULL && ctx->popup_overlay != NULL) {
        lv_obj_delete(ctx->popup_overlay);
    }
}

static void cover_popup_delete_cb(lv_event_t *event)
{
    w_cover_tile_t *ctx = (w_cover_tile_t *)lv_event_get_user_data(event);
    if (ctx != NULL) {
        ctx->popup_overlay = NULL;
        ctx->popup_slider = NULL;
        ctx->popup_value = NULL;
    }
}

static void cover_popup_close_cb(lv_event_t *event)
{
    w_cover_tile_t *ctx = (w_cover_tile_t *)lv_event_get_user_data(event);
    /* Taps on the dimmed area close the popup; taps inside the card do not
     * reach the overlay (the card stops event bubbling). */
    if (lv_event_get_target(event) == lv_event_get_current_target(event)) {
        cover_popup_close(ctx);
    }
}

static void cover_popup_slider_cb(lv_event_t *event)
{
    w_cover_tile_t *ctx = (w_cover_tile_t *)lv_event_get_user_data(event);
    if (ctx == NULL || ctx->popup_slider == NULL) {
        return;
    }
    const int value = (int)lv_slider_get_value(ctx->popup_slider);
    const lv_event_code_t code = lv_event_get_code(event);
    if (code == LV_EVENT_VALUE_CHANGED) {
        cover_popup_set_value_text(ctx, value);
    } else if (code == LV_EVENT_RELEASED) {
        cover_send_position(ctx, value);
    }
}

static void cover_popup_cmd_cb(lv_event_t *event)
{
    w_cover_tile_t *ctx = (w_cover_tile_t *)lv_event_get_user_data(event);
    lv_obj_t *button = lv_event_get_target(event);
    if (ctx == NULL || button == NULL || ctx->unavailable) {
        return;
    }
    cover_send(ctx, (w_cover_cmd_t)(uintptr_t)lv_obj_get_user_data(button));
}

static lv_obj_t *cover_popup_button(lv_obj_t *parent, w_cover_tile_t *ctx, w_cover_cmd_t cmd, lv_coord_t w,
    lv_coord_t h)
{
    lv_obj_t *button = lv_obj_create(parent);
    lv_obj_remove_style_all(button);
    lv_obj_set_size(button, w, h);
    lv_obj_add_flag(button, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(button, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(button, 14, LV_PART_MAIN);
    lv_obj_set_style_bg_color(button, lv_color_hex(APP_UI_COLOR_NAV_BTN_BG_IDLE), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(button, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(button, lv_color_hex(APP_UI_COLOR_NAV_BTN_BG_ACTIVE), LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_set_style_border_width(button, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(button, lv_color_hex(cover_cmd_accent(cmd)), LV_PART_MAIN);
    lv_obj_set_user_data(button, (void *)(uintptr_t)cmd);
    lv_obj_add_event_cb(button, cover_popup_cmd_cb, LV_EVENT_CLICKED, ctx);

    lv_obj_t *label = lv_label_create(button);
    lv_label_set_text(label, cover_i18n(cover_cmd_label_key(cmd), cover_cmd_label_fallback(cmd)));
    lv_obj_set_style_text_font(label, cover_font_px(20), LV_PART_MAIN);
    lv_obj_set_style_text_color(label, lv_color_hex(APP_UI_COLOR_TEXT_PRIMARY), LV_PART_MAIN);
    lv_obj_center(label);
    return button;
}

static void cover_popup_open(w_cover_tile_t *ctx)
{
    if (ctx == NULL || ctx->popup_overlay != NULL || ctx->unavailable) {
        return;
    }

    lv_obj_t *overlay = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(overlay);
    lv_obj_set_size(overlay, LV_PCT(100), LV_PCT(100));
    lv_obj_add_flag(overlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(overlay, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(overlay, LV_OPA_60, LV_PART_MAIN);
    lv_obj_add_event_cb(overlay, cover_popup_delete_cb, LV_EVENT_DELETE, ctx);
    lv_obj_add_event_cb(overlay, cover_popup_close_cb, LV_EVENT_CLICKED, ctx);
    ctx->popup_overlay = overlay;

    lv_obj_update_layout(overlay);
    const lv_coord_t screen_h = lv_obj_get_height(overlay);
    const lv_coord_t card_h = LV_MIN(screen_h - 40, 440);
    const lv_coord_t card_w = ctx->supports_position ? 340 : 260;

    lv_obj_t *card = lv_obj_create(overlay);
    lv_obj_set_size(card, card_w, card_h);
    lv_obj_center(card);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_set_style_radius(card, 16, LV_PART_MAIN);
    lv_obj_set_style_bg_color(card, lv_color_hex(APP_UI_COLOR_CONTENT_BG), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(card, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(card, lv_color_hex(APP_UI_COLOR_CONTENT_BORDER), LV_PART_MAIN);
    lv_obj_set_style_pad_all(card, 16, LV_PART_MAIN);

    lv_obj_t *title = lv_label_create(card);
    lv_label_set_text(title, (ctx->title_label != NULL) ? lv_label_get_text(ctx->title_label) : ctx->entity_id);
    lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);
    lv_obj_set_width(title, card_w - 32 - 46);
    lv_obj_set_style_text_font(title, cover_font_px(20), LV_PART_MAIN);
    lv_obj_set_style_text_color(title, lv_color_hex(APP_UI_COLOR_TEXT_PRIMARY), LV_PART_MAIN);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 0, 4);

    lv_obj_t *close = lv_obj_create(card);
    lv_obj_remove_style_all(close);
    lv_obj_set_size(close, 38, 38);
    lv_obj_add_flag(close, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_radius(close, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(close, lv_color_hex(APP_UI_COLOR_NAV_BTN_BG_IDLE), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(close, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_align(close, LV_ALIGN_TOP_RIGHT, 0, -2);
    lv_obj_t *close_label = lv_label_create(close);
    lv_label_set_text(close_label, "X");
    lv_obj_set_style_text_color(close_label, lv_color_hex(APP_UI_COLOR_TEXT_PRIMARY), LV_PART_MAIN);
    lv_obj_center(close_label);
    lv_obj_add_event_cb(close, cover_popup_close_cb, LV_EVENT_CLICKED, ctx);

    const lv_coord_t body_y = 52;
    const lv_coord_t body_h = card_h - 32 - body_y;
    const lv_coord_t button_w = ctx->supports_position ? 128 : card_w - 32;
    const lv_coord_t button_h = LV_MIN(64, (body_h - 2 * 12) / 3);

    if (ctx->supports_position) {
        /* Value above the slider; the slider runs bottom (closed) to top (open). */
        ctx->popup_value = lv_label_create(card);
        lv_obj_set_style_text_font(ctx->popup_value, cover_font_px(28), LV_PART_MAIN);
        lv_obj_set_style_text_color(ctx->popup_value, lv_color_hex(APP_UI_COLOR_TEXT_PRIMARY), LV_PART_MAIN);
        lv_obj_set_width(ctx->popup_value, 140);
        lv_obj_set_style_text_align(ctx->popup_value, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
        lv_obj_align(ctx->popup_value, LV_ALIGN_TOP_LEFT, 0, body_y);

        const lv_coord_t value_h = 44;
        lv_obj_t *slider = lv_slider_create(card);
        lv_slider_set_range(slider, 0, 100);
        lv_obj_set_size(slider, 76, body_h - value_h - 8);
        lv_obj_align(slider, LV_ALIGN_TOP_LEFT, (140 - 76) / 2, body_y + value_h + 4);
        lv_obj_set_style_radius(slider, 22, LV_PART_MAIN);
        lv_obj_set_style_bg_color(slider, lv_color_hex(APP_UI_COLOR_CARD_BG_OFF), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(slider, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_border_width(slider, 1, LV_PART_MAIN);
        lv_obj_set_style_border_color(slider, lv_color_hex(APP_UI_COLOR_CARD_BORDER), LV_PART_MAIN);
        lv_obj_set_style_radius(slider, 22, LV_PART_INDICATOR);
        lv_obj_set_style_bg_color(slider, lv_color_hex(APP_UI_COLOR_CARD_ICON_ON), LV_PART_INDICATOR);
        lv_obj_set_style_bg_opa(slider, LV_OPA_COVER, LV_PART_INDICATOR);
        /* Slim bar instead of a round knob, like a blind's bottom rail. */
        lv_obj_set_style_radius(slider, 4, LV_PART_KNOB);
        lv_obj_set_style_bg_color(slider, lv_color_hex(APP_UI_COLOR_TEXT_PRIMARY), LV_PART_KNOB);
        lv_obj_set_style_pad_hor(slider, 6, LV_PART_KNOB);
        lv_obj_set_style_pad_ver(slider, -32, LV_PART_KNOB); /* 76 px square -> ~12 px bar */
        lv_obj_set_ext_click_area(slider, 12);
        lv_obj_add_event_cb(slider, cover_popup_slider_cb, LV_EVENT_VALUE_CHANGED, ctx);
        lv_obj_add_event_cb(slider, cover_popup_slider_cb, LV_EVENT_RELEASED, ctx);
        ctx->popup_slider = slider;

        const int start = ctx->have_position ? ctx->position : (ctx->is_open ? 100 : 0);
        lv_slider_set_value(slider, start, LV_ANIM_OFF);
        cover_popup_set_value_text(ctx, start);
    }

    /* Open / stop / close stacked on the right (or full width). */
    const w_cover_cmd_t order[3] = {W_COVER_CMD_OPEN, W_COVER_CMD_STOP, W_COVER_CMD_CLOSE};
    const lv_coord_t stack_h = 3 * button_h + 2 * 12;
    const lv_coord_t stack_y = body_y + (body_h - stack_h) / 2;
    for (int i = 0; i < 3; i++) {
        lv_obj_t *button = cover_popup_button(card, ctx, order[i], button_w, button_h);
        lv_obj_align(button, LV_ALIGN_TOP_RIGHT, 0, stack_y + i * (button_h + 12));
    }
}

static void cover_card_event_cb(lv_event_t *event)
{
    const lv_event_code_t code = lv_event_get_code(event);
    w_cover_tile_t *ctx = (w_cover_tile_t *)lv_event_get_user_data(event);
    if (ctx == NULL) {
        return;
    }

    if (code == LV_EVENT_SIZE_CHANGED) {
        cover_layout(ctx);
        return;
    }

    if (code == LV_EVENT_DELETE) {
        if (ctx->timer != NULL) {
            lv_timer_del(ctx->timer);
            ctx->timer = NULL;
        }
        /* The popup lives on the top layer and points at ctx. */
        cover_popup_close(ctx);
        free(ctx);
        return;
    }

    if (code == LV_EVENT_LONG_PRESSED) {
        /* A long press that started on the rail stays a drag. */
        if (!ctx->dragging) {
            ctx->long_press_guard = true;
            cover_popup_open(ctx);
        }
        return;
    }

    if (code == LV_EVENT_PRESSED || code == LV_EVENT_PRESSING) {
        if (code == LV_EVENT_PRESSED) {
            /* A new touch always starts a fresh interaction. */
            ctx->slider_click_guard = false;
            ctx->long_press_guard = false;
        }
        if (ctx->unavailable || !ctx->supports_position || ctx->track == NULL ||
            lv_obj_has_flag(ctx->track, LV_OBJ_FLAG_HIDDEN)) {
            return;
        }
        lv_indev_t *indev = lv_indev_active();
        if (indev == NULL) {
            return;
        }
        lv_point_t point = {0};
        lv_indev_get_point(indev, &point);

        if (code == LV_EVENT_PRESSING) {
            if (ctx->dragging) {
                cover_slider_update_drag(ctx, &point);
            }
            return;
        }
        if (cover_slider_band_contains(ctx->card, ctx->track, &point)) {
            cover_slider_begin_drag(ctx, &point);
        }
        return;
    }

    if (code == LV_EVENT_RELEASED) {
        if (ctx->dragging) {
            cover_slider_end_drag(ctx);
        }
        return;
    }

    if (code == LV_EVENT_PRESS_LOST) {
        if (ctx->dragging) {
            ctx->dragging = false;
            cover_render(ctx);
            cover_layout(ctx);
        }
        return;
    }

    if (code == LV_EVENT_CLICKED) {
        /* The tap that just moved the slider must not also toggle the cover. */
        if (ctx->slider_click_guard) {
            ctx->slider_click_guard = false;
            return;
        }
        if (ctx->long_press_guard) {
            ctx->long_press_guard = false;
            return;
        }
        if (lv_event_get_target(event) != ctx->card) {
            return;
        }
        cover_toggle(ctx);
    }
}

static lv_obj_t *cover_make_button(lv_obj_t *card, w_cover_cmd_t cmd, w_cover_tile_t *ctx)
{
    lv_obj_t *button = lv_btn_create(card);
    if (button == NULL) {
        return NULL;
    }
    lv_obj_clear_flag(button, LV_OBJ_FLAG_SCROLLABLE);
    /* Keep the click out of the card so it never doubles as a tile toggle. */
    lv_obj_clear_flag(button, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_set_user_data(button, (void *)(intptr_t)cmd);
    cover_style_button(button, APP_UI_COLOR_NAV_BTN_BG_IDLE, APP_UI_COLOR_CARD_BORDER,
                       APP_UI_COLOR_TEXT_PRIMARY, LV_OPA_COVER);
    lv_obj_add_event_cb(button, cover_button_event_cb, LV_EVENT_CLICKED, ctx);

    lv_obj_t *label = lv_label_create(button);
    if (label != NULL) {
        lv_label_set_text(label, cover_i18n(cover_cmd_label_key(cmd), cover_cmd_label_fallback(cmd)));
        lv_obj_set_style_text_font(label, APP_FONT_TEXT_16, LV_PART_MAIN);
        lv_obj_set_style_text_color(label, lv_color_hex(APP_UI_COLOR_TEXT_PRIMARY), LV_PART_MAIN);
        lv_obj_center(label);
    }
    return button;
}

esp_err_t w_cover_tile_create(const ui_widget_def_t *def, lv_obj_t *parent, ui_widget_instance_t *out_instance)
{
    if (def == NULL || parent == NULL || out_instance == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    lv_obj_t *card = lv_obj_create(parent);
    if (card == NULL) {
        return ESP_ERR_NO_MEM;
    }
    lv_obj_remove_style_all(card);
    theme_default_style_card(card);
    lv_obj_set_style_bg_color(card, lv_color_hex(APP_UI_COLOR_CARD_BG_OFF), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(card, 16, LV_PART_MAIN);
    lv_obj_set_style_border_width(card, 1, LV_PART_MAIN);
    lv_obj_set_style_pad_all(card, 8, LV_PART_MAIN);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
    /* Position and size must be applied AFTER remove_style_all/theming. */
    lv_obj_set_pos(card, def->x, def->y);
    lv_obj_set_size(card, def->w, def->h);

    w_cover_tile_t *ctx = (w_cover_tile_t *)ui_calloc_prefer_psram(1, sizeof(w_cover_tile_t));
    if (ctx == NULL) {
        lv_obj_del(card);
        return ESP_ERR_NO_MEM;
    }
    ctx->card = card;
    ctx->position = -1;
    ctx->drag_position = 0;
    ctx->pending_cmd = W_COVER_CMD_COUNT;
    /* Assume the entity can be positioned; HA corrects this on the first state. */
    ctx->supports_position = true;
    strlcpy(ctx->entity_id, def->entity_id, sizeof(ctx->entity_id));

    const bool title_is_auto_id = (def->title[0] != '\0' && strcmp(def->title, def->id) == 0);
    if (def->title[0] != '\0' && !title_is_auto_id) {
        lv_obj_t *title = lv_label_create(card);
        lv_obj_add_flag(title, LV_OBJ_FLAG_USER_1);
        ctx->title_label = title;
        lv_obj_set_style_text_color(title, lv_color_hex(APP_UI_COLOR_TEXT_MUTED), LV_PART_MAIN);
        lv_obj_set_style_text_font(title, cover_font_px(16), LV_PART_MAIN);
        lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);
        lv_obj_set_width(title, lv_pct(100));
        lv_label_set_text(title, def->title);
        lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 0);
    }

    if (cover_glyph_available(W_COVER_GLYPH_OPEN)) {
        lv_obj_t *icon = lv_label_create(card);
        ctx->icon = icon;
        lv_obj_set_style_text_font(icon, mdi_font_icon_56(), LV_PART_MAIN);
        lv_obj_set_style_text_color(icon, lv_color_hex(APP_UI_COLOR_CARD_ICON_OFF), LV_PART_MAIN);
        cover_set_glyph(icon, W_COVER_GLYPH_OPEN);
    }

    lv_obj_t *value = lv_label_create(card);
    lv_obj_add_flag(value, LV_OBJ_FLAG_USER_3);
    ctx->value_label = value;
    lv_label_set_long_mode(value, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_align(value, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_width(value, lv_pct(100));
    lv_obj_set_style_text_color(value, lv_color_hex(APP_UI_COLOR_TEXT_MUTED), LV_PART_MAIN);
    lv_obj_set_style_text_font(value, cover_font_px(28), LV_PART_MAIN);

    lv_obj_t *caption = lv_label_create(card);
    lv_obj_add_flag(caption, LV_OBJ_FLAG_USER_2);
    ctx->caption_label = caption;
    lv_obj_set_style_text_color(caption, lv_color_hex(APP_UI_COLOR_TEXT_SOFT), LV_PART_MAIN);
    lv_obj_set_style_text_font(caption, cover_font_px(14), LV_PART_MAIN);
    lv_obj_set_style_text_align(caption, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_label_set_long_mode(caption, LV_LABEL_LONG_DOT);
    lv_obj_set_width(caption, lv_pct(100));
    lv_label_set_text(caption, "");

    lv_obj_t *track = lv_obj_create(card);
    if (track != NULL) {
        lv_obj_remove_style_all(track);
        lv_obj_clear_flag(track, LV_OBJ_FLAG_SCROLLABLE);
        /* Leave the presses to the card so the whole band around the rail works. */
        lv_obj_clear_flag(track, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_bg_color(track, lv_color_hex(APP_UI_COLOR_CARD_BORDER), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(track, LV_OPA_COVER, LV_PART_MAIN);
        ctx->track = track;

        lv_obj_t *fill = lv_obj_create(track);
        if (fill != NULL) {
            lv_obj_remove_style_all(fill);
            lv_obj_clear_flag(fill, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_clear_flag(fill, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_set_style_bg_color(fill, lv_color_hex(APP_UI_COLOR_STATE_ON), LV_PART_MAIN);
            lv_obj_set_style_bg_opa(fill, LV_OPA_COVER, LV_PART_MAIN);
            ctx->fill = fill;
        }
    }

    ctx->button_count = W_COVER_CMD_COUNT;
    for (int i = 0; i < ctx->button_count; i++) {
        ctx->buttons[i].cmd = (w_cover_cmd_t)i;
        ctx->buttons[i].button = cover_make_button(card, (w_cover_cmd_t)i, ctx);
        ctx->buttons[i].label = (ctx->buttons[i].button != NULL)
                                    ? lv_obj_get_child(ctx->buttons[i].button, 0)
                                    : NULL;
    }

    lv_obj_add_event_cb(card, cover_card_event_cb, LV_EVENT_SIZE_CHANGED, ctx);
    lv_obj_add_event_cb(card, cover_card_event_cb, LV_EVENT_PRESSED, ctx);
    lv_obj_add_event_cb(card, cover_card_event_cb, LV_EVENT_PRESSING, ctx);
    lv_obj_add_event_cb(card, cover_card_event_cb, LV_EVENT_RELEASED, ctx);
    lv_obj_add_event_cb(card, cover_card_event_cb, LV_EVENT_PRESS_LOST, ctx);
    lv_obj_add_event_cb(card, cover_card_event_cb, LV_EVENT_CLICKED, ctx);
    lv_obj_add_event_cb(card, cover_card_event_cb, LV_EVENT_DELETE, ctx);
    lv_obj_add_event_cb(card, cover_card_event_cb, LV_EVENT_LONG_PRESSED, ctx);

    ctx->accent = APP_UI_COLOR_TEXT_MUTED;
    strlcpy(ctx->caption_text, cover_i18n("common.unavailable", "unavailable"), sizeof(ctx->caption_text));
    cover_render(ctx);
    lv_obj_set_style_border_color(card, lv_color_hex(APP_UI_COLOR_CARD_BORDER), LV_PART_MAIN);
    cover_update_buttons(ctx);
    cover_layout(ctx);

    ctx->timer = lv_timer_create(cover_timer_cb, 1000, ctx);

    out_instance->obj = card;
    out_instance->ctx = ctx;
    return ESP_OK;
}

void w_cover_tile_apply_state(ui_widget_instance_t *instance, const ha_state_t *state)
{
    if (instance == NULL || instance->obj == NULL || state == NULL) {
        return;
    }
    w_cover_tile_t *ctx = (w_cover_tile_t *)instance->ctx;
    if (ctx == NULL) {
        return;
    }

    ctx->busy = false;

    if (cover_state_is_unavailable(state->state)) {
        ctx->unavailable = true;
        ctx->have_position = false;
        ctx->position = -1;
        ctx->is_open = false;
        ctx->state_opening = false;
        ctx->state_closing = false;
        ctx->pending_cmd = W_COVER_CMD_COUNT;
        ctx->dragging = false;
        strlcpy(ctx->caption_text, cover_i18n("common.unavailable", "unavailable"), sizeof(ctx->caption_text));
        cover_render(ctx);
        cover_update_buttons(ctx);
        cover_layout(ctx);
        return;
    }

    ctx->unavailable = false;
    ctx->state_opening = (strcmp(state->state, "opening") == 0);
    ctx->state_closing = (strcmp(state->state, "closing") == 0);

    int position = -1;
    if (state->attributes_json[0] != '\0') {
        cJSON *attrs = cJSON_Parse(state->attributes_json);
        if (attrs != NULL) {
            cJSON *pos = cJSON_GetObjectItemCaseSensitive(attrs, "current_position");
            if (cJSON_IsNumber(pos)) {
                position = (int)pos->valuedouble;
                if (position < 0) {
                    position = 0;
                }
                if (position > 100) {
                    position = 100;
                }
            }
            cJSON *features = cJSON_GetObjectItemCaseSensitive(attrs, "supported_features");
            if (cJSON_IsNumber(features)) {
                ctx->supports_position =
                    (((uint32_t)features->valuedouble & W_COVER_FEATURE_SET_POSITION) != 0);
            }
            cJSON_Delete(attrs);
        }
    }

    ctx->have_position = (position >= 0);
    ctx->position = position;

    /* A settled state means the command we sent has arrived. */
    if (!ctx->state_opening && !ctx->state_closing) {
        ctx->pending_cmd = W_COVER_CMD_COUNT;
    }

    bool open_like;
    if (strcmp(state->state, "opening") == 0) {
        open_like = true;
    } else if (strcmp(state->state, "closing") == 0) {
        open_like = false;
    } else if (ctx->have_position) {
        open_like = (position > 0);
    } else {
        open_like = (strcmp(state->state, "open") == 0);
    }
    ctx->is_open = open_like;

    const char *caption = NULL;
    uint32_t accent = APP_UI_COLOR_TEXT_MUTED;
    cover_state_style(state->state, &caption, &accent);
    ctx->accent = accent;
    strlcpy(ctx->caption_text, caption != NULL ? caption : "", sizeof(ctx->caption_text));

    /* While the finger is on the rail its preview wins over the HA echo. */
    if (ctx->dragging) {
        return;
    }

    cover_render(ctx);
    cover_update_buttons(ctx);
    cover_layout(ctx);
    cover_popup_sync(ctx);
}

void w_cover_tile_mark_unavailable(ui_widget_instance_t *instance)
{
    if (instance == NULL || instance->obj == NULL) {
        return;
    }
    w_cover_tile_t *ctx = (w_cover_tile_t *)instance->ctx;
    if (ctx == NULL) {
        return;
    }

    ctx->busy = false;
    ctx->unavailable = true;
    ctx->have_position = false;
    ctx->position = -1;
    ctx->is_open = false;
    ctx->state_opening = false;
    ctx->state_closing = false;
    ctx->pending_cmd = W_COVER_CMD_COUNT;
    ctx->dragging = false;
    strlcpy(ctx->caption_text, cover_i18n("common.unavailable", "unavailable"), sizeof(ctx->caption_text));
    cover_render(ctx);
    cover_update_buttons(ctx);
    cover_layout(ctx);
}
