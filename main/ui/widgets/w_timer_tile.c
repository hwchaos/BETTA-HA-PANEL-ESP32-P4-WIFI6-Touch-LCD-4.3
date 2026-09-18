/* SPDX-License-Identifier: LicenseRef-FNCL-1.1
 * Copyright (c) 2026 Cpt_Kirk
 *
 * Timer tile - works with Home Assistant timer.* helpers and, when no entity is
 * configured, as a standalone countdown using the built-in 1 / 5 / 10 minute
 * presets. The value counts down locally between Home Assistant updates and the
 * button row follows the timer phase (start / pause / cancel). A standalone
 * timer has no sound on this panel, so the finished state is signalled by
 * blinking the tile until it is tapped.
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
#include "ui/theme/theme_default.h"
#include "ui/ui_i18n.h"
#include "ui/ui_memory.h"
#include "diag/system_log.h"

static const char *TAG = "w_timer_tile";

#define W_TIMER_BUTTON_COUNT 3
#define W_TIMER_MIN_BUTTON_H 150
#define W_TIMER_MIN_BUTTON_W 170
#define W_TIMER_BUSY_TIMEOUT_MS 6000
#define W_TIMER_BLINK_PERIOD_MS 500
#define W_TIMER_BLINK_STEPS 20
#define W_TIMER_ACCENT 0xFFB648U
#define W_TIMER_ACCENT_DONE 0x2ECC9AU

typedef enum {
    W_TIMER_BTN_NONE = 0,
    W_TIMER_BTN_START,
    W_TIMER_BTN_PAUSE,
    W_TIMER_BTN_RESUME,
    W_TIMER_BTN_CANCEL,
    W_TIMER_BTN_RESET,
    W_TIMER_BTN_MIN1,
    W_TIMER_BTN_MIN5,
    W_TIMER_BTN_MIN10,
} w_timer_btn_t;

typedef enum {
    W_TIMER_IDLE = 0,
    W_TIMER_ACTIVE,
    W_TIMER_PAUSED,
    W_TIMER_FINISHED,
} w_timer_phase_t;

typedef struct {
    char entity_id[APP_MAX_ENTITY_ID_LEN];
    lv_obj_t *card;
    lv_obj_t *title_label;
    lv_obj_t *value_label;
    lv_obj_t *status_label;
    lv_obj_t *buttons[W_TIMER_BUTTON_COUNT];
    lv_obj_t *button_labels[W_TIMER_BUTTON_COUNT];
    w_timer_btn_t roles[W_TIMER_BUTTON_COUNT];
    lv_timer_t *tick_timer;
    uint32_t accent;
    uint32_t busy_until_ms;
    uint32_t remaining_ms;
    uint32_t reference_tick;
    char value_text[16];
    char status_text[48];
    w_timer_phase_t phase;
    int blink_steps;
    bool have_entity;
    bool explicit_title;
    bool unavailable;
    bool busy;
    bool blink_on;
} w_timer_tile_t;

static void timer_layout(w_timer_tile_t *ctx);
static void timer_refresh_buttons(w_timer_tile_t *ctx);
static void timer_apply_visual(w_timer_tile_t *ctx);
static void timer_ensure_tick(w_timer_tile_t *ctx);

static const char *timer_i18n(const char *key, const char *fallback)
{
    const char *text = ui_i18n_get(key, NULL);
    return (text != NULL && text[0] != '\0') ? text : fallback;
}

static const lv_font_t *timer_font_px(int px)
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

static const lv_font_t *timer_fit_font(const char *text, int max_width, int max_height)
{
    static const int sizes[] = {34, 28, 24, 20, 18, 16, 14};
    const lv_font_t *result = timer_font_px(14);
    const char *label = (text != NULL) ? text : "";

    for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
        const lv_font_t *font = timer_font_px(sizes[i]);
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

/* Accepts the "H:MM:SS" / "MM:SS" / "SS" forms Home Assistant uses for timer
 * durations ("0:05:00", "0:04:33.500000"). */
