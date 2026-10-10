#include "ui/ui_topbar_waste.h"
#include "ui/ui_i18n.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "cJSON.h"
#include "ha/ha_client.h"
#include "ha/ha_model.h"
#include "ui/fonts/app_text_fonts.h"
#include "ui/theme/theme_default.h"
#include "ui/ui_mdi_icon.h"

#define WASTE_SLOTS RUNTIME_WASTE_SLOTS
#define WASTE_REFRESH_MS 10000
#define WASTE_ICON_PX 22

typedef struct {
    char entity_id[APP_MAX_ENTITY_ID_LEN];
    uint32_t color;
    lv_obj_t *chip;
    lv_obj_t *icon;
    lv_obj_t *days;
    int shown_days; /* -1 = hidden */
    bool blinking;
} waste_slot_t;

static waste_slot_t s_slots[WASTE_SLOTS];
static lv_timer_t *s_timer;
static void (*s_relayout)(void);
static ha_state_t s_state; /* large: kept off the stack */
static bool s_suppressed;

/* ---- days until pickup from the many shapes such sensors come in ---- */

static int waste_days_until_date(const char *text)
{
    int y = 0, m = 0, d = 0;
    if (text == NULL || sscanf(text, "%4d-%2d-%2d", &y, &m, &d) != 3 || y < 2000 || m < 1 || m > 12 || d < 1) {
        return -1;
    }
    time_t now = time(NULL);
    struct tm today;
    localtime_r(&now, &today);
    today.tm_hour = 12;
    today.tm_min = 0;
    today.tm_sec = 0;
    struct tm then = {0};
    then.tm_year = y - 1900;
    then.tm_mon = m - 1;
    then.tm_mday = d;
    then.tm_hour = 12;
    then.tm_isdst = -1;
    const double diff = difftime(mktime(&then), mktime(&today));
    const int days = (int)((diff + (diff >= 0 ? 43200.0 : -43200.0)) / 86400.0);
    return days >= 0 ? days : -1;
}

static bool waste_parse_int(const char *text, int *out)
{
    if (text == NULL || *text == '\0') {
        return false;
    }
    char *end = NULL;
    const double v = strtod(text, &end);
    while (end != NULL && isspace((unsigned char)*end)) {
        end++;
    }
    if (end == text || (end != NULL && *end != '\0')) {
        return false;
    }
    *out = (int)(v + 0.5);
    return true;
}

static bool waste_text_has(const char *text, const char *word)
{
    char low[96];
    size_t n = 0;
    for (; text[n] != '\0' && n < sizeof(low) - 1; n++) {
        low[n] = (char)tolower((unsigned char)text[n]);
    }
    low[n] = '\0';
    return strstr(low, word) != NULL;
}

static int waste_days_from_state(const ha_state_t *st)
{
    const char *s = st->state;
    if (s[0] == '\0' || strcmp(s, "unavailable") == 0 || strcmp(s, "unknown") == 0) {
        return -1;
    }
    int days = -1;
    if (waste_parse_int(s, &days)) {
        return days >= 0 ? days : -1;
    }
    cJSON *attrs = cJSON_Parse(st->attributes_json);
    if (attrs != NULL) {
        static const char *const number_keys[] = {"daysTo", "days_to", "days", "days_until"};
        for (size_t i = 0; i < sizeof(number_keys) / sizeof(number_keys[0]) && days < 0; i++) {
            cJSON *v = cJSON_GetObjectItemCaseSensitive(attrs, number_keys[i]);
            if (cJSON_IsNumber(v) && v->valuedouble >= 0) {
                days = (int)(v->valuedouble + 0.5);
            }
        }
        static const char *const date_keys[] = {"next_date", "date", "start_time", "next_collection"};
        for (size_t i = 0; i < sizeof(date_keys) / sizeof(date_keys[0]) && days < 0; i++) {
            cJSON *v = cJSON_GetObjectItemCaseSensitive(attrs, date_keys[i]);
            if (cJSON_IsString(v)) {
                days = waste_days_until_date(v->valuestring);
            }
        }
        cJSON_Delete(attrs);
    }
    if (days >= 0) {
        return days;
    }
    days = waste_days_until_date(s);
    if (days >= 0) {
        return days;
    }
    if (waste_text_has(s, "heute") || waste_text_has(s, "today")) {
        return 0;
    }
    if (waste_text_has(s, "bermorgen")) { /* "übermorgen" */
        return 2;
    }
    if (waste_text_has(s, "morgen") || waste_text_has(s, "tomorrow")) {
        return 1;
    }
    for (const char *p = s; *p != '\0'; p++) {
        if (isdigit((unsigned char)*p)) {
            return atoi(p);
        }
    }
    return -1;
}

