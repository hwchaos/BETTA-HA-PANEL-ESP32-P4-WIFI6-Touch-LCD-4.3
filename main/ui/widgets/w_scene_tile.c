/* SPDX-License-Identifier: LicenseRef-FNCL-1.1
 * Copyright (c) 2026 Cpt_Kirk
 *
 * Scene tile - runs a Home Assistant scene (scene.* entities). A scene has no
 * meaningful state, so the whole tile is one large button: the card and the icon
 * light up in the accent colour while the call is in flight and the outcome is
 * reported in the status line.
 */
#include "ui/ui_widget_factory.h"

#include <ctype.h>
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

static const char *TAG = "w_scene_tile";

#define W_SCENE_GLYPH_PRIMARY  0xF0E09U /* wallpaper */
#define W_SCENE_GLYPH_SECOND   0xF050EU /* theme-light-dark */
#define W_SCENE_ACCENT_DEFAULT 0x7C6BFFU
#define W_SCENE_TIMER_PERIOD   200
#define W_SCENE_BUSY_TIMEOUT_MS 6000
#define W_SCENE_FLASH_MS 900
#define W_SCENE_FAILED_MS 4000

typedef enum {
    W_SCENE_IDLE = 0,
    W_SCENE_BUSY,
    W_SCENE_FAILED,
} w_scene_state_t;

typedef struct {
    char entity_id[APP_MAX_ENTITY_ID_LEN];
    char name[APP_MAX_NAME_LEN];
    lv_obj_t *card;
    lv_obj_t *icon;
    lv_obj_t *name_label;
    lv_obj_t *status_label;
    lv_timer_t *timer;
    uint32_t accent;
    uint32_t state_until_ms;
    uint32_t flash_until_ms;
    w_scene_state_t state;
    bool have_entity;
    bool have_glyph;
    bool active;
    bool explicit_title;
    bool unavailable;
} w_scene_tile_t;

static void scene_timer_cb(lv_timer_t *timer);

static const char *scene_i18n(const char *key, const char *fallback)
{
    const char *text = ui_i18n_get(key, NULL);
    return (text != NULL && text[0] != '\0') ? text : fallback;
}

static const lv_font_t *scene_font_px(int px)
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

static const lv_font_t *scene_fit_font(const char *text, int max_width, int max_height)
{
    static const int sizes[] = {28, 24, 20, 18, 16, 14};
    const lv_font_t *result = scene_font_px(14);
    const char *label = (text != NULL) ? text : "";

    for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
        const lv_font_t *font = scene_font_px(sizes[i]);
        if (max_height > 0 && lv_font_get_line_height(font) > max_height) {
            continue;
        }
        lv_point_t size = {0, 0};
        lv_text_get_size(&size, label, font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
        if (max_width <= 0 || size.x <= max_width) {
            result = font;
            break;
        }
    }
    return result;
}

static bool scene_glyph_available(const lv_font_t *font, uint32_t cp)
{
    if (font == NULL) {
        return false;
    }
    lv_font_glyph_dsc_t dsc;
    return lv_font_get_glyph_dsc(font, &dsc, cp, 0);
}

static void scene_set_glyph(lv_obj_t *label, uint32_t cp)
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

/* "scene.lo_kitchen_night" -> "Lo Kitchen Night" so tiles stay readable even
 * when the user did not type a title. */
static void scene_pretty_name(const char *entity_id, char *out, size_t out_len)
{
    if (out == NULL || out_len == 0) {
        return;
    }
    out[0] = '\0';
    if (entity_id == NULL || entity_id[0] == '\0') {
        return;
    }

    const char *name = strchr(entity_id, '.');
    name = (name != NULL) ? (name + 1) : entity_id;

    bool upper = true;
    size_t written = 0;
    for (size_t i = 0; name[i] != '\0' && written + 1 < out_len; i++) {
        const char c = name[i];
        if (c == '_' || c == '-') {
            out[written++] = ' ';
            upper = true;
        } else if (upper) {
            out[written++] = (char)toupper((unsigned char)c);
            upper = false;
        } else {
            out[written++] = c;
        }
    }
    out[written] = '\0';
}

