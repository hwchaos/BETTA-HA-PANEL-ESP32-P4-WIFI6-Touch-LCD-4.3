#include "ui/ui_topbar_alerts.h"
#include "ui/ui_i18n.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "app_config.h"
#include "ha/ha_client.h"
#include "ha/ha_model.h"
#include "ui/fonts/app_text_fonts.h"
#include "ui/theme/theme_default.h"
#include "ui/ui_mdi_icon.h"
#include "ui/ui_screen_saver.h"

#define ALERT_COLOR 0xE53935
#define ALERT_REFRESH_MS 2000
#define ALERT_MAX_ENTRIES 12
#define ALERT_PN_MAX 8
#define ALERT_TITLE_LEN 96
#define ALERT_TEXT_LEN 320

typedef struct {
    char title[ALERT_TITLE_LEN];
    char text[ALERT_TEXT_LEN];
} alert_entry_t;

typedef struct {
    char id[64];
    alert_entry_t entry;
} alert_pn_t; /* HA persistent notification */

static SemaphoreHandle_t s_lock;
static alert_pn_t s_pn[ALERT_PN_MAX];
static int s_pn_count;
static bool s_pn_enabled;
static bool s_pn_subscribed;
static char s_entities[RUNTIME_ALERT_ENTITIES][APP_MAX_ENTITY_ID_LEN];

static lv_obj_t *s_chip;
static lv_timer_t *s_timer;
static void (*s_relayout)(void);
static bool s_visible;
static bool s_blinking;
static uint32_t s_read_sig;
static uint32_t s_current_sig;
static lv_obj_t *s_popup;
static ha_state_t s_state; /* large: off the stack */
static bool s_suppressed;

static alert_entry_t s_entries[ALERT_MAX_ENTRIES];
static int s_entry_count;

/* ---- HA persistent notifications (WS task) ---- */

static void alert_pn_copy(alert_pn_t *dst, const char *id, cJSON *n)
{
    snprintf(dst->id, sizeof(dst->id), "%s", id);
    cJSON *title = cJSON_GetObjectItemCaseSensitive(n, "title");
    cJSON *message = cJSON_GetObjectItemCaseSensitive(n, "message");
    snprintf(dst->entry.title, sizeof(dst->entry.title), "%s",
        (cJSON_IsString(title) && title->valuestring != NULL && title->valuestring[0] != '\0') ? title->valuestring
                                                                                               : "Home Assistant");
    snprintf(dst->entry.text, sizeof(dst->entry.text), "%s",
        (cJSON_IsString(message) && message->valuestring != NULL) ? message->valuestring : "");
}

static void alert_pn_event_cb(cJSON *event, void *user)
{
    (void)user;
    cJSON *type = cJSON_GetObjectItemCaseSensitive(event, "type");
    cJSON *list = cJSON_GetObjectItemCaseSensitive(event, "notifications");
    if (!cJSON_IsString(type) || !cJSON_IsObject(list) || s_lock == NULL) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (strcmp(type->valuestring, "current") == 0) {
        s_pn_count = 0;
    }
    cJSON *n = NULL;
    cJSON_ArrayForEach(n, list)
    {
        const char *id = n->string != NULL ? n->string : "";
        int found = -1;
        for (int i = 0; i < s_pn_count; i++) {
            if (strcmp(s_pn[i].id, id) == 0) {
                found = i;
                break;
            }
        }
        if (strcmp(type->valuestring, "removed") == 0) {
            if (found >= 0) {
                memmove(&s_pn[found], &s_pn[found + 1], sizeof(s_pn[0]) * (size_t)(s_pn_count - found - 1));
                s_pn_count--;
            }
            continue;
        }
        if (found < 0) {
            if (s_pn_count >= ALERT_PN_MAX) {
                continue;
            }
            found = s_pn_count++;
        }
        alert_pn_copy(&s_pn[found], id, n);
    }
    xSemaphoreGive(s_lock);
}

/* ---- warning entities (LVGL task) ---- */

static bool alert_state_active(const char *state)
{
    if (state == NULL || state[0] == '\0') {
        return false;
    }
    char low[32];
    size_t n = 0;
    for (; state[n] != '\0' && n < sizeof(low) - 1; n++) {
        low[n] = (char)tolower((unsigned char)state[n]);
    }
    low[n] = '\0';
    static const char *const idle[] = {"off", "unknown", "unavailable", "none", "false", "no", "ok", "normal",
        "aus", "keine", "nein", "0", "clear", "idle"};
    for (size_t i = 0; i < sizeof(idle) / sizeof(idle[0]); i++) {
        if (strcmp(low, idle[i]) == 0) {
            return false;
        }
    }
    char *end = NULL;
    const double v = strtod(state, &end);
    if (end != state && *end == '\0') {
        return v > 0.0;
    }
    return true;
}

