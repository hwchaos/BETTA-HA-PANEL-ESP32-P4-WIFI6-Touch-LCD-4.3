/* SPDX-License-Identifier: LicenseRef-FNCL-1.1
 * Copyright (c) 2026 Cpt_Kirk
 *
 * Clock tile.
 *
 * Shows the local time (optionally with seconds and the date) and refreshes
 * itself once per second.  The widget type string stays "clock_alarm" so that
 * layouts saved by older firmware versions keep working, but the tile no
 * longer schedules or rings an alarm.  This build has no audio output.
 */
#include "ui/ui_widget_factory.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "esp_log.h"

#include "app_config.h"
#include "diag/system_log.h"
#include "ui/fonts/app_text_fonts.h"
#include "ui/theme/theme_default.h"
#include "ui/ui_i18n.h"
#include "ui/ui_memory.h"

static const char *TAG = "w_clock";

#define CLOCK_ROW_GAP 6
#define CLOCK_PAD 12
#define CLOCK_ROWS_MAX 3

typedef struct {
    char title[APP_MAX_NAME_LEN];
    bool show_seconds;
    bool show_date;
    int width_px;
    lv_obj_t *card;
    lv_obj_t *title_label;
    lv_obj_t *time_label;
    lv_obj_t *date_label;
    lv_timer_t *timer;
} w_clock_t;

static const char *clock_i18n(const char *key, const char *fallback)
{
    const char *text = ui_i18n_get(key, NULL);
    return (text != NULL && text[0] != '\0') ? text : fallback;
}

/* Weekday index used by the date row: 0 = Monday .. 6 = Sunday. */
static int clock_weekday_index(const struct tm *info)
{
    return (info->tm_wday + 6) % 7;
}

static const char *clock_day_short(int index)
{
    static const struct {
        const char *key;
        const char *fallback;
    } DAYS[7] = {
        {"clock_alarm.day_mon", "Mon"},
        {"clock_alarm.day_tue", "Tue"},
        {"clock_alarm.day_wed", "Wed"},
        {"clock_alarm.day_thu", "Thu"},
        {"clock_alarm.day_fri", "Fri"},
        {"clock_alarm.day_sat", "Sat"},
        {"clock_alarm.day_sun", "Sun"},
    };
    if (index < 0 || index > 6) {
        return "";
    }
    return clock_i18n(DAYS[index].key, DAYS[index].fallback);
}

static void clock_style_label(lv_obj_t *label, const lv_font_t *font, uint32_t color, lv_label_long_mode_t mode)
{
    lv_obj_set_style_text_font(label, font, LV_PART_MAIN);
    lv_obj_set_style_text_color(label, lv_color_hex(color), LV_PART_MAIN);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_style_pad_all(label, 0, LV_PART_MAIN);
    lv_obj_set_width(label, lv_pct(100));
    lv_label_set_long_mode(label, mode);
}

/* The dedicated clock font only contains digits and the colon, so narrow tiles
 * fall back to the text fonts. */
static const lv_font_t *clock_time_font(int width_px, bool show_seconds)
{
    const lv_font_t *font;
    if (width_px >= 300) {
        font = APP_FONT_CLOCK_84;
    } else if (width_px >= 180) {
        font = APP_FONT_DISPLAY_40;
    } else if (width_px >= 130) {
        font = APP_FONT_TEXT_34;
    } else {
        font = APP_FONT_TEXT_24;
    }
    if (!show_seconds || width_px >= 280) {
        return font;
    }
    if (font == APP_FONT_CLOCK_84) {
        return APP_FONT_DISPLAY_40;
    }
    if (font == APP_FONT_DISPLAY_40) {
        return APP_FONT_TEXT_34;
    }
    return APP_FONT_TEXT_24;
}

static void clock_update_labels(w_clock_t *ctx, const struct tm *info)
{
    char buffer[96];

    if (ctx->time_label != NULL) {
        if (ctx->show_seconds) {
            snprintf(buffer, sizeof(buffer), "%02d:%02d:%02d", info->tm_hour, info->tm_min, info->tm_sec);
        } else {
            snprintf(buffer, sizeof(buffer), "%02d:%02d", info->tm_hour, info->tm_min);
        }
        lv_label_set_text(ctx->time_label, buffer);
    }

    if (ctx->date_label != NULL) {
        snprintf(buffer, sizeof(buffer), "%s %02d.%02d.%04d", clock_day_short(clock_weekday_index(info)),
                 info->tm_mday, info->tm_mon + 1, info->tm_year + 1900);
        lv_label_set_text(ctx->date_label, buffer);
    }
}

static void clock_refresh(w_clock_t *ctx)
{
    struct tm info = {0};
    time_t now = time(NULL);
    localtime_r(&now, &info);
    clock_update_labels(ctx, &info);
}

static void clock_timer_cb(lv_timer_t *timer)
{
    system_log_note_lvgl_cb("clock_timer_cb");
    w_clock_t *ctx = (w_clock_t *)lv_timer_get_user_data(timer);
    if (ctx == NULL) {
        return;
    }
    clock_refresh(ctx);
}

static void clock_card_delete_cb(lv_event_t *event)
{
    w_clock_t *ctx = (w_clock_t *)lv_event_get_user_data(event);
    if (ctx == NULL) {
        return;
    }
    if (ctx->timer != NULL) {
        lv_timer_del(ctx->timer);
        ctx->timer = NULL;
    }
    free(ctx);
}