static bool scene_friendly_name(const ha_state_t *state, char *out, size_t out_len)
{
    if (state == NULL || out == NULL || out_len == 0) {
        return false;
    }
    out[0] = '\0';
    if (state->attributes_json[0] == '\0') {
        return false;
    }

    cJSON *attrs = cJSON_Parse(state->attributes_json);
    if (attrs == NULL) {
        return false;
    }

    bool found = false;
    cJSON *item = cJSON_GetObjectItemCaseSensitive(attrs, "friendly_name");
    if (cJSON_IsString(item) && item->valuestring != NULL && item->valuestring[0] != '\0') {
        strlcpy(out, item->valuestring, out_len);
        found = true;
    }
    cJSON_Delete(attrs);
    return found;
}

static void scene_set_status(w_scene_tile_t *ctx, const char *text, uint32_t color)
{
    if (ctx == NULL || ctx->status_label == NULL) {
        return;
    }
    lv_label_set_text(ctx->status_label, text != NULL ? text : "");
    lv_obj_set_style_text_color(ctx->status_label, lv_color_hex(color), LV_PART_MAIN);
}

static void scene_apply_visual(w_scene_tile_t *ctx)
{
    if (ctx == NULL || ctx->card == NULL) {
        return;
    }

    uint32_t bg = APP_UI_COLOR_CARD_BG_ON;
    uint32_t border = APP_UI_COLOR_CARD_BORDER;
    uint32_t icon_color = ctx->accent;
    uint32_t name_color = APP_UI_COLOR_TEXT_PRIMARY;
    uint32_t status_color = APP_UI_COLOR_TEXT_MUTED;
    const char *status = scene_i18n("scene.hint", "Tap to run");

    if (ctx->unavailable) {
        bg = APP_UI_COLOR_CARD_BG_OFF;
        border = APP_UI_COLOR_CARD_BORDER;
        icon_color = APP_UI_COLOR_CARD_ICON_OFF;
        name_color = APP_UI_COLOR_TEXT_MUTED;
        status = scene_i18n("common.unavailable", "Unavailable");
    } else if (ctx->state == W_SCENE_BUSY) {
        border = ctx->accent;
        icon_color = ctx->accent;
        status = scene_i18n("scene.running", "Running...");
        status_color = ctx->accent;
    } else if (ctx->state == W_SCENE_FAILED) {
        bg = APP_UI_COLOR_CARD_BG_OFF;
        border = APP_UI_COLOR_ERROR;
        icon_color = APP_UI_COLOR_ERROR;
        status = scene_i18n("scene.failed", "Command failed");
        status_color = APP_UI_COLOR_ERROR;
    } else if (ctx->active) {
        border = ctx->accent;
        icon_color = ctx->accent;
        status = scene_i18n("scene.done", "Done");
        status_color = ctx->accent;
    }

    const bool emphasised = (ctx->state == W_SCENE_BUSY) || ctx->active;
    lv_obj_set_style_bg_color(ctx->card, lv_color_hex(bg), LV_PART_MAIN);
    lv_obj_set_style_border_color(ctx->card, lv_color_hex(border), LV_PART_MAIN);
    lv_obj_set_style_border_width(ctx->card, emphasised ? 3 : 1, LV_PART_MAIN);
    if (ctx->icon != NULL) {
        lv_obj_set_style_text_color(ctx->icon, lv_color_hex(icon_color), LV_PART_MAIN);
    }
    if (ctx->name_label != NULL) {
        lv_obj_set_style_text_color(ctx->name_label, lv_color_hex(name_color), LV_PART_MAIN);
    }
    scene_set_status(ctx, status, status_color);
}