static void alert_add(const char *title, const char *text)
{
    if (s_entry_count >= ALERT_MAX_ENTRIES) {
        return;
    }
    alert_entry_t *e = &s_entries[s_entry_count++];
    /* Long texts are cut on purpose. */
    strlcpy(e->title, title != NULL ? title : "", sizeof(e->title));
    strlcpy(e->text, text != NULL ? text : "", sizeof(e->text));
}

static void alert_collect_entity(const char *entity_id)
{
    if (entity_id[0] == '\0' || !ha_model_get_state(entity_id, &s_state) || !alert_state_active(s_state.state)) {
        return;
    }
    cJSON *attrs = cJSON_Parse(s_state.attributes_json);
    cJSON *fname = attrs != NULL ? cJSON_GetObjectItemCaseSensitive(attrs, "friendly_name") : NULL;
    const char *name = (cJSON_IsString(fname) && fname->valuestring != NULL) ? fname->valuestring : entity_id;

    /* DWD weather warnings: warning_count + warning_N_headline/description. */
    int added = 0;
    cJSON *count = attrs != NULL ? cJSON_GetObjectItemCaseSensitive(attrs, "warning_count") : NULL;
    if (cJSON_IsNumber(count)) {
        for (int i = 1; i <= (int)count->valuedouble && i <= 6; i++) {
            char key[40];
            snprintf(key, sizeof(key), "warning_%d_headline", i);
            cJSON *headline = cJSON_GetObjectItemCaseSensitive(attrs, key);
            snprintf(key, sizeof(key), "warning_%d_description", i);
            cJSON *desc = cJSON_GetObjectItemCaseSensitive(attrs, key);
            if (cJSON_IsString(headline) && headline->valuestring != NULL) {
                alert_add(headline->valuestring, cJSON_IsString(desc) ? desc->valuestring : "");
                added++;
            }
        }
    }
    if (added == 0) {
        cJSON *unit = attrs != NULL ? cJSON_GetObjectItemCaseSensitive(attrs, "unit_of_measurement") : NULL;
        char text[96];
        snprintf(text, sizeof(text), "%s%s%s", s_state.state, cJSON_IsString(unit) ? " " : "",
            cJSON_IsString(unit) ? unit->valuestring : "");
        alert_add(name, text);
    }
    cJSON_Delete(attrs);
}

static uint32_t alert_signature(void)
{
    uint32_t h = 2166136261u;
    for (int i = 0; i < s_entry_count; i++) {
        for (const char *p = s_entries[i].title; *p; p++) {
            h = (h ^ (uint8_t)*p) * 16777619u;
        }
        for (const char *p = s_entries[i].text; *p; p++) {
            h = (h ^ (uint8_t)*p) * 16777619u;
        }
        h = (h ^ 0x1F) * 16777619u;
    }
    return h;
}

static void alert_collect(void)
{
    s_entry_count = 0;
    if (s_lock != NULL && s_pn_enabled) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        for (int i = 0; i < s_pn_count; i++) {
            alert_add(s_pn[i].entry.title, s_pn[i].entry.text);
        }
        xSemaphoreGive(s_lock);
    }
    for (int i = 0; i < RUNTIME_ALERT_ENTITIES; i++) {
        alert_collect_entity(s_entities[i]);
    }
    s_current_sig = alert_signature();
}

/* ---- chip ---- */

static void alert_blink_exec(void *obj, int32_t v)
{
    lv_obj_set_style_opa((lv_obj_t *)obj, (lv_opa_t)v, LV_PART_MAIN);
}

static void alert_set_blink(bool on)
{
    if (s_chip == NULL || s_blinking == on) {
        return;
    }
    s_blinking = on;
    lv_anim_delete(s_chip, alert_blink_exec);
    if (!on) {
        lv_obj_set_style_opa(s_chip, LV_OPA_COVER, LV_PART_MAIN);
        return;
    }
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_chip);
    lv_anim_set_exec_cb(&a, alert_blink_exec);
    lv_anim_set_values(&a, LV_OPA_COVER, LV_OPA_20);
    lv_anim_set_duration(&a, 500);
    lv_anim_set_playback_duration(&a, 500);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_anim_start(&a);
}