static uint32_t timer_ms_from_hms(const char *text)
{
    if (text == NULL || text[0] == '\0') {
        return 0;
    }

    int parts[3] = {0, 0, 0};
    int count = 0;
    const char *cursor = text;

    while (*cursor != '\0' && count < 3) {
        while (*cursor == ' ') {
            cursor++;
        }
        if (*cursor < '0' || *cursor > '9') {
            break;
        }
        long value = 0;
        while (*cursor >= '0' && *cursor <= '9') {
            value = (value * 10) + (*cursor - '0');
            if (value > 359999) {
                value = 359999;
            }
            cursor++;
        }
        parts[count++] = (int)value;
        while (*cursor != '\0' && *cursor != ':') {
            cursor++;
        }
        if (*cursor == ':') {
            cursor++;
        } else {
            break;
        }
    }

    int hours = 0;
    int minutes = 0;
    int seconds = 0;
    if (count == 3) {
        hours = parts[0];
        minutes = parts[1];
        seconds = parts[2];
    } else if (count == 2) {
        minutes = parts[0];
        seconds = parts[1];
    } else if (count == 1) {
        seconds = parts[0];
    } else {
        return 0;
    }

    const uint32_t total_ms = ((uint32_t)hours * 3600U + (uint32_t)minutes * 60U + (uint32_t)seconds) * 1000U;
    return total_ms;
}

static void timer_format_ms(uint32_t ms, char *out, size_t out_len)
{
    if (out == NULL || out_len == 0) {
        return;
    }

    uint32_t total_sec = (ms + 999U) / 1000U;
    const uint32_t hours = total_sec / 3600U;
    total_sec %= 3600U;
    const uint32_t minutes = total_sec / 60U;
    const uint32_t seconds = total_sec % 60U;

    if (hours > 0) {
        snprintf(out, out_len, "%" PRIu32 ":%02" PRIu32 ":%02" PRIu32, hours, minutes, seconds);
    } else {
        snprintf(out, out_len, "%02" PRIu32 ":%02" PRIu32, minutes, seconds);
    }
}

static uint32_t timer_current_remaining_ms(const w_timer_tile_t *ctx)
{
    if (ctx->phase != W_TIMER_ACTIVE) {
        return ctx->remaining_ms;
    }

    const uint32_t elapsed = lv_tick_get() - ctx->reference_tick;
    if (elapsed >= ctx->remaining_ms) {
        return 0;
    }
    return ctx->remaining_ms - elapsed;
}

static void timer_arm_countdown(w_timer_tile_t *ctx, uint32_t remaining_ms)
{
    ctx->remaining_ms = remaining_ms;
    ctx->reference_tick = lv_tick_get();
}

static const char *timer_btn_label_key(w_timer_btn_t btn)
{
    switch (btn) {
    case W_TIMER_BTN_START:
        return "timer.btn_start";
    case W_TIMER_BTN_PAUSE:
        return "timer.btn_pause";
    case W_TIMER_BTN_RESUME:
        return "timer.btn_resume";
    case W_TIMER_BTN_CANCEL:
        return "timer.btn_cancel";
    case W_TIMER_BTN_RESET:
        return "timer.btn_reset";
    case W_TIMER_BTN_MIN1:
        return "timer.btn_min1";
    case W_TIMER_BTN_MIN5:
        return "timer.btn_min5";
    case W_TIMER_BTN_MIN10:
        return "timer.btn_min10";
    default:
        return "";
    }
}

static const char *timer_btn_label_fallback(w_timer_btn_t btn)
{
    switch (btn) {
    case W_TIMER_BTN_START:
        return "Start";
    case W_TIMER_BTN_PAUSE:
        return "Pause";
    case W_TIMER_BTN_RESUME:
        return "Resume";
    case W_TIMER_BTN_CANCEL:
        return "Cancel";
    case W_TIMER_BTN_RESET:
        return "Reset";
    case W_TIMER_BTN_MIN1:
        return "1 min";
    case W_TIMER_BTN_MIN5:
        return "5 min";
    case W_TIMER_BTN_MIN10:
        return "10 min";
    default:
        return "";
    }
}

static uint32_t timer_btn_accent(w_timer_btn_t btn)
{
    switch (btn) {
    case W_TIMER_BTN_CANCEL:
        return 0xFF5252;
    case W_TIMER_BTN_PAUSE:
        return 0x41BDF5;
    case W_TIMER_BTN_RESUME:
    case W_TIMER_BTN_START:
        return W_TIMER_ACCENT_DONE;
    case W_TIMER_BTN_RESET:
        return 0x7C6BFF;
    default:
        return W_TIMER_ACCENT;
    }
}