/* ---- chips ---- */

static void waste_blink_exec(void *obj, int32_t v)
{
    lv_obj_set_style_opa((lv_obj_t *)obj, (lv_opa_t)v, LV_PART_MAIN);
}

static void waste_set_blink(waste_slot_t *slot, bool on)
{
    if (slot->chip == NULL || slot->blinking == on) {
        return;
    }
    slot->blinking = on;
    lv_anim_delete(slot->chip, waste_blink_exec);
    if (!on) {
        lv_obj_set_style_opa(slot->chip, LV_OPA_COVER, LV_PART_MAIN);
        return;
    }
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, slot->chip);
    lv_anim_set_exec_cb(&a, waste_blink_exec);
    lv_anim_set_values(&a, LV_OPA_COVER, LV_OPA_20);
    lv_anim_set_duration(&a, 600);
    lv_anim_set_playback_duration(&a, 600);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_anim_start(&a);
}

static void waste_refresh(void)
{
    bool relayout = false;
    for (int i = 0; i < WASTE_SLOTS; i++) {
        waste_slot_t *slot = &s_slots[i];
        if (slot->chip == NULL) {
            continue;
        }
        int days = -1;
        if (slot->entity_id[0] != '\0' && ha_model_get_state(slot->entity_id, &s_state)) {
            days = waste_days_from_state(&s_state);
        }
        if (days != slot->shown_days) {
            if ((days < 0) != (slot->shown_days < 0)) {
                relayout = true;
            }
            slot->shown_days = days;
            if (days < 0 || s_suppressed) {
                lv_obj_add_flag(slot->chip, LV_OBJ_FLAG_HIDDEN);
            } else {
                char text[12];
                if (days == 0) {
                    snprintf(text, sizeof(text), "%s", ui_i18n_get("waste.today", "today"));
                } else if (days > 99) {
                    snprintf(text, sizeof(text), "99+");
                } else {
                    snprintf(text, sizeof(text), "%d%s", days, ui_i18n_get("waste.days_suffix", "d"));
                }
                lv_label_set_text(slot->days, text);
                lv_obj_clear_flag(slot->chip, LV_OBJ_FLAG_HIDDEN);
            }
            relayout = true;
        }
        lv_obj_set_style_text_color(slot->icon, lv_color_hex(slot->color), LV_PART_MAIN);
        waste_set_blink(slot, days == 0);
    }
    if (relayout && s_relayout != NULL) {
        s_relayout();
    }
}

static void waste_timer_cb(lv_timer_t *timer)
{
    LV_UNUSED(timer);
    waste_refresh();
}

