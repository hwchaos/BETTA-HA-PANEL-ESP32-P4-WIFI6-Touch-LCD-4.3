/* SPDX-License-Identifier: LicenseRef-FNCL-1.1
 * Copyright (c) 2026 Cpt_Kirk
 *
 * Person tile - shows where a person is (person.* entities). Home turns the
 * tile green, a named zone blue and not_home stays muted. The avatar is a
 * plain circle holding the first letter of the name, so no extra bitmap is
 * needed and every name stays readable. Display only - persons have no service
 * to call.
 */
#include "ui/ui_widget_factory.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_log.h"

#include "ui/fonts/app_text_fonts.h"
#include "ui/theme/theme_default.h"
#include "ui/ui_i18n.h"
#include "ui/ui_memory.h"

static const char *TAG = "w_person_tile";

#define W_PERSON_ACCENT_HOME 0x2ECC9AU
#define W_PERSON_ACCENT_ZONE 0x41BDF5U
#define W_PERSON_ACCENT_AWAY 0x8A93A5U

typedef struct {
    char entity_id[APP_MAX_ENTITY_ID_LEN];
    char name[APP_MAX_NAME_LEN];
    char status[APP_MAX_NAME_LEN];
    lv_obj_t *card;
    lv_obj_t *avatar;
    lv_obj_t *avatar_label;
    lv_obj_t *name_label;
    lv_obj_t *status_label;
    uint32_t accent;
    bool explicit_title;
    bool unavailable;
} w_person_tile_t;

static const char *person_i18n(const char *key, const char *fallback)
{
    const char *text = ui_i18n_get(key, NULL);
    return (text != NULL && text[0] != '\0') ? text : fallback;
}

static const lv_font_t *person_font_px(int px)
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