static void alert_refresh(void)
{
    if (s_pn_enabled && !s_pn_subscribed) {
        s_pn_subscribed =
            ha_client_ws_subscribe("persistent_notification/subscribe", alert_pn_event_cb, NULL) == ESP_OK;
    }
    if (s_chip == NULL) {
        return;
    }
    alert_collect();
    const bool visible = s_entry_count > 0 && !s_suppressed;
    if (visible != s_visible) {
        s_visible = visible;
        if (visible) {
            lv_obj_clear_flag(s_chip, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_chip, LV_OBJ_FLAG_HIDDEN);
        }
        if (s_relayout != NULL) {
            s_relayout();
        }
    }
    alert_set_blink(visible && s_current_sig != s_read_sig);
}

static void alert_timer_cb(lv_timer_t *timer)
{
    LV_UNUSED(timer);
    alert_refresh();
}

/* ---- popup ---- */

static void alert_popup_close(void)
{
    if (s_popup != NULL) {
        lv_obj_del(s_popup);
        s_popup = NULL;
    }
}

static void alert_read_cb(lv_event_t *event)
{
    LV_UNUSED(event);
    s_read_sig = s_current_sig;
    alert_popup_close();
    alert_set_blink(false);
}

static void alert_close_cb(lv_event_t *event)
{
    LV_UNUSED(event);
    alert_popup_close();
}

static lv_obj_t *alert_button(lv_obj_t *parent, const char *text, uint32_t color, lv_event_cb_t cb)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_style_bg_color(btn, lv_color_hex(color), LV_PART_MAIN);
    lv_obj_set_style_radius(btn, 12, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(btn, 22, LV_PART_MAIN);
    lv_obj_set_style_pad_ver(btn, 10, LV_PART_MAIN);
    lv_obj_t *label = lv_label_create(btn);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, APP_FONT_TEXT_18, LV_PART_MAIN);
    lv_obj_center(label);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);
    return btn;
}

