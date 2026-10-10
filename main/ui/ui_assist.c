#include "ui/ui_assist.h"

#include <stdio.h>
#include <string.h>

#include "app_config.h"
#include "assist/panel_assist.h"
#include "esp_timer.h"
#include "ui/fonts/app_text_fonts.h"
#include "ui/theme/theme_default.h"
#include "ui/ui_i18n.h"
#include "ui/ui_mdi_icon.h"
#include "ui/ui_screen_saver.h"
#include "ui/ui_geometry.h"

/* A press shorter than this is a tap: listening then continues until Home
 * Assistant detects the end of the sentence. Longer = push-to-talk. */
#define ASSIST_TAP_MS 600
#define ASSIST_BUBBLE_MIN_MS 6000
#define ASSIST_BUBBLE_MS_PER_CHAR 70
#define ASSIST_BUBBLE_GAP 10

static lv_obj_t *s_chip;
static lv_obj_t *s_bubble;
static lv_obj_t *s_bubble_status;
static lv_obj_t *s_bubble_text;
static lv_timer_t *s_timer;
static uint32_t s_seen_seq;
static int64_t s_press_ms;
static int64_t s_hide_at_ms;
static panel_assist_state_t s_shown_state;

static int64_t assist_now_ms(void)
{
    return esp_timer_get_time() / 1000;
}

static void assist_bubble_hide(void)
{
    if (s_bubble != NULL) {
        lv_obj_add_flag(s_bubble, LV_OBJ_FLAG_HIDDEN);
    }
    s_hide_at_ms = 0;
}

static void assist_bubble_click_cb(lv_event_t *event)
{
    LV_UNUSED(event);
    panel_assist_cancel();
    assist_bubble_hide();
}

