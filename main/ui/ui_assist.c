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
#include "drivers/display_init.h"
#include "ui/ui_geometry.h"

/* A press shorter than this is a tap: listening then continues until Home
 * Assistant detects the end of the sentence. Longer = push-to-talk. */
#define ASSIST_TAP_MS 600
#define ASSIST_SHOW_MIN_MS 6000
#define ASSIST_SHOW_MS_PER_CHAR 70
#define ASSIST_PULSE_MS 1400
/* Fixed colours: the overlay is always dark, whatever the theme. */
#define ASSIST_COLOR_LISTEN 0x3B82F6
#define ASSIST_COLOR_WORK 0x334155
#define ASSIST_COLOR_DONE 0x22C55E
#define ASSIST_COLOR_ERROR 0xEF4444
#define ASSIST_COLOR_TEXT 0xF1F5F9
#define ASSIST_COLOR_MUTED 0x94A3B8

/* Full-screen voice overlay: a dimmed backdrop with a microphone disc in the
 * middle (pulsing while listening, a spinner while Home Assistant works),
 * the recognised sentence and the answer below. A tap ends the recording,
 * aborts the request or closes the answer. */
static lv_obj_t *s_chip;
static lv_obj_t *s_overlay;
static lv_obj_t *s_ring;
static lv_obj_t *s_disc;
static lv_obj_t *s_icon;
static lv_obj_t *s_spinner;
static lv_obj_t *s_status;
static lv_obj_t *s_text;
static lv_obj_t *s_hint;
static lv_timer_t *s_timer;
static uint32_t s_seen_seq;
static int64_t s_press_ms;
static int64_t s_hide_at_ms;
static panel_assist_state_t s_shown_state;
static int32_t s_orb;

static int64_t assist_now_ms(void)
{
    return esp_timer_get_time() / 1000;
}

static void assist_pulse_cb(void *obj, int32_t v)
{
    const int32_t size = s_orb + (s_orb * 6 / 10) * v / 1000;
    lv_obj_set_size(obj, size, size);
    lv_obj_set_style_bg_opa(obj, (lv_opa_t)(120 * (1000 - v) / 1000), LV_PART_MAIN);
}

static void assist_pulse(bool on)
{
    if (s_ring == NULL) {
        return;
    }
    lv_anim_delete(s_ring, assist_pulse_cb);
    if (!on) {
        lv_obj_add_flag(s_ring, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_obj_remove_flag(s_ring, LV_OBJ_FLAG_HIDDEN);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_ring);
    lv_anim_set_exec_cb(&a, assist_pulse_cb);
    lv_anim_set_values(&a, 0, 1000);
    lv_anim_set_duration(&a, ASSIST_PULSE_MS);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_anim_start(&a);
}

static void assist_overlay_hide(void)
{
    if (s_overlay != NULL) {
        assist_pulse(false);
        lv_obj_add_flag(s_overlay, LV_OBJ_FLAG_HIDDEN);
    }
    s_hide_at_ms = 0;
}

static void assist_overlay_click_cb(lv_event_t *event)
{
    LV_UNUSED(event);
    panel_assist_status_t st;
    panel_assist_get_status(&st);
    if (panel_assist_is_busy() && st.state == PANEL_ASSIST_LISTENING) {
        panel_assist_stop_listening(); /* done talking */
        return;
    }
    panel_assist_cancel();
    assist_overlay_hide();
}

static lv_obj_t *assist_label(lv_obj_t *parent, const lv_font_t *font, uint32_t color)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_obj_set_width(label, LV_PCT(84));
    lv_obj_set_style_text_font(label, font, LV_PART_MAIN);
    lv_obj_set_style_text_color(label, lv_color_hex(color), LV_PART_MAIN);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    return label;
}