static void alert_popup_open(void)
{
    alert_popup_close();
    alert_collect();
    ui_screen_saver_wake();

    lv_obj_t *overlay = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(overlay);
    lv_obj_set_size(overlay, APP_SCREEN_WIDTH, APP_SCREEN_HEIGHT);
    lv_obj_set_style_bg_color(overlay, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(overlay, LV_OPA_60, LV_PART_MAIN);
    lv_obj_add_flag(overlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(overlay, alert_close_cb, LV_EVENT_CLICKED, NULL);
    s_popup = overlay;

    lv_obj_t *card = lv_obj_create(overlay);
    const lv_coord_t w = APP_SCREEN_WIDTH > 700 ? 640 : APP_SCREEN_WIDTH - 40;
    lv_obj_set_width(card, w);
    lv_obj_set_height(card, LV_SIZE_CONTENT);
    lv_obj_set_style_max_height(card, APP_SCREEN_HEIGHT - 60, LV_PART_MAIN);
    lv_obj_center(card);
    lv_obj_set_style_bg_color(card, lv_color_hex(APP_UI_COLOR_CARD_BG_OFF), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(card, lv_color_hex(ALERT_COLOR), LV_PART_MAIN);
    lv_obj_set_style_border_width(card, 2, LV_PART_MAIN);
    lv_obj_set_style_radius(card, 18, LV_PART_MAIN);
    lv_obj_set_style_pad_all(card, 18, LV_PART_MAIN);
    lv_obj_set_style_pad_row(card, 10, LV_PART_MAIN);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE); /* taps on the card do not close */

    lv_obj_t *title = lv_label_create(card);
    lv_label_set_text(title, s_entry_count > 0 ? ui_i18n_get("alerts.title", "Notifications")
                                            : ui_i18n_get("alerts.none", "No notifications"));
    lv_obj_set_style_text_font(title, APP_FONT_TEXT_22, LV_PART_MAIN);
    lv_obj_set_style_text_color(title, lv_color_hex(ALERT_COLOR), LV_PART_MAIN);

    lv_obj_t *list = lv_obj_create(card);
    lv_obj_remove_style_all(list);
    lv_obj_set_width(list, LV_PCT(100));
    lv_obj_set_height(list, LV_SIZE_CONTENT);
    lv_obj_set_style_max_height(list, APP_SCREEN_HEIGHT - 220, LV_PART_MAIN);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(list, 12, LV_PART_MAIN);
    lv_obj_add_flag(list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);
    for (int i = 0; i < s_entry_count; i++) {
        lv_obj_t *head = lv_label_create(list);
        lv_obj_set_width(head, LV_PCT(100));
        lv_label_set_long_mode(head, LV_LABEL_LONG_WRAP);
        lv_label_set_text(head, s_entries[i].title);
        lv_obj_set_style_text_font(head, APP_FONT_TEXT_20, LV_PART_MAIN);
        lv_obj_set_style_text_color(head, lv_color_hex(APP_UI_COLOR_TEXT_PRIMARY), LV_PART_MAIN);
        if (s_entries[i].text[0] != '\0') {
            lv_obj_t *body = lv_label_create(list);
            lv_obj_set_width(body, LV_PCT(100));
            lv_label_set_long_mode(body, LV_LABEL_LONG_WRAP);
            lv_label_set_text(body, s_entries[i].text);
            lv_obj_set_style_text_font(body, APP_FONT_TEXT_16, LV_PART_MAIN);
            lv_obj_set_style_text_color(body, lv_color_hex(APP_UI_COLOR_TEXT_MUTED), LV_PART_MAIN);
        }
    }

    lv_obj_t *row = lv_obj_create(card);
    lv_obj_remove_style_all(row);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 12, LV_PART_MAIN);
    (void)alert_button(row, ui_i18n_get("alerts.close", "Close"), 0x455A64, alert_close_cb);
    if (s_entry_count > 0) {
        (void)alert_button(row, ui_i18n_get("alerts.read", "Mark read"), ALERT_COLOR, alert_read_cb);
    }
}

static void alert_chip_click_cb(lv_event_t *event)
{
    LV_UNUSED(event);
    alert_popup_open();
}

lv_obj_t *ui_topbar_alerts_create(lv_obj_t *topbar, void (*relayout)(void))
{
    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutex();
    }
    s_relayout = relayout;
    lv_obj_t *chip = lv_label_create(topbar);
    lv_obj_remove_style_all(chip);
    lv_obj_add_flag(chip, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(chip, 10);
    lv_obj_set_style_bg_color(chip, lv_color_hex(APP_UI_COLOR_TOPBAR_CHIP_BG), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(chip, LV_OPA_70, LV_PART_MAIN);
    lv_obj_set_style_border_width(chip, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(chip, lv_color_hex(APP_UI_COLOR_TOPBAR_CHIP_BORDER), LV_PART_MAIN);
    lv_obj_set_style_border_opa(chip, LV_OPA_80, LV_PART_MAIN);
    lv_obj_set_style_text_align(chip, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_style_text_color(chip, lv_color_hex(ALERT_COLOR), LV_PART_MAIN);
    if (!ui_mdi_icon_apply(chip, "alert", 24)) {
        lv_label_set_text(chip, LV_SYMBOL_WARNING);
        lv_obj_set_style_text_font(chip, &lv_font_montserrat_20, LV_PART_MAIN);
    }
    lv_obj_add_event_cb(chip, alert_chip_click_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_flag(chip, LV_OBJ_FLAG_HIDDEN);
    s_chip = chip;
    s_visible = false;
    s_blinking = false;
    if (s_timer == NULL) {
        s_timer = lv_timer_create(alert_timer_cb, ALERT_REFRESH_MS, NULL);
    }
    return chip;
}

void ui_topbar_alerts_reset(void)
{
    s_chip = NULL;
    s_visible = false;
    s_blinking = false;
    alert_popup_close(); /* on the top layer, which survives the screen rebuild */
}

void ui_topbar_alerts_apply_settings(const runtime_settings_t *settings)
{
    if (settings == NULL) {
        return;
    }
    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutex();
    }
    const char *ids[RUNTIME_ALERT_ENTITIES];
    for (int i = 0; i < RUNTIME_ALERT_ENTITIES; i++) {
        snprintf(s_entities[i], sizeof(s_entities[i]), "%s", settings->display_alert_entities[i]);
        ids[i] = s_entities[i];
    }
    ha_client_set_extra_entities(RUNTIME_WASTE_SLOTS, ids, RUNTIME_ALERT_ENTITIES);
    s_pn_enabled = settings->display_alert_persistent;
    if (s_timer != NULL) {
        lv_timer_ready(s_timer);
    }
    if (s_pn_enabled && !s_pn_subscribed) {
        s_pn_subscribed =
            ha_client_ws_subscribe("persistent_notification/subscribe", alert_pn_event_cb, NULL) == ESP_OK;
    }
}

void ui_topbar_alerts_set_suppressed(bool suppressed)
{
    if (s_suppressed == suppressed) {
        return;
    }
    s_suppressed = suppressed;
    if (s_chip != NULL && suppressed) {
        lv_obj_add_flag(s_chip, LV_OBJ_FLAG_HIDDEN);
        s_visible = false;
        alert_set_blink(false);
    }
}

bool ui_topbar_alerts_visible(void)
{
    return s_chip != NULL && s_visible;
}

lv_obj_t *ui_topbar_alerts_chip(void)
{
    return s_chip;
}