static void assist_bubble_create(void)
{
    if (s_bubble != NULL) {
        return;
    }
    /* Above the bottom bar, or centred over the content box with a left bar. */
    const lv_coord_t bottom = APP_SCREEN_HEIGHT - (APP_CONTENT_BOX_Y + APP_CONTENT_BOX_HEIGHT);
    const lv_coord_t shift_x = ui_geometry_nav_left() ? (APP_CONTENT_BOX_X / 2) : 0;

    s_bubble = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s_bubble);
    lv_obj_set_width(s_bubble, (APP_CONTENT_BOX_WIDTH * 4) / 5);
    lv_obj_set_height(s_bubble, LV_SIZE_CONTENT);
    lv_obj_align(s_bubble, LV_ALIGN_BOTTOM_MID, shift_x, -(bottom + ASSIST_BUBBLE_GAP));
    lv_obj_set_style_bg_color(s_bubble, lv_color_hex(APP_UI_COLOR_CARD_BG_OFF), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_bubble, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_bubble, 2, LV_PART_MAIN);
    lv_obj_set_style_border_color(s_bubble, lv_color_hex(APP_UI_COLOR_STATE_ON), LV_PART_MAIN);
    lv_obj_set_style_radius(s_bubble, 18, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(s_bubble, 18, LV_PART_MAIN);
    lv_obj_set_style_pad_ver(s_bubble, 12, LV_PART_MAIN);
    lv_obj_set_style_pad_row(s_bubble, 4, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(s_bubble, 24, LV_PART_MAIN);
    lv_obj_set_style_shadow_opa(s_bubble, LV_OPA_50, LV_PART_MAIN);
    lv_obj_set_style_shadow_color(s_bubble, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_flex_flow(s_bubble, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(s_bubble, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_bubble, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_bubble, assist_bubble_click_cb, LV_EVENT_CLICKED, NULL);

    s_bubble_status = lv_label_create(s_bubble);
    lv_obj_set_width(s_bubble_status, LV_PCT(100));
    lv_obj_set_style_text_font(s_bubble_status, APP_FONT_TEXT_16, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_bubble_status, lv_color_hex(APP_UI_COLOR_TEXT_MUTED), LV_PART_MAIN);
    lv_label_set_long_mode(s_bubble_status, LV_LABEL_LONG_DOT);

    s_bubble_text = lv_label_create(s_bubble);
    lv_obj_set_width(s_bubble_text, LV_PCT(100));
    lv_obj_set_style_text_font(s_bubble_text, APP_FONT_TEXT_20, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_bubble_text, lv_color_hex(APP_UI_COLOR_TEXT_PRIMARY), LV_PART_MAIN);
    lv_label_set_long_mode(s_bubble_text, LV_LABEL_LONG_WRAP);

    lv_obj_add_flag(s_bubble, LV_OBJ_FLAG_HIDDEN);
}

static const char *assist_error_text(const char *code)
{
    if (strcmp(code, "ha-offline") == 0) {
        return ui_i18n_get("assist.err_offline", "Home Assistant is not connected");
    }
    if (strcmp(code, "mic") == 0) {
        return ui_i18n_get("assist.err_mic", "Microphone not available");
    }
    if (strcmp(code, "stt-no-text-recognized") == 0 || strcmp(code, "stt-stream-failed") == 0) {
        return ui_i18n_get("assist.err_no_speech", "Sorry, I did not understand that");
    }
    if (strcmp(code, "timeout") == 0) {
        return ui_i18n_get("assist.err_timeout", "No answer from Home Assistant");
    }
    if (strcmp(code, "pipeline-not-found") == 0 || strcmp(code, "start-failed") == 0 ||
        strcmp(code, "no-pipeline") == 0) {
        return ui_i18n_get("assist.err_pipeline", "Assist pipeline not available");
    }
    return ui_i18n_get("assist.err_generic", "That did not work");
}

static void assist_chip_tint(panel_assist_state_t state)
{
    if (s_chip == NULL) {
        return;
    }
    uint32_t bg = APP_UI_COLOR_TOPBAR_CHIP_BG;
    uint32_t fg = APP_UI_COLOR_TOPBAR_MUTED;
    if (state == PANEL_ASSIST_LISTENING) {
        bg = APP_UI_COLOR_ERROR;
        fg = 0xFFFFFF;
    } else if (state == PANEL_ASSIST_PROCESSING) {
        bg = APP_UI_COLOR_STATE_ON;
        fg = 0xFFFFFF;
    }
    lv_obj_set_style_bg_color(s_chip, lv_color_hex(bg), LV_PART_MAIN);
    lv_obj_set_style_text_color(s_chip, lv_color_hex(fg), LV_PART_MAIN);
}

static void assist_refresh(void)
{
    panel_assist_status_t st;
    panel_assist_get_status(&st);
    if (st.seq == s_seen_seq && st.state == s_shown_state) {
        return;
    }
    s_seen_seq = st.seq;
    s_shown_state = st.state;
    assist_chip_tint(st.state);

    if (st.state == PANEL_ASSIST_IDLE) {
        assist_bubble_hide();
        return;
    }
    assist_bubble_create();
    lv_obj_clear_flag(s_bubble, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_bubble);
    s_hide_at_ms = 0;

    uint32_t border = APP_UI_COLOR_STATE_ON;
    const char *status = "";
    const char *text = "";
    char quoted[192];
    switch (st.state) {
    case PANEL_ASSIST_LISTENING:
        border = APP_UI_COLOR_ERROR;
        status = ui_i18n_get("assist.listening", "Listening...");
        text = "";
        break;
    case PANEL_ASSIST_PROCESSING:
        status = ui_i18n_get("assist.processing", "One moment...");
        if (st.heard[0] != '\0') {
            snprintf(quoted, sizeof(quoted), "\"%s\"", st.heard);
            text = quoted;
        }
        break;
    case PANEL_ASSIST_DONE:
        if (st.heard[0] != '\0') {
            snprintf(quoted, sizeof(quoted), "\"%s\"", st.heard);
            status = quoted;
        }
        text = (st.reply[0] != '\0') ? st.reply : ui_i18n_get("assist.done", "Done");
        s_hide_at_ms = assist_now_ms() + ASSIST_BUBBLE_MIN_MS + (int64_t)strlen(text) * ASSIST_BUBBLE_MS_PER_CHAR;
        break;
    case PANEL_ASSIST_ERROR:
    default:
        border = APP_UI_COLOR_ERROR;
        if (st.heard[0] != '\0') {
            snprintf(quoted, sizeof(quoted), "\"%s\"", st.heard);
            status = quoted;
        }
        text = assist_error_text(st.error);
        s_hide_at_ms = assist_now_ms() + ASSIST_BUBBLE_MIN_MS;
        break;
    }
    lv_obj_set_style_border_color(s_bubble, lv_color_hex(border), LV_PART_MAIN);
    lv_label_set_text(s_bubble_status, status);
    lv_label_set_text(s_bubble_text, text);
    if (text[0] == '\0') {
        lv_obj_add_flag(s_bubble_text, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_clear_flag(s_bubble_text, LV_OBJ_FLAG_HIDDEN);
    }
}

static void assist_timer_cb(lv_timer_t *timer)
{
    LV_UNUSED(timer);
    assist_refresh();
    if (s_hide_at_ms != 0 && assist_now_ms() >= s_hide_at_ms && !panel_assist_is_busy()) {
        assist_bubble_hide();
    }
}

static void assist_chip_event_cb(lv_event_t *event)
{
    const lv_event_code_t code = lv_event_get_code(event);
    if (code == LV_EVENT_PRESSED) {
        ui_screen_saver_wake();
        panel_assist_status_t st;
        panel_assist_get_status(&st);
        if (panel_assist_is_busy()) {
            /* A tap while listening ends the sentence, otherwise it aborts. */
            if (st.state == PANEL_ASSIST_LISTENING) {
                panel_assist_stop_listening();
            } else {
                panel_assist_cancel();
            }
            s_press_ms = 0;
            return;
        }
        if (panel_assist_start() == ESP_OK) {
            s_press_ms = assist_now_ms();
        }
    } else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        if (s_press_ms != 0 && assist_now_ms() - s_press_ms >= ASSIST_TAP_MS) {
            panel_assist_stop_listening();
        }
        s_press_ms = 0;
    }
}

void ui_assist_attach_chip(lv_obj_t *chip)
{
    s_chip = chip;
    if (chip == NULL) {
        return;
    }
    if (!ui_mdi_icon_apply(chip, "microphone", 24)) {
        lv_label_set_text(chip, LV_SYMBOL_AUDIO);
        lv_obj_set_style_text_font(chip, &lv_font_montserrat_20, LV_PART_MAIN);
    }
    lv_obj_add_event_cb(chip, assist_chip_event_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(chip, assist_chip_event_cb, LV_EVENT_RELEASED, NULL);
    lv_obj_add_event_cb(chip, assist_chip_event_cb, LV_EVENT_PRESS_LOST, NULL);
    s_shown_state = PANEL_ASSIST_IDLE;
    /* The bubble lives on the top layer and survives page rebuilds. */
    if (s_timer == NULL) {
        s_timer = lv_timer_create(assist_timer_cb, 100, NULL);
    }
}