static void assist_overlay_create(void)
{
    if (s_overlay != NULL) {
        return;
    }
    s_orb = LV_MIN(APP_SCREEN_WIDTH, APP_SCREEN_HEIGHT) / 4;

    s_overlay = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s_overlay);
    lv_obj_set_size(s_overlay, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(s_overlay, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_overlay, LV_OPA_90, LV_PART_MAIN);
    lv_obj_set_flex_flow(s_overlay, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_overlay, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(s_overlay, s_orb / 8, LV_PART_MAIN);
    lv_obj_set_scrollable(s_overlay, false);
    lv_obj_add_flag(s_overlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_overlay, assist_overlay_click_cb, LV_EVENT_CLICKED, NULL);

    /* the disc with its pulse ring and spinner, in a box that fits the ring */
    const int32_t box_size = s_orb * 16 / 10;
    lv_obj_t *box = lv_obj_create(s_overlay);
    lv_obj_remove_style_all(box);
    lv_obj_set_size(box, box_size, box_size);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_scrollable(box, false);

    s_ring = lv_obj_create(box);
    lv_obj_remove_style_all(s_ring);
    lv_obj_set_size(s_ring, s_orb, s_orb);
    lv_obj_set_style_radius(s_ring, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_center(s_ring);
    lv_obj_remove_flag(s_ring, LV_OBJ_FLAG_CLICKABLE);

    s_spinner = lv_spinner_create(box);
    lv_obj_set_size(s_spinner, s_orb + s_orb / 5, s_orb + s_orb / 5);
    lv_obj_center(s_spinner);
    lv_obj_set_style_arc_width(s_spinner, LV_MAX(3, s_orb / 30), LV_PART_MAIN);
    lv_obj_set_style_arc_width(s_spinner, LV_MAX(3, s_orb / 30), LV_PART_INDICATOR);
    lv_obj_set_style_arc_opa(s_spinner, LV_OPA_20, LV_PART_MAIN);
    lv_obj_remove_flag(s_spinner, LV_OBJ_FLAG_CLICKABLE);

    s_disc = lv_obj_create(box);
    lv_obj_remove_style_all(s_disc);
    lv_obj_set_size(s_disc, s_orb, s_orb);
    lv_obj_set_style_radius(s_disc, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_disc, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_center(s_disc);
    lv_obj_remove_flag(s_disc, LV_OBJ_FLAG_CLICKABLE);

    s_icon = lv_label_create(s_disc);
    lv_label_set_text(s_icon, "");
    lv_obj_set_style_text_color(s_icon, lv_color_white(), LV_PART_MAIN);
    lv_obj_center(s_icon);

    s_status = assist_label(s_overlay, APP_FONT_TEXT_24, ASSIST_COLOR_MUTED);
    s_text = assist_label(s_overlay, APP_FONT_TEXT_28, ASSIST_COLOR_TEXT);
    lv_obj_set_style_max_height(s_text, APP_SCREEN_HEIGHT * 2 / 5, LV_PART_MAIN);
    s_hint = assist_label(s_overlay, APP_FONT_TEXT_18, ASSIST_COLOR_MUTED);
    lv_obj_set_style_text_opa(s_hint, LV_OPA_70, LV_PART_MAIN);

    lv_obj_add_flag(s_overlay, LV_OBJ_FLAG_HIDDEN);
}

static void assist_overlay_icon(const char *name, const char *fallback)
{
    if (!ui_mdi_icon_apply(s_icon, name, s_orb / 2)) {
        lv_label_set_text(s_icon, fallback);
        lv_obj_set_style_text_font(s_icon, &lv_font_montserrat_28, LV_PART_MAIN);
    }
    lv_obj_center(s_icon);
}

static void assist_set_label(lv_obj_t *label, const char *text)
{
    lv_label_set_text(label, text);
    if (text[0] == '\0') {
        lv_obj_add_flag(label, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_remove_flag(label, LV_OBJ_FLAG_HIDDEN);
    }
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
    /* Counts as activity like a touch: ends the screensaver (or wakes the
     * dark panel) for a session the wake word started, and keeps the
     * display on while the answer is shown. */
    if (st.state != PANEL_ASSIST_IDLE && st.state != s_shown_state) {
        display_note_activity_from("assist");
    }
    const panel_assist_state_t prev = s_shown_state;
    s_shown_state = st.state;
    assist_chip_tint(st.state);

    if (st.state == PANEL_ASSIST_IDLE) {
        assist_overlay_hide();
        return;
    }
    assist_overlay_create();
    lv_obj_remove_flag(s_overlay, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_overlay);
    s_hide_at_ms = 0;

    uint32_t color = ASSIST_COLOR_LISTEN;
    const char *icon = "microphone";
    const char *fallback = LV_SYMBOL_AUDIO;
    const char *status = "";
    const char *text = "";
    const char *hint = "";
    bool spin = false;
    char quoted[192];
    switch (st.state) {
    case PANEL_ASSIST_LISTENING:
        status = ui_i18n_get("assist.listening", "Listening...");
        hint = ui_i18n_get("assist.hint_listening", "Tap when you are done");
        break;
    case PANEL_ASSIST_PROCESSING:
        color = ASSIST_COLOR_WORK;
        spin = true;
        status = ui_i18n_get("assist.processing", "One moment...");
        if (st.heard[0] != '\0') {
            snprintf(quoted, sizeof(quoted), "\"%s\"", st.heard);
            text = quoted;
        }
        hint = ui_i18n_get("assist.hint_cancel", "Tap to cancel");
        break;
    case PANEL_ASSIST_DONE:
        color = ASSIST_COLOR_DONE;
        icon = "check";
        fallback = LV_SYMBOL_OK;
        if (st.heard[0] != '\0') {
            snprintf(quoted, sizeof(quoted), "\"%s\"", st.heard);
            status = quoted;
        }
        text = (st.reply[0] != '\0') ? st.reply : ui_i18n_get("assist.done", "Done");
        hint = ui_i18n_get("assist.hint_close", "Tap to close");
        s_hide_at_ms = assist_now_ms() + ASSIST_SHOW_MIN_MS + (int64_t)strlen(text) * ASSIST_SHOW_MS_PER_CHAR;
        break;
    case PANEL_ASSIST_ERROR:
    default:
        color = ASSIST_COLOR_ERROR;
        icon = "alert-circle-outline";
        fallback = LV_SYMBOL_WARNING;
        if (st.heard[0] != '\0') {
            snprintf(quoted, sizeof(quoted), "\"%s\"", st.heard);
            status = quoted;
        }
        text = assist_error_text(st.error);
        hint = ui_i18n_get("assist.hint_close", "Tap to close");
        s_hide_at_ms = assist_now_ms() + ASSIST_SHOW_MIN_MS;
        break;
    }
    lv_obj_set_style_bg_color(s_disc, lv_color_hex(color), LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_ring, lv_color_hex(color), LV_PART_MAIN);
    lv_obj_set_style_arc_color(s_spinner, lv_color_hex(ASSIST_COLOR_LISTEN), LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(s_spinner, lv_color_hex(ASSIST_COLOR_MUTED), LV_PART_MAIN);
    if (st.state != prev || lv_label_get_text(s_icon)[0] == '\0') {
        assist_overlay_icon(icon, fallback);
    }
    if (spin) {
        lv_obj_remove_flag(s_spinner, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_spinner, LV_OBJ_FLAG_HIDDEN);
    }
    assist_pulse(st.state == PANEL_ASSIST_LISTENING);
    assist_set_label(s_status, status);
    assist_set_label(s_text, text);
    assist_set_label(s_hint, hint);
}

static void assist_timer_cb(lv_timer_t *timer)
{
    LV_UNUSED(timer);
    assist_refresh();
    if (s_hide_at_ms != 0 && assist_now_ms() >= s_hide_at_ms && !panel_assist_is_busy()) {
        assist_overlay_hide();
    }
}

static void assist_chip_event_cb(lv_event_t *event)
{
    const lv_event_code_t code = lv_event_get_code(event);
    if (code == LV_EVENT_PRESSED) {
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
    /* The overlay lives on the top layer and survives page rebuilds. */
    if (s_timer == NULL) {
        s_timer = lv_timer_create(assist_timer_cb, 100, NULL);
    }
}