/* Height of the visible rows, used to drop the optional rows when the tile is
 * too small for all of them. */
static int clock_rows_height(const w_clock_t *ctx)
{
    int height = 0;
    int rows = 0;
    if (ctx->title_label != NULL) {
        height += lv_obj_get_height(ctx->title_label);
        rows++;
    }
    height += lv_obj_get_height(ctx->time_label);
    rows++;
    if (ctx->date_label != NULL) {
        height += lv_obj_get_height(ctx->date_label);
        rows++;
    }
    return height + CLOCK_ROW_GAP * (rows - 1);
}

static void clock_layout_rows(w_clock_t *ctx)
{
    lv_obj_t *rows[CLOCK_ROWS_MAX];
    int count = 0;
    if (ctx->title_label != NULL) {
        rows[count++] = ctx->title_label;
    }
    rows[count++] = ctx->time_label;
    if (ctx->date_label != NULL) {
        rows[count++] = ctx->date_label;
    }

    for (int i = 0; i < count; i++) {
        if (i == 0) {
            lv_obj_align(rows[i], LV_ALIGN_TOP_MID, 0, 0);
        } else {
            lv_obj_align_to(rows[i], rows[i - 1], LV_ALIGN_OUT_BOTTOM_MID, 0, CLOCK_ROW_GAP);
        }
    }
}

esp_err_t w_clock_alarm_create(const ui_widget_def_t *def, lv_obj_t *parent, ui_widget_instance_t *out_instance)
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
    /* set_pos/set_size must come after remove_style_all() - see w_binary_sensor. */
    lv_obj_set_pos(card, def->x, def->y);
    lv_obj_set_size(card, def->w, def->h);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(card, lv_color_hex(APP_UI_COLOR_CARD_BG_OFF), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(card, APP_UI_CARD_RADIUS, LV_PART_MAIN);
    lv_obj_set_style_border_width(card, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(card, lv_color_hex(APP_UI_COLOR_CARD_BORDER), LV_PART_MAIN);
    lv_obj_set_style_pad_all(card, CLOCK_PAD, LV_PART_MAIN);

    w_clock_t *ctx = (w_clock_t *)ui_calloc_prefer_psram(1, sizeof(*ctx));
    if (ctx == NULL) {
        lv_obj_del(card);
        return ESP_ERR_NO_MEM;
    }
    ctx->card = card;
    ctx->width_px = def->w;
    snprintf(ctx->title, sizeof(ctx->title), "%s", def->title);
    ctx->show_seconds = def->clock_show_seconds;
    ctx->show_date = def->clock_show_date;

    /* Like the other tiles, hide the title while it still carries the
     * auto-generated id. */
    const bool title_is_auto_id = (def->title[0] != '\0' && strcmp(def->title, def->id) == 0);
    if (def->title[0] != '\0' && !title_is_auto_id) {
        ctx->title_label = lv_label_create(card);
        lv_obj_add_flag(ctx->title_label, LV_OBJ_FLAG_USER_1);
        clock_style_label(ctx->title_label, APP_FONT_TEXT_16, APP_UI_COLOR_TEXT_MUTED, LV_LABEL_LONG_CLIP);
        lv_label_set_text(ctx->title_label, def->title);
    }

    ctx->time_label = lv_label_create(card);
    lv_obj_add_flag(ctx->time_label, LV_OBJ_FLAG_USER_3);
    clock_style_label(ctx->time_label, clock_time_font(def->w, ctx->show_seconds), APP_UI_COLOR_TEXT_PRIMARY,
                      LV_LABEL_LONG_CLIP);
    lv_label_set_text(ctx->time_label, "00:00");

    if (ctx->show_date) {
        ctx->date_label = lv_label_create(card);
        lv_obj_add_flag(ctx->date_label, LV_OBJ_FLAG_USER_2);
        clock_style_label(ctx->date_label, APP_FONT_TEXT_16, APP_UI_COLOR_TEXT_MUTED, LV_LABEL_LONG_CLIP);
        lv_label_set_text(ctx->date_label, "");
    }

    clock_refresh(ctx);
    lv_obj_update_layout(card);

    /* Drop the optional rows if they would overflow the tile. */
    const int inner_h = def->h - (CLOCK_PAD * 2);
    if (ctx->date_label != NULL && clock_rows_height(ctx) > inner_h) {
        lv_obj_del(ctx->date_label);
        ctx->date_label = NULL;
    }
    if (ctx->title_label != NULL && clock_rows_height(ctx) > inner_h) {
        lv_obj_del(ctx->title_label);
        ctx->title_label = NULL;
    }
    clock_layout_rows(ctx);

    lv_obj_add_event_cb(card, clock_card_delete_cb, LV_EVENT_DELETE, ctx);
    ctx->timer = lv_timer_create(clock_timer_cb, 1000, ctx);

    out_instance->obj = card;
    out_instance->ctx = ctx;
    ESP_LOGI(TAG, "clock tile ready (seconds %s, date %s)", ctx->show_seconds ? "on" : "off",
             ctx->show_date ? "on" : "off");
    return ESP_OK;
}

void w_clock_alarm_apply_state(ui_widget_instance_t *instance, const ha_state_t *state)
{
    (void)instance;
    (void)state;
}

void w_clock_alarm_mark_unavailable(ui_widget_instance_t *instance)
{
    (void)instance;
}