static void timer_style_button(lv_obj_t *button, uint32_t border, uint32_t text, lv_opa_t opa)
{
    if (button == NULL) {
        return;
    }
    lv_obj_set_style_bg_color(button, lv_color_hex(APP_UI_COLOR_NAV_BTN_BG_IDLE), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(button, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(button, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(button, lv_color_hex(border), LV_PART_MAIN);
    lv_obj_set_style_shadow_width(button, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(button, 10, LV_PART_MAIN);
    lv_obj_set_style_opa(button, opa, LV_PART_MAIN);
    lv_obj_set_style_text_color(button, lv_color_hex(text), LV_PART_MAIN);
}

static void timer_refresh_buttons(w_timer_tile_t *ctx)
{
    if (ctx == NULL) {
        return;
    }

    const bool disabled = ctx->unavailable || ctx->busy;
    w_timer_btn_t roles[W_TIMER_BUTTON_COUNT] = {W_TIMER_BTN_NONE, W_TIMER_BTN_NONE, W_TIMER_BTN_NONE};

    if (ctx->have_entity) {
        if (ctx->phase == W_TIMER_ACTIVE) {
            roles[0] = W_TIMER_BTN_PAUSE;
            roles[1] = W_TIMER_BTN_CANCEL;
        } else if (ctx->phase == W_TIMER_PAUSED) {
            roles[0] = W_TIMER_BTN_RESUME;
            roles[1] = W_TIMER_BTN_CANCEL;
        } else if (ctx->phase == W_TIMER_IDLE) {
            roles[0] = W_TIMER_BTN_START;
        }
    } else if (ctx->phase == W_TIMER_ACTIVE || ctx->phase == W_TIMER_PAUSED) {
        roles[0] = W_TIMER_BTN_CANCEL;
    } else if (ctx->phase == W_TIMER_FINISHED) {
        roles[0] = W_TIMER_BTN_RESET;
    } else {
        roles[0] = W_TIMER_BTN_MIN1;
        roles[1] = W_TIMER_BTN_MIN5;
        roles[2] = W_TIMER_BTN_MIN10;
    }

    for (int i = 0; i < W_TIMER_BUTTON_COUNT; i++) {
        ctx->roles[i] = roles[i];
        if (ctx->buttons[i] == NULL) {
            continue;
        }
        if (roles[i] == W_TIMER_BTN_NONE) {
            lv_obj_add_flag(ctx->buttons[i], LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        lv_obj_clear_flag(ctx->buttons[i], LV_OBJ_FLAG_HIDDEN);
        if (ctx->button_labels[i] != NULL) {
            lv_label_set_text(ctx->button_labels[i],
                timer_i18n(timer_btn_label_key(roles[i]), timer_btn_label_fallback(roles[i])));
        }
        const uint32_t accent = timer_btn_accent(roles[i]);
        timer_style_button(ctx->buttons[i],
            disabled ? APP_UI_COLOR_CARD_BORDER : accent,
            disabled ? APP_UI_COLOR_TEXT_MUTED : accent,
            disabled ? LV_OPA_50 : LV_OPA_COVER);
        if (ctx->button_labels[i] != NULL) {
            lv_obj_set_style_text_color(ctx->button_labels[i],
                lv_color_hex(disabled ? APP_UI_COLOR_TEXT_MUTED : accent), LV_PART_MAIN);
        }
    }
}

static void timer_apply_visual(w_timer_tile_t *ctx)
{
    if (ctx == NULL || ctx->card == NULL) {
        return;
    }

    uint32_t border = W_TIMER_ACCENT;
    uint32_t value_color = APP_UI_COLOR_TEXT_PRIMARY;
    const char *status = timer_i18n("timer.status_idle", "Idle");

    if (ctx->unavailable) {
        border = APP_UI_COLOR_CARD_BORDER;
        value_color = APP_UI_COLOR_TEXT_MUTED;
        status = timer_i18n("common.unavailable", "Unavailable");
    } else if (ctx->busy) {
        status = timer_i18n("timer.status_sending", "Sending...");
    } else if (ctx->phase == W_TIMER_ACTIVE) {
        border = W_TIMER_ACCENT;
        value_color = W_TIMER_ACCENT;
        status = timer_i18n("timer.status_active", "Running");
    } else if (ctx->phase == W_TIMER_PAUSED) {
        border = 0x41BDF5;
        value_color = 0x41BDF5;
        status = timer_i18n("timer.status_paused", "Paused");
    } else if (ctx->phase == W_TIMER_FINISHED) {
        border = ctx->blink_on ? W_TIMER_ACCENT_DONE : APP_UI_COLOR_CARD_BORDER;
        value_color = W_TIMER_ACCENT_DONE;
        status = timer_i18n("timer.status_done", "Done");
    }

    lv_obj_set_style_bg_color(ctx->card,
        lv_color_hex(ctx->unavailable ? APP_UI_COLOR_CARD_BG_OFF : APP_UI_COLOR_CARD_BG_ON), LV_PART_MAIN);
    lv_obj_set_style_border_color(ctx->card, lv_color_hex(border), LV_PART_MAIN);
    lv_obj_set_style_border_width(ctx->card,
        (ctx->phase == W_TIMER_ACTIVE || ctx->phase == W_TIMER_FINISHED) ? 3 : 1, LV_PART_MAIN);

    if (ctx->value_label != NULL) {
        lv_obj_set_style_text_color(ctx->value_label, lv_color_hex(value_color), LV_PART_MAIN);
    }
    if (ctx->status_label != NULL) {
        lv_obj_set_style_text_color(ctx->status_label, lv_color_hex(border), LV_PART_MAIN);
        strlcpy(ctx->status_text, status, sizeof(ctx->status_text));
        lv_label_set_text(ctx->status_label, ctx->status_text);
    }
}

static void timer_set_value(w_timer_tile_t *ctx, uint32_t remaining_ms)
{
    if (ctx == NULL || ctx->value_label == NULL) {
        return;
    }

    char text[16];
    timer_format_ms(remaining_ms, text, sizeof(text));
    if (strcmp(text, ctx->value_text) == 0) {
        return;
    }
    strlcpy(ctx->value_text, text, sizeof(ctx->value_text));
    lv_label_set_text(ctx->value_label, ctx->value_text);
    timer_layout(ctx);
}

static void timer_tick_cb(lv_timer_t *timer)
{
    system_log_note_lvgl_cb("timer_tick_cb");
    w_timer_tile_t *ctx = (timer != NULL) ? (w_timer_tile_t *)lv_timer_get_user_data(timer) : NULL;
    if (ctx == NULL) {
        return;
    }

    if (ctx->busy && (int32_t)(lv_tick_get() - ctx->busy_until_ms) >= 0) {
        ctx->busy = false;
        timer_refresh_buttons(ctx);
        timer_apply_visual(ctx);
    }

    if (ctx->phase == W_TIMER_FINISHED) {
        if (ctx->blink_steps > 0) {
            ctx->blink_steps--;
            ctx->blink_on = !ctx->blink_on;
            timer_apply_visual(ctx);
        } else {
            ctx->blink_on = true;
            timer_apply_visual(ctx);
            ctx->tick_timer = NULL;
            lv_timer_del(timer);
        }
        return;
    }

    if (ctx->phase == W_TIMER_ACTIVE) {
        const uint32_t remaining = timer_current_remaining_ms(ctx);
        timer_set_value(ctx, remaining);
        if (remaining == 0) {
            if (!ctx->have_entity) {
                ctx->phase = W_TIMER_FINISHED;
                ctx->blink_steps = W_TIMER_BLINK_STEPS;
                ctx->blink_on = true;
                lv_timer_set_period(timer, W_TIMER_BLINK_PERIOD_MS);
                timer_set_value(ctx, 0);
                timer_refresh_buttons(ctx);
                timer_apply_visual(ctx);
                return;
            }
            /* Home Assistant owns the real deadline: wait for its state update. */
            ctx->tick_timer = NULL;
            lv_timer_del(timer);
            return;
        }
    }

    if (ctx->phase != W_TIMER_ACTIVE && !ctx->busy) {
        ctx->tick_timer = NULL;
        lv_timer_del(timer);
    }
}

static void timer_ensure_tick(w_timer_tile_t *ctx)
{
    if (ctx->tick_timer == NULL) {
        ctx->tick_timer = lv_timer_create(timer_tick_cb, 1000, ctx);
    }
    if (ctx->tick_timer != NULL) {
        lv_timer_reset(ctx->tick_timer);
        lv_timer_resume(ctx->tick_timer);
    }
}

static void timer_start_local(w_timer_tile_t *ctx, uint32_t minutes)
{
    ctx->phase = W_TIMER_ACTIVE;
    timer_arm_countdown(ctx, minutes * 60000U);
    timer_set_value(ctx, ctx->remaining_ms);
    timer_refresh_buttons(ctx);
    timer_apply_visual(ctx);
    timer_ensure_tick(ctx);
}

static void timer_send(w_timer_tile_t *ctx, const char *service)
{
    if (ctx == NULL || !ctx->have_entity || service == NULL) {
        return;
    }

    char payload[APP_MAX_ENTITY_ID_LEN + 32];
    snprintf(payload, sizeof(payload), "{\"entity_id\":\"%s\"}", ctx->entity_id);

    const esp_err_t err = ha_client_call_service("timer", service, payload);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "timer.%s failed for %s: %s", service, ctx->entity_id, esp_err_to_name(err));
        ctx->busy = false;
        if (ctx->status_label != NULL) {
            lv_label_set_text(ctx->status_label, timer_i18n("timer.status_failed", "Command failed"));
            lv_obj_set_style_text_color(ctx->status_label, lv_color_hex(APP_UI_COLOR_ERROR), LV_PART_MAIN);
        }
        timer_refresh_buttons(ctx);
        return;
    }

    ctx->busy = true;
    ctx->busy_until_ms = lv_tick_get() + W_TIMER_BUSY_TIMEOUT_MS;
    timer_refresh_buttons(ctx);
    timer_apply_visual(ctx);
    timer_ensure_tick(ctx);
}

static void timer_button_action(w_timer_tile_t *ctx, w_timer_btn_t role)
{
    if (ctx == NULL || role == W_TIMER_BTN_NONE) {
        return;
    }

    switch (role) {
    case W_TIMER_BTN_START:
        timer_send(ctx, "start");
        break;
    case W_TIMER_BTN_RESUME:
        if (ctx->have_entity) {
            timer_send(ctx, "start");
        } else {
            timer_arm_countdown(ctx, ctx->remaining_ms);
            ctx->phase = W_TIMER_ACTIVE;
            timer_refresh_buttons(ctx);
            timer_apply_visual(ctx);
            timer_ensure_tick(ctx);
        }
        break;
    case W_TIMER_BTN_PAUSE:
        if (ctx->have_entity) {
            timer_send(ctx, "pause");
        } else {
            const uint32_t remaining = timer_current_remaining_ms(ctx);
            ctx->remaining_ms = remaining;
            ctx->phase = W_TIMER_PAUSED;
            timer_refresh_buttons(ctx);
            timer_apply_visual(ctx);
        }
        break;
    case W_TIMER_BTN_CANCEL:
        if (ctx->have_entity) {
            timer_send(ctx, "cancel");
        } else {
            ctx->phase = W_TIMER_IDLE;
            ctx->remaining_ms = 0;
            ctx->blink_steps = 0;
            timer_set_value(ctx, 0);
            timer_refresh_buttons(ctx);
            timer_apply_visual(ctx);
        }
        break;
    case W_TIMER_BTN_RESET:
        ctx->phase = W_TIMER_IDLE;
        ctx->remaining_ms = 0;
        ctx->blink_steps = 0;
        timer_set_value(ctx, 0);
        timer_refresh_buttons(ctx);
        timer_apply_visual(ctx);
        break;
    case W_TIMER_BTN_MIN1:
        timer_start_local(ctx, 1);
        break;
    case W_TIMER_BTN_MIN5:
        timer_start_local(ctx, 5);
        break;
    case W_TIMER_BTN_MIN10:
        timer_start_local(ctx, 10);
        break;
    default:
        break;
    }
}

static void timer_card_tap(w_timer_tile_t *ctx)
{
    if (ctx == NULL || ctx->unavailable) {
        return;
    }

    if (ctx->have_entity) {
        if (ctx->phase == W_TIMER_ACTIVE) {
            timer_button_action(ctx, W_TIMER_BTN_PAUSE);
        } else if (ctx->phase == W_TIMER_PAUSED) {
            timer_button_action(ctx, W_TIMER_BTN_RESUME);
        } else {
            timer_button_action(ctx, W_TIMER_BTN_START);
        }
        return;
    }

    if (ctx->phase == W_TIMER_ACTIVE) {
        timer_button_action(ctx, W_TIMER_BTN_PAUSE);
    } else if (ctx->phase == W_TIMER_PAUSED) {
        timer_button_action(ctx, W_TIMER_BTN_RESUME);
    } else if (ctx->phase == W_TIMER_FINISHED) {
        timer_button_action(ctx, W_TIMER_BTN_RESET);
    } else {
        timer_start_local(ctx, 5);
    }
}

static void timer_button_event_cb(lv_event_t *event)
{
    if (event == NULL) {
        return;
    }

    lv_obj_t *button = lv_event_get_target(event);
    w_timer_tile_t *ctx = (w_timer_tile_t *)lv_event_get_user_data(event);
    if (button == NULL || ctx == NULL) {
        return;
    }

    const lv_event_code_t code = lv_event_get_code(event);
    if (code != LV_EVENT_CLICKED) {
        return;
    }

    const intptr_t index = (intptr_t)lv_obj_get_user_data(button) - 1;
    if (index < 0 || index >= W_TIMER_BUTTON_COUNT) {
        return;
    }
    timer_button_action(ctx, ctx->roles[index]);
}

static void timer_card_event_cb(lv_event_t *event)
{
    if (event == NULL) {
        return;
    }

    w_timer_tile_t *ctx = (w_timer_tile_t *)lv_event_get_user_data(event);
    if (ctx == NULL) {
        return;
    }

    const lv_event_code_t code = lv_event_get_code(event);
    if (code == LV_EVENT_SIZE_CHANGED) {
        timer_layout(ctx);
    } else if (code == LV_EVENT_CLICKED) {
        timer_card_tap(ctx);
    } else if (code == LV_EVENT_DELETE) {
        if (ctx->tick_timer != NULL) {
            lv_timer_del(ctx->tick_timer);
            ctx->tick_timer = NULL;
        }
        free(ctx);
    }
}

static void timer_layout(w_timer_tile_t *ctx)
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

    const lv_coord_t inner_w = card_w - (2 * pad);
    const lv_coord_t inner_h = card_h - (2 * pad);

    lv_coord_t title_h = 0;
    if (ctx->title_label != NULL) {
        title_h = (card_h >= 150) ? 20 : 18;
        lv_obj_set_style_text_font(ctx->title_label, timer_font_px(title_h >= 20 ? 16 : 14), LV_PART_MAIN);
        lv_obj_set_width(ctx->title_label, inner_w);
        lv_obj_set_style_text_align(ctx->title_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
        lv_obj_align(ctx->title_label, LV_ALIGN_TOP_MID, 0, 0);
    }

    bool buttons_fit = (card_h >= W_TIMER_MIN_BUTTON_H && card_w >= W_TIMER_MIN_BUTTON_W);
    int visible_buttons = 0;
    for (int i = 0; i < W_TIMER_BUTTON_COUNT; i++) {
        if (ctx->roles[i] != W_TIMER_BTN_NONE) {
            visible_buttons++;
        }
    }
    if (visible_buttons == 0) {
        buttons_fit = false;
    }

    const lv_coord_t row_h = buttons_fit ? LV_MAX((lv_coord_t)32, LV_MIN((lv_coord_t)48, card_h / 5)) : 0;
    const lv_coord_t row_gap = 6;

    if (buttons_fit) {
        const lv_coord_t slot_w = (inner_w - ((visible_buttons - 1) * row_gap)) / visible_buttons;
        lv_coord_t x = 0;
        for (int i = 0; i < W_TIMER_BUTTON_COUNT; i++) {
            if (ctx->buttons[i] == NULL || ctx->roles[i] == W_TIMER_BTN_NONE) {
                continue;
            }
            lv_obj_set_size(ctx->buttons[i], slot_w, row_h);
            lv_obj_align(ctx->buttons[i], LV_ALIGN_BOTTOM_LEFT, x, 0);
            x += slot_w + row_gap;
        }
    } else {
        for (int i = 0; i < W_TIMER_BUTTON_COUNT; i++) {
            if (ctx->buttons[i] != NULL && ctx->roles[i] != W_TIMER_BTN_NONE) {
                lv_obj_add_flag(ctx->buttons[i], LV_OBJ_FLAG_HIDDEN);
            }
        }
    }

    if (ctx->value_label == NULL) {
        return;
    }

    const lv_coord_t band_top = title_h;
    const lv_coord_t band_h = inner_h - title_h - row_h - (buttons_fit ? row_gap : 0);
    const lv_font_t *value_font = timer_fit_font(ctx->value_text, inner_w, LV_MAX((lv_coord_t)18, (band_h * 3) / 4));
    lv_obj_set_style_text_font(ctx->value_label, value_font, LV_PART_MAIN);
    lv_obj_update_layout(ctx->value_label);

    const lv_coord_t value_h = lv_obj_get_height(ctx->value_label);
    const lv_coord_t status_h = (ctx->status_label != NULL && band_h >= 40) ? 18 : 0;

    if (ctx->status_label != NULL) {
        if (status_h > 0) {
            lv_obj_clear_flag(ctx->status_label, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(ctx->status_label, LV_OBJ_FLAG_HIDDEN);
        }
    }

    const lv_coord_t stack_h = value_h + status_h + (status_h > 0 ? 2 : 0);
    const lv_coord_t value_y = band_top + LV_MAX((lv_coord_t)0, (band_h - stack_h) / 2);
    lv_obj_align(ctx->value_label, LV_ALIGN_TOP_MID, 0, value_y);

    if (status_h > 0) {
        lv_obj_set_style_text_font(ctx->status_label, timer_font_px(14), LV_PART_MAIN);
        lv_obj_set_width(ctx->status_label, inner_w);
        lv_obj_set_style_text_align(ctx->status_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
        lv_obj_align_to(ctx->status_label, ctx->value_label, LV_ALIGN_OUT_BOTTOM_MID, 0, 2);
    }
}

esp_err_t w_timer_tile_create(const ui_widget_def_t *def, lv_obj_t *parent, ui_widget_instance_t *out_instance)
{
    if (def == NULL || parent == NULL || out_instance == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    w_timer_tile_t *ctx = (w_timer_tile_t *)ui_calloc_prefer_psram(1, sizeof(w_timer_tile_t));
    if (ctx == NULL) {
        ESP_LOGE(TAG, "out of memory for timer tile");
        return ESP_ERR_NO_MEM;
    }

    ctx->accent = W_TIMER_ACCENT;
    ctx->phase = W_TIMER_IDLE;
    ctx->blink_on = true;
    ctx->have_entity = (def->entity_id[0] != '\0');
    if (ctx->have_entity) {
        strlcpy(ctx->entity_id, def->entity_id, sizeof(ctx->entity_id));
    }
    strlcpy(ctx->status_text, timer_i18n("timer.status_idle", "Idle"), sizeof(ctx->status_text));

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

    lv_obj_t *title = lv_label_create(card);
    lv_obj_add_flag(title, LV_OBJ_FLAG_USER_1);
    ctx->title_label = title;
    lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_color(title, lv_color_hex(APP_UI_COLOR_TEXT_SOFT), LV_PART_MAIN);
    lv_obj_set_style_text_font(title, timer_font_px(14), LV_PART_MAIN);
    lv_obj_set_width(title, lv_pct(100));
    const bool title_is_auto_id = (def->title[0] == '\0') || (strcmp(def->title, def->id) == 0);
    if (title_is_auto_id) {
        lv_obj_add_flag(title, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(title, "");
    } else {
        lv_label_set_text(title, def->title);
    }
    ctx->explicit_title = !title_is_auto_id;

    lv_obj_t *value = lv_label_create(card);
    lv_obj_add_flag(value, LV_OBJ_FLAG_USER_3);
    ctx->value_label = value;
    lv_label_set_long_mode(value, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_align(value, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_style_text_color(value, lv_color_hex(APP_UI_COLOR_TEXT_PRIMARY), LV_PART_MAIN);
    lv_obj_set_style_text_font(value, timer_font_px(28), LV_PART_MAIN);
    strlcpy(ctx->value_text, "00:00", sizeof(ctx->value_text));
    lv_label_set_text(value, ctx->value_text);

    lv_obj_t *status = lv_label_create(card);
    lv_obj_add_flag(status, LV_OBJ_FLAG_USER_2);
    ctx->status_label = status;
    lv_label_set_long_mode(status, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(status, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_style_text_color(status, lv_color_hex(APP_UI_COLOR_TEXT_MUTED), LV_PART_MAIN);
    lv_obj_set_style_text_font(status, timer_font_px(14), LV_PART_MAIN);
    lv_obj_set_width(status, lv_pct(100));
    lv_label_set_text(status, ctx->status_text);

    for (int i = 0; i < W_TIMER_BUTTON_COUNT; i++) {
        lv_obj_t *button = lv_btn_create(card);
        if (button == NULL) {
            continue;
        }
        ctx->buttons[i] = button;
        lv_obj_clear_flag(button, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_style_shadow_width(button, 0, LV_PART_MAIN);
        lv_obj_set_user_data(button, (void *)(intptr_t)(i + 1));
        lv_obj_add_event_cb(button, timer_button_event_cb, LV_EVENT_CLICKED, ctx);

        lv_obj_t *label = lv_label_create(button);
        ctx->button_labels[i] = label;
        lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
        lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
        lv_obj_set_style_text_font(label, timer_font_px(14), LV_PART_MAIN);
        lv_obj_center(label);
        lv_obj_add_flag(button, LV_OBJ_FLAG_HIDDEN);
    }

    lv_obj_add_event_cb(card, timer_card_event_cb, LV_EVENT_SIZE_CHANGED, ctx);
    lv_obj_add_event_cb(card, timer_card_event_cb, LV_EVENT_CLICKED, ctx);
    lv_obj_add_event_cb(card, timer_card_event_cb, LV_EVENT_DELETE, ctx);

    timer_refresh_buttons(ctx);
    timer_apply_visual(ctx);
    timer_layout(ctx);

    out_instance->obj = card;
    out_instance->ctx = ctx;
    return ESP_OK;
}

void w_timer_tile_apply_state(ui_widget_instance_t *instance, const ha_state_t *state)
{
    if (instance == NULL || instance->obj == NULL || state == NULL) {
        return;
    }
    w_timer_tile_t *ctx = (w_timer_tile_t *)instance->ctx;
    if (ctx == NULL || !ctx->have_entity) {
        return;
    }

    ctx->busy = false;

    const bool offline = (state->state[0] == '\0') || (strcmp(state->state, "unavailable") == 0);
    if (offline) {
        ctx->unavailable = true;
        ctx->phase = W_TIMER_IDLE;
        timer_refresh_buttons(ctx);
        timer_apply_visual(ctx);
        return;
    }

    ctx->unavailable = false;

    /* The tile counts down on its own between updates, so the remaining time is
     * re-based on every state message. */
    uint32_t remaining_ms = 0;
    uint32_t duration_ms = 0;
    if (state->attributes_json[0] != '\0') {
        cJSON *attrs = cJSON_Parse(state->attributes_json);
        if (attrs != NULL) {
            cJSON *remaining = cJSON_GetObjectItemCaseSensitive(attrs, "remaining");
            if (cJSON_IsString(remaining) && remaining->valuestring != NULL) {
                remaining_ms = timer_ms_from_hms(remaining->valuestring);
            }
            cJSON *duration = cJSON_GetObjectItemCaseSensitive(attrs, "duration");
            if (cJSON_IsString(duration) && duration->valuestring != NULL) {
                duration_ms = timer_ms_from_hms(duration->valuestring);
            }
            cJSON_Delete(attrs);
        }
    }

    if (strcmp(state->state, "active") == 0) {
        ctx->phase = W_TIMER_ACTIVE;
        timer_arm_countdown(ctx, remaining_ms);
        timer_set_value(ctx, remaining_ms);
        timer_ensure_tick(ctx);
    } else if (strcmp(state->state, "paused") == 0) {
        ctx->phase = W_TIMER_PAUSED;
        ctx->remaining_ms = remaining_ms;
        timer_set_value(ctx, remaining_ms);
    } else {
        ctx->phase = W_TIMER_IDLE;
        ctx->remaining_ms = 0;
        timer_set_value(ctx, duration_ms);
    }

    if (!ctx->explicit_title && state->attributes_json[0] != '\0' && ctx->title_label != NULL) {
        cJSON *attrs = cJSON_Parse(state->attributes_json);
        if (attrs != NULL) {
            cJSON *friendly = cJSON_GetObjectItemCaseSensitive(attrs, "friendly_name");
            if (cJSON_IsString(friendly) && friendly->valuestring != NULL && friendly->valuestring[0] != '\0') {
                lv_obj_clear_flag(ctx->title_label, LV_OBJ_FLAG_HIDDEN);
                lv_label_set_text(ctx->title_label, friendly->valuestring);
            }
            cJSON_Delete(attrs);
        }
    }

    timer_refresh_buttons(ctx);
    timer_apply_visual(ctx);
    timer_layout(ctx);
}

void w_timer_tile_mark_unavailable(ui_widget_instance_t *instance)
{
    if (instance == NULL || instance->obj == NULL) {
        return;
    }
    w_timer_tile_t *ctx = (w_timer_tile_t *)instance->ctx;
    if (ctx == NULL || !ctx->have_entity) {
        /* A standalone timer keeps running while Home Assistant is away. */
        return;
    }

    ctx->unavailable = true;
    ctx->busy = false;
    ctx->phase = W_TIMER_IDLE;
    timer_refresh_buttons(ctx);
    timer_apply_visual(ctx);
}