static const lv_font_t *person_fit_font(const char *text, int max_width, int max_height)
{
    static const int sizes[] = {28, 24, 20, 18, 16, 14};
    const lv_font_t *result = person_font_px(14);
    const char *label = (text != NULL) ? text : "";

    for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
        const lv_font_t *font = person_font_px(sizes[i]);
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

/* "person.jan_kowalski" -> "Jan Kowalski" for tiles without an explicit title. */
static void person_pretty_name(const char *entity_id, char *out, size_t out_len)
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

static void person_avatar_letter(const char *name, char *out, size_t out_len)
{
    if (out == NULL || out_len == 0) {
        return;
    }
    out[0] = '\0';
    if (name == NULL || name[0] == '\0') {
        strlcpy(out, "?", out_len);
        return;
    }

    size_t idx = 0;
    while (name[idx] == ' ') {
        idx++;
    }
    if (name[idx] == '\0') {
        strlcpy(out, "?", out_len);
        return;
    }

    char letter[2] = {(char)toupper((unsigned char)name[idx]), '\0'};
    strlcpy(out, letter, out_len);
}

/* Maps the person state to the status text plus accent colour. Returns false
 * for the stateless values (unavailable/unknown/empty). */
static bool person_map_state(const char *state, const char **out_status, uint32_t *out_accent)
{
    if (state == NULL || state[0] == '\0') {
        *out_status = person_i18n("common.unavailable", "Unavailable");
        *out_accent = APP_UI_COLOR_CARD_ICON_OFF;
        return false;
    }
    if (strcmp(state, "home") == 0) {
        *out_status = person_i18n("person.home", "Home");
        *out_accent = W_PERSON_ACCENT_HOME;
        return true;
    }
    if (strcmp(state, "not_home") == 0) {
        *out_status = person_i18n("person.away", "Away");
        *out_accent = W_PERSON_ACCENT_AWAY;
        return true;
    }
    if (strcmp(state, "unavailable") == 0 || strcmp(state, "unknown") == 0) {
        *out_status = person_i18n("common.unavailable", "Unavailable");
        *out_accent = APP_UI_COLOR_CARD_ICON_OFF;
        return false;
    }

    /* Any other value is the friendly name of the zone the person is in. */
    *out_status = state;
    *out_accent = W_PERSON_ACCENT_ZONE;
    return true;
}

static void person_apply_visual(w_person_tile_t *ctx)
{
    if (ctx == NULL || ctx->card == NULL) {
        return;
    }

    const bool known = !ctx->unavailable;
    const uint32_t accent = known ? ctx->accent : APP_UI_COLOR_CARD_ICON_OFF;

    lv_obj_set_style_bg_color(ctx->card,
        lv_color_hex(known ? APP_UI_COLOR_CARD_BG_ON : APP_UI_COLOR_CARD_BG_OFF), LV_PART_MAIN);
    lv_obj_set_style_border_color(ctx->card,
        lv_color_hex(known ? accent : APP_UI_COLOR_CARD_BORDER), LV_PART_MAIN);
    lv_obj_set_style_border_width(ctx->card, known ? 2 : 1, LV_PART_MAIN);

    if (ctx->avatar != NULL) {
        lv_obj_set_style_bg_color(ctx->avatar, lv_color_hex(accent), LV_PART_MAIN);
    }
    if (ctx->name_label != NULL) {
        lv_obj_set_style_text_color(ctx->name_label,
            lv_color_hex(known ? APP_UI_COLOR_TEXT_PRIMARY : APP_UI_COLOR_TEXT_MUTED), LV_PART_MAIN);
    }
    if (ctx->status_label != NULL) {
        lv_obj_set_style_text_color(ctx->status_label,
            lv_color_hex(known ? accent : APP_UI_COLOR_TEXT_MUTED), LV_PART_MAIN);
    }
}

static void person_layout(w_person_tile_t *ctx)
{
    if (ctx == NULL || ctx->card == NULL || ctx->avatar == NULL) {
        return;
    }

    const lv_coord_t card_w = lv_obj_get_width(ctx->card);
    const lv_coord_t card_h = lv_obj_get_height(ctx->card);
    if (card_w <= 0 || card_h <= 0) {
        return;
    }

    const lv_coord_t pad = (card_w < 240 || card_h < 180) ? 6 : 10;
    lv_obj_set_style_pad_all(ctx->card, pad, LV_PART_MAIN);

    const lv_coord_t inner_w = card_w - (2 * pad);
    const lv_coord_t inner_h = card_h - (2 * pad);
    const lv_coord_t status_h = (card_h >= 120) ? 18 : 0;
    const bool wide = (card_w >= ((card_h * 3) / 2));

    lv_coord_t avatar_size = 0;
    lv_coord_t text_x = 0;
    lv_coord_t text_w = 0;
    lv_coord_t text_h = 0;

    if (wide) {
        avatar_size = LV_MAX((lv_coord_t)28, LV_MIN(inner_h, (lv_coord_t)96));
        text_x = avatar_size + 8;
        text_w = LV_MAX((lv_coord_t)40, inner_w - text_x);
        text_h = inner_h;
        lv_obj_align(ctx->avatar, LV_ALIGN_LEFT_MID, 0, 0);
    } else {
        avatar_size = inner_h - status_h - 6;
        avatar_size = LV_MAX((lv_coord_t)28, LV_MIN(inner_w, avatar_size));
        text_x = 0;
        text_w = inner_w;
        text_h = inner_h - status_h - avatar_size;
        lv_obj_align(ctx->avatar, LV_ALIGN_TOP_MID, 0, 0);
    }
    lv_obj_set_size(ctx->avatar, avatar_size, avatar_size);

    if (ctx->avatar_label != NULL) {
        const int letter_px = (int)(avatar_size / 2);
        lv_obj_set_style_text_font(ctx->avatar_label, person_font_px(letter_px), LV_PART_MAIN);
        lv_obj_center(ctx->avatar_label);
    }

    const lv_coord_t name_h = LV_MAX((lv_coord_t)16, text_h / 2);
    const lv_font_t *name_font = person_fit_font(ctx->name, text_w, name_h);
    const lv_coord_t name_line_h = (lv_coord_t)lv_font_get_line_height(name_font);
    const lv_coord_t status_line_h = (lv_coord_t)lv_font_get_line_height(person_font_px(14));

    if (ctx->name_label != NULL) {
        lv_obj_set_style_text_font(ctx->name_label, name_font, LV_PART_MAIN);
        lv_obj_set_width(ctx->name_label, text_w);
        if (wide) {
            const lv_coord_t stack = name_line_h + ((status_h > 0) ? status_line_h : 0);
            lv_obj_align(ctx->name_label, LV_ALIGN_TOP_LEFT, text_x,
                LV_MAX((lv_coord_t)0, (inner_h - stack) / 2));
        } else {
            lv_obj_align(ctx->name_label, LV_ALIGN_TOP_MID, 0, inner_h - status_h - name_line_h);
        }
    }

    if (ctx->status_label != NULL) {
        if (status_h > 0) {
            lv_obj_clear_flag(ctx->status_label, LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_style_text_font(ctx->status_label, person_font_px(14), LV_PART_MAIN);
            lv_obj_set_width(ctx->status_label, wide ? text_w : inner_w);
            if (wide) {
                lv_obj_align_to(ctx->status_label, ctx->name_label, LV_ALIGN_OUT_BOTTOM_LEFT, 0, 2);
            } else {
                lv_obj_align(ctx->status_label, LV_ALIGN_BOTTOM_MID, 0, 0);
            }
        } else {
            lv_obj_add_flag(ctx->status_label, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

static void person_set_name(w_person_tile_t *ctx)
{
    if (ctx == NULL || ctx->name_label == NULL) {
        return;
    }

    lv_label_set_text(ctx->name_label, ctx->name);

    char letter[4] = {0};
    person_avatar_letter(ctx->name, letter, sizeof(letter));
    if (ctx->avatar_label != NULL) {
        lv_label_set_text(ctx->avatar_label, letter);
    }
}

static void person_card_event_cb(lv_event_t *event)
{
    if (event == NULL) {
        return;
    }

    w_person_tile_t *ctx = (w_person_tile_t *)lv_event_get_user_data(event);
    if (ctx == NULL) {
        return;
    }

    const lv_event_code_t code = lv_event_get_code(event);
    if (code == LV_EVENT_SIZE_CHANGED) {
        person_layout(ctx);
    } else if (code == LV_EVENT_DELETE) {
        free(ctx);
    }
}

esp_err_t w_person_tile_create(const ui_widget_def_t *def, lv_obj_t *parent, ui_widget_instance_t *out_instance)
{
    if (def == NULL || parent == NULL || out_instance == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    w_person_tile_t *ctx = (w_person_tile_t *)ui_calloc_prefer_psram(1, sizeof(w_person_tile_t));
    if (ctx == NULL) {
        ESP_LOGE(TAG, "out of memory for person tile");
        return ESP_ERR_NO_MEM;
    }

    ctx->accent = W_PERSON_ACCENT_AWAY;
    if (def->entity_id[0] != '\0') {
        strlcpy(ctx->entity_id, def->entity_id, sizeof(ctx->entity_id));
    }
    if (def->title[0] != '\0' && strcmp(def->title, def->id) != 0) {
        strlcpy(ctx->name, def->title, sizeof(ctx->name));
        ctx->explicit_title = true;
    } else {
        person_pretty_name(ctx->entity_id, ctx->name, sizeof(ctx->name));
    }
    strlcpy(ctx->status, person_i18n("common.unavailable", "Unavailable"), sizeof(ctx->status));

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
    lv_obj_set_style_pad_all(card, 8, LV_PART_MAIN);
    lv_obj_set_pos(card, def->x, def->y);
    lv_obj_set_size(card, def->w, def->h);
    ctx->card = card;

    lv_obj_t *avatar = lv_obj_create(card);
    lv_obj_remove_style_all(avatar);
    lv_obj_clear_flag(avatar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(avatar, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(avatar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(avatar, lv_color_hex(W_PERSON_ACCENT_AWAY), LV_PART_MAIN);
    ctx->avatar = avatar;

    lv_obj_t *avatar_label = lv_label_create(avatar);
    ctx->avatar_label = avatar_label;
    lv_obj_set_style_text_color(avatar_label, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_set_style_text_font(avatar_label, person_font_px(20), LV_PART_MAIN);
    lv_label_set_text(avatar_label, "");

    lv_obj_t *name = lv_label_create(card);
    ctx->name_label = name;
    lv_obj_add_flag(name, LV_OBJ_FLAG_USER_1);
    lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(name, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_style_text_color(name, lv_color_hex(APP_UI_COLOR_TEXT_PRIMARY), LV_PART_MAIN);
    lv_obj_set_style_text_font(name, person_font_px(20), LV_PART_MAIN);
    lv_obj_set_width(name, lv_pct(100));

    lv_obj_t *status = lv_label_create(card);
    ctx->status_label = status;
    lv_obj_add_flag(status, LV_OBJ_FLAG_USER_3);
    lv_label_set_long_mode(status, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(status, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_style_text_color(status, lv_color_hex(APP_UI_COLOR_TEXT_MUTED), LV_PART_MAIN);
    lv_obj_set_style_text_font(status, person_font_px(14), LV_PART_MAIN);
    lv_obj_set_width(status, lv_pct(100));
    lv_label_set_text(status, ctx->status);

    lv_obj_add_event_cb(card, person_card_event_cb, LV_EVENT_SIZE_CHANGED, ctx);
    lv_obj_add_event_cb(card, person_card_event_cb, LV_EVENT_DELETE, ctx);

    person_set_name(ctx);
    person_apply_visual(ctx);
    person_layout(ctx);

    out_instance->obj = card;
    out_instance->ctx = ctx;
    return ESP_OK;
}

void w_person_tile_apply_state(ui_widget_instance_t *instance, const ha_state_t *state)
{
    if (instance == NULL || instance->obj == NULL || state == NULL) {
        return;
    }
    w_person_tile_t *ctx = (w_person_tile_t *)instance->ctx;
    if (ctx == NULL) {
        return;
    }

    if (!ctx->explicit_title && state->attributes_json[0] != '\0') {
        cJSON *attrs = cJSON_Parse(state->attributes_json);
        if (attrs != NULL) {
            cJSON *friendly = cJSON_GetObjectItemCaseSensitive(attrs, "friendly_name");
            if (cJSON_IsString(friendly) && friendly->valuestring != NULL && friendly->valuestring[0] != '\0' &&
                strcmp(friendly->valuestring, ctx->name) != 0) {
                strlcpy(ctx->name, friendly->valuestring, sizeof(ctx->name));
                person_set_name(ctx);
                person_layout(ctx);
            }
            cJSON_Delete(attrs);
        }
    }

    const char *status = NULL;
    uint32_t accent = W_PERSON_ACCENT_AWAY;
    const bool known = person_map_state(state->state, &status, &accent);

    ctx->unavailable = !known;
    ctx->accent = accent;
    strlcpy(ctx->status,
        (status != NULL && status[0] != '\0') ? status : person_i18n("common.unavailable", "Unavailable"),
        sizeof(ctx->status));

    if (ctx->status_label != NULL) {
        lv_label_set_text(ctx->status_label, ctx->status);
    }
    person_apply_visual(ctx);
}

void w_person_tile_mark_unavailable(ui_widget_instance_t *instance)
{
    if (instance == NULL || instance->obj == NULL) {
        return;
    }
    w_person_tile_t *ctx = (w_person_tile_t *)instance->ctx;
    if (ctx == NULL) {
        return;
    }

    ctx->unavailable = true;
    strlcpy(ctx->status, person_i18n("common.unavailable", "Unavailable"), sizeof(ctx->status));
    if (ctx->status_label != NULL) {
        lv_label_set_text(ctx->status_label, ctx->status);
    }
    person_apply_visual(ctx);
}