void ui_topbar_waste_create(lv_obj_t *topbar, void (*relayout)(void))
{
    s_relayout = relayout;
    for (int i = 0; i < WASTE_SLOTS; i++) {
        waste_slot_t *slot = &s_slots[i];
        lv_obj_t *chip = lv_obj_create(topbar);
        lv_obj_remove_style_all(chip);
        lv_obj_set_size(chip, LV_SIZE_CONTENT, 34);
        lv_obj_set_style_bg_color(chip, lv_color_hex(APP_UI_COLOR_TOPBAR_CHIP_BG), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(chip, LV_OPA_70, LV_PART_MAIN);
        lv_obj_set_style_border_width(chip, 1, LV_PART_MAIN);
        lv_obj_set_style_border_color(chip, lv_color_hex(APP_UI_COLOR_TOPBAR_CHIP_BORDER), LV_PART_MAIN);
        lv_obj_set_style_border_opa(chip, LV_OPA_80, LV_PART_MAIN);
        lv_obj_set_style_radius(chip, 10, LV_PART_MAIN);
        lv_obj_set_style_pad_hor(chip, 6, LV_PART_MAIN);
        lv_obj_set_style_pad_column(chip, 3, LV_PART_MAIN);
        lv_obj_set_flex_flow(chip, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(chip, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_clear_flag(chip, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

        lv_obj_t *icon = lv_label_create(chip);
        if (!ui_mdi_icon_apply(icon, "trash-can", WASTE_ICON_PX)) {
            lv_label_set_text(icon, LV_SYMBOL_TRASH);
        }
        lv_obj_t *days = lv_label_create(chip);
        lv_obj_set_style_text_font(days, APP_FONT_TEXT_16, LV_PART_MAIN);
        lv_obj_set_style_text_color(days, lv_color_hex(APP_UI_COLOR_TOPBAR_TEXT), LV_PART_MAIN);
        lv_label_set_text(days, "");

        lv_obj_add_flag(chip, LV_OBJ_FLAG_HIDDEN);
        slot->chip = chip;
        slot->icon = icon;
        slot->days = days;
        slot->shown_days = -1;
        slot->blinking = false;
    }
    if (s_timer == NULL) {
        s_timer = lv_timer_create(waste_timer_cb, WASTE_REFRESH_MS, NULL);
    }
    waste_refresh();
}

void ui_topbar_waste_reset(void)
{
    for (int i = 0; i < WASTE_SLOTS; i++) {
        s_slots[i].chip = NULL;
        s_slots[i].icon = NULL;
        s_slots[i].days = NULL;
        s_slots[i].shown_days = -1;
        s_slots[i].blinking = false;
    }
}

void ui_topbar_waste_apply_settings(const runtime_settings_t *settings)
{
    if (settings == NULL) {
        return;
    }
    const char *ids[WASTE_SLOTS];
    for (int i = 0; i < WASTE_SLOTS; i++) {
        if (strcmp(s_slots[i].entity_id, settings->display_waste[i].entity_id) != 0) {
            snprintf(s_slots[i].entity_id, sizeof(s_slots[i].entity_id), "%s", settings->display_waste[i].entity_id);
            s_slots[i].shown_days = -2; /* other entity: redraw */
        }
        s_slots[i].color = settings->display_waste[i].color & 0xFFFFFF;
        ids[i] = s_slots[i].entity_id;
    }
    ha_client_set_extra_entities(0, ids, WASTE_SLOTS); /* slots 0..3; alerts use 4..7 */
    if (s_timer != NULL) {
        lv_timer_ready(s_timer); /* show the change right away */
    }
}

void ui_topbar_waste_set_suppressed(bool suppressed)
{
    s_suppressed = suppressed;
    for (int i = 0; i < WASTE_SLOTS; i++) {
        if (s_slots[i].chip == NULL) {
            continue;
        }
        if (suppressed || s_slots[i].shown_days < 0) {
            lv_obj_add_flag(s_slots[i].chip, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_clear_flag(s_slots[i].chip, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

lv_coord_t ui_topbar_waste_width(lv_coord_t gap)
{
    if (s_suppressed) {
        return 0;
    }
    lv_coord_t total = 0;
    int n = 0;
    for (int i = 0; i < WASTE_SLOTS; i++) {
        if (s_slots[i].chip == NULL || s_slots[i].shown_days < 0) {
            continue;
        }
        lv_obj_update_layout(s_slots[i].chip);
        total += lv_obj_get_width(s_slots[i].chip);
        n++;
    }
    return n > 0 ? total + (lv_coord_t)(n - 1) * gap : 0;
}

lv_coord_t ui_topbar_waste_layout(lv_coord_t left, lv_coord_t gap)
{
    if (s_suppressed) {
        return left;
    }
    /* Soonest pickup first; equal days keep the configured order. */
    int order[WASTE_SLOTS];
    for (int i = 0; i < WASTE_SLOTS; i++) {
        order[i] = i;
    }
    for (int i = 1; i < WASTE_SLOTS; i++) {
        const int cur = order[i];
        int j = i - 1;
        while (j >= 0 && s_slots[order[j]].shown_days > s_slots[cur].shown_days) {
            order[j + 1] = order[j];
            j--;
        }
        order[j + 1] = cur;
    }
    for (int k = 0; k < WASTE_SLOTS; k++) {
        waste_slot_t *slot = &s_slots[order[k]];
        if (slot->chip == NULL || slot->shown_days < 0) {
            continue;
        }
        lv_obj_update_layout(slot->chip);
        lv_obj_align(slot->chip, LV_ALIGN_LEFT_MID, left, 0);
        left += lv_obj_get_width(slot->chip) + gap;
    }
    return left;
}