static void scene_layout(w_scene_tile_t *ctx)
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

    const lv_coord_t status_h = (card_h >= 150) ? 20 : ((card_h >= 110) ? 16 : 0);
    const lv_coord_t inner_w = card_w - (2 * pad);
    const lv_coord_t inner_h = card_h - (2 * pad) - status_h;

    if (ctx->status_label != NULL) {
        if (status_h > 0) {
            lv_obj_clear_flag(ctx->status_label, LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_style_text_font(ctx->status_label, scene_font_px(status_h >= 20 ? 16 : 14), LV_PART_MAIN);
            lv_obj_set_width(ctx->status_label, inner_w);
            lv_obj_align(ctx->status_label, LV_ALIGN_BOTTOM_MID, 0, 0);
        } else {
            lv_obj_add_flag(ctx->status_label, LV_OBJ_FLAG_HIDDEN);
        }
    }

    const lv_font_t *name_font = scene_fit_font(ctx->name, inner_w, LV_MAX((lv_coord_t)16, inner_h / 2));
    const lv_coord_t name_h = (lv_coord_t)lv_font_get_line_height(name_font);
    if (ctx->name_label != NULL) {
        lv_obj_set_style_text_font(ctx->name_label, name_font, LV_PART_MAIN);
        lv_obj_set_width(ctx->name_label, inner_w);
        lv_obj_align(ctx->name_label, LV_ALIGN_TOP_MID, 0, LV_MAX((lv_coord_t)0, inner_h - name_h));
    }

    lv_coord_t icon_h = inner_h - name_h - 4;
    const lv_font_t *icon_font = NULL;
    if (ctx->icon != NULL && ctx->have_glyph) {
        if (icon_h >= 50 && inner_w >= 50) {
            icon_font = mdi_font_icon_56();
        } else if (icon_h >= 36 && inner_w >= 36) {
            icon_font = mdi_font_icon_42();
        }
    }

    if (ctx->icon != NULL) {
        if (icon_font != NULL) {
            lv_obj_clear_flag(ctx->icon, LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_style_text_font(ctx->icon, icon_font, LV_PART_MAIN);
            lv_obj_align(ctx->icon, LV_ALIGN_TOP_MID, 0, 0);
        } else {
            lv_obj_add_flag(ctx->icon, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

static void scene_ensure_timer(w_scene_tile_t *ctx)
{
    if (ctx->timer == NULL) {
        ctx->timer = lv_timer_create(scene_timer_cb, W_SCENE_TIMER_PERIOD, ctx);
    }
    if (ctx->timer != NULL) {
        lv_timer_reset(ctx->timer);
        lv_timer_resume(ctx->timer);
    }
}

static void scene_timer_cb(lv_timer_t *timer)
{
    system_log_note_lvgl_cb("scene_timer_cb");
    w_scene_tile_t *ctx = (timer != NULL) ? (w_scene_tile_t *)lv_timer_get_user_data(timer) : NULL;
    if (ctx == NULL) {
        return;
    }

    const uint32_t now = lv_tick_get();
    bool changed = false;

    if (ctx->state != W_SCENE_IDLE && (int32_t)(now - ctx->state_until_ms) >= 0) {
        ctx->state = W_SCENE_IDLE;
        changed = true;
    }
    if (ctx->active && (int32_t)(now - ctx->flash_until_ms) >= 0) {
        ctx->active = false;
        changed = true;
    }
    if (changed) {
        scene_apply_visual(ctx);
    }

    if (ctx->state == W_SCENE_IDLE && !ctx->active) {
        ctx->timer = NULL;
        lv_timer_del(timer);
    }
}

static void scene_run(w_scene_tile_t *ctx)
{
    if (ctx == NULL || !ctx->have_entity || ctx->unavailable) {
        return;
    }
    if (ctx->state == W_SCENE_BUSY) {
        return;
    }

    char payload[APP_MAX_ENTITY_ID_LEN + 32];
    snprintf(payload, sizeof(payload), "{\"entity_id\":\"%s\"}", ctx->entity_id);

    const esp_err_t err = ha_client_call_service("scene", "turn_on", payload);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "scene.turn_on failed for %s: %s", ctx->entity_id, esp_err_to_name(err));
        ctx->state = W_SCENE_FAILED;
        ctx->state_until_ms = lv_tick_get() + W_SCENE_FAILED_MS;
        ctx->active = false;
        scene_apply_visual(ctx);
        scene_ensure_timer(ctx);
        return;
    }

    ctx->state = W_SCENE_BUSY;
    ctx->state_until_ms = lv_tick_get() + W_SCENE_BUSY_TIMEOUT_MS;
    ctx->active = true;
    ctx->flash_until_ms = lv_tick_get() + W_SCENE_FLASH_MS;
    scene_apply_visual(ctx);
    scene_ensure_timer(ctx);
}

static void scene_card_event_cb(lv_event_t *event)
{
    if (event == NULL) {
        return;
    }

    w_scene_tile_t *ctx = (w_scene_tile_t *)lv_event_get_user_data(event);
    if (ctx == NULL) {
        return;
    }

    const lv_event_code_t code = lv_event_get_code(event);
    if (code == LV_EVENT_SIZE_CHANGED) {
        scene_layout(ctx);
    } else if (code == LV_EVENT_CLICKED) {
        scene_run(ctx);
    } else if (code == LV_EVENT_DELETE) {
        if (ctx->timer != NULL) {
            lv_timer_del(ctx->timer);
            ctx->timer = NULL;
        }
        free(ctx);
    }
}

esp_err_t w_scene_tile_create(const ui_widget_def_t *def, lv_obj_t *parent, ui_widget_instance_t *out_instance)
{
    if (def == NULL || parent == NULL || out_instance == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    w_scene_tile_t *ctx = (w_scene_tile_t *)ui_calloc_prefer_psram(1, sizeof(w_scene_tile_t));
    if (ctx == NULL) {
        ESP_LOGE(TAG, "out of memory for scene tile");
        return ESP_ERR_NO_MEM;
    }

    ctx->accent = W_SCENE_ACCENT_DEFAULT;
    ctx->have_entity = (def->entity_id[0] != '\0');
    if (ctx->have_entity) {
        strlcpy(ctx->entity_id, def->entity_id, sizeof(ctx->entity_id));
    }
    if (def->title[0] != '\0' && strcmp(def->title, def->id) != 0) {
        strlcpy(ctx->name, def->title, sizeof(ctx->name));
        ctx->explicit_title = true;
    } else {
        scene_pretty_name(ctx->entity_id, ctx->name, sizeof(ctx->name));
    }

    lv_obj_t *card = lv_obj_create(parent);
    if (card == NULL) {
        free(ctx);
        return ESP_ERR_NO_MEM;
    }
    lv_obj_remove_style_all(card);
    theme_default_style_card(card);
    lv_obj_set_style_radius(card, 16, LV_PART_MAIN);
    lv_obj_set_style_border_width(card, 1, LV_PART_MAIN);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_pad_all(card, 8, LV_PART_MAIN);
    lv_obj_set_pos(card, def->x, def->y);
    lv_obj_set_size(card, def->w, def->h);
    ctx->card = card;

    lv_obj_t *icon = lv_label_create(card);
    ctx->icon = icon;
    lv_obj_add_flag(icon, LV_OBJ_FLAG_USER_1);
    lv_obj_set_style_text_color(icon, lv_color_hex(ctx->accent), LV_PART_MAIN);
    ctx->have_glyph = false;
    if (scene_glyph_available(mdi_font_icon_56(), W_SCENE_GLYPH_PRIMARY)) {
        lv_obj_set_style_text_font(icon, mdi_font_icon_56(), LV_PART_MAIN);
        scene_set_glyph(icon, W_SCENE_GLYPH_PRIMARY);
        ctx->have_glyph = true;
    } else if (scene_glyph_available(mdi_font_icon_42(), W_SCENE_GLYPH_SECOND)) {
        lv_obj_set_style_text_font(icon, mdi_font_icon_42(), LV_PART_MAIN);
        scene_set_glyph(icon, W_SCENE_GLYPH_SECOND);
        ctx->have_glyph = true;
    } else if (scene_glyph_available(mdi_font_icon_56(), W_SCENE_GLYPH_SECOND)) {
        lv_obj_set_style_text_font(icon, mdi_font_icon_56(), LV_PART_MAIN);
        scene_set_glyph(icon, W_SCENE_GLYPH_SECOND);
        ctx->have_glyph = true;
    } else {
        lv_obj_add_flag(icon, LV_OBJ_FLAG_HIDDEN);
    }

    lv_obj_t *name = lv_label_create(card);
    ctx->name_label = name;
    lv_obj_add_flag(name, LV_OBJ_FLAG_USER_3);
    lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(name, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_style_text_color(name, lv_color_hex(APP_UI_COLOR_TEXT_PRIMARY), LV_PART_MAIN);
    lv_obj_set_style_text_font(name, scene_font_px(20), LV_PART_MAIN);
    lv_obj_set_width(name, lv_pct(100));
    lv_label_set_text(name, ctx->name);

    lv_obj_t *status = lv_label_create(card);
    ctx->status_label = status;
    lv_obj_add_flag(status, LV_OBJ_FLAG_USER_2);
    lv_label_set_long_mode(status, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(status, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_style_text_color(status, lv_color_hex(APP_UI_COLOR_TEXT_MUTED), LV_PART_MAIN);
    lv_obj_set_style_text_font(status, scene_font_px(14), LV_PART_MAIN);
    lv_obj_set_width(status, lv_pct(100));
    lv_label_set_text(status, "");

    lv_obj_add_event_cb(card, scene_card_event_cb, LV_EVENT_SIZE_CHANGED, ctx);
    lv_obj_add_event_cb(card, scene_card_event_cb, LV_EVENT_CLICKED, ctx);
    lv_obj_add_event_cb(card, scene_card_event_cb, LV_EVENT_DELETE, ctx);

    scene_apply_visual(ctx);
    scene_layout(ctx);

    out_instance->obj = card;
    out_instance->ctx = ctx;
    return ESP_OK;
}

void w_scene_tile_apply_state(ui_widget_instance_t *instance, const ha_state_t *state)
{
    if (instance == NULL || instance->obj == NULL || state == NULL) {
        return;
    }
    w_scene_tile_t *ctx = (w_scene_tile_t *)instance->ctx;
    if (ctx == NULL) {
        return;
    }

    /* Scenes are stateless in Home Assistant - "unknown" is the normal resting
     * value, so only "unavailable" counts as offline. */
    const bool offline = (state->state[0] == '\0') || (strcmp(state->state, "unavailable") == 0);
    ctx->unavailable = offline;

    /* A fresh state update means the command landed: drop the pending marker,
     * the flash highlight keeps running for its short window. */
    ctx->state = W_SCENE_IDLE;

    if (!offline && !ctx->explicit_title) {
        char friendly[APP_MAX_NAME_LEN];
        if (scene_friendly_name(state, friendly, sizeof(friendly)) && strcmp(friendly, ctx->name) != 0) {
            strlcpy(ctx->name, friendly, sizeof(ctx->name));
            if (ctx->name_label != NULL) {
                lv_label_set_text(ctx->name_label, ctx->name);
            }
            scene_layout(ctx);
        }
    }

    scene_apply_visual(ctx);
}

void w_scene_tile_mark_unavailable(ui_widget_instance_t *instance)
{
    if (instance == NULL || instance->obj == NULL) {
        return;
    }
    w_scene_tile_t *ctx = (w_scene_tile_t *)instance->ctx;
    if (ctx == NULL) {
        return;
    }

    ctx->unavailable = true;
    ctx->state = W_SCENE_IDLE;
    ctx->active = false;
    scene_apply_visual(ctx);
}
