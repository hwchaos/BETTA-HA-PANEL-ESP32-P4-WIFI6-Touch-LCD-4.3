/* SPDX-License-Identifier: LicenseRef-FNCL-1.1
 * Copyright (c) 2026 Cpt_Kirk
 *
 * Alarm tile for alarm_control_panel entities (Home Assistant / Alarmo).
 * Arm (away/home/night/vacation/custom) and disarm with an optional code. The
 * code can be stored in the layout (alarm_code) or typed on the built-in keypad
 * (alarm_ask_code) - Alarmo needs "code" to accept arm/disarm in most setups.
 *
 * Alarmo extras, all opt-in from the layout (defaults keep the classic look):
 *   - open/bypassed sensor list (alarm_show_sensors / alarm_show_bypassed) with
 *     friendly names resolved from the entity registry,
 *   - exit/entry delay countdown from the "delay" attribute,
 *   - force-arm confirmation calling alarmo.arm with force=true (alarm_force_arm),
 *   - alarmo.arm / alarmo.disarm services, optional skip_delay (alarm_backend,
 *     alarm_skip_delay),
 *   - zone caption for multi-zone setups (alarm_zone_label) - every Alarmo zone
 *     is a separate alarm_control_panel entity, so one tile per zone.
 *
 * Alarmo bus events (ha_alarm_events): a refused arming never reaches the entity
 * attributes, so the reasons come from the events instead - the info line shows
 * "Arming failed: <sensor names | reason>" in red for a few seconds, arm buttons
 * are dimmed for modes Alarmo reports as not ready, and a failed/rejected command
 * releases the busy state right away instead of waiting for the timeout.
 *
 * The PIN is detected from the entity itself (code_format / code_arm_required),
 * so a panel without alarm_ask_code still asks for the code when HA needs one.
 */
#include "ui/ui_widget_factory.h"

#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_log.h"

#include "ha/ha_alarm_events.h"
#include "ha/ha_client.h"
#include "ha/ha_model.h"
#include "ui/fonts/app_text_fonts.h"
#include "ui/theme/theme_default.h"
#include "ui/ui_i18n.h"
#include "ui/ui_memory.h"
#include "diag/system_log.h"

static const char *TAG = "w_alarm_tile";

#define W_ALARM_DEFAULT_MODES "away,home,night,disarm"
#define W_ALARM_BUSY_TIMEOUT_MS 8000
/* How long an Alarmo event text/status stays on the tile. */
#define W_ALARM_EVENT_TEXT_MS 10000
#define W_ALARM_STATUS_TEXT_LEN 72
#define W_ALARM_SENSORS_TEXT_LEN 160
/* Above this count the tile stops naming sensors one by one. */
#define W_ALARM_SENSORS_LISTED 4

typedef enum {
    W_ALARM_ACTION_AWAY = 0,
    W_ALARM_ACTION_HOME,
    W_ALARM_ACTION_NIGHT,
    W_ALARM_ACTION_VACATION,
    W_ALARM_ACTION_CUSTOM,
    W_ALARM_ACTION_DISARM,
    W_ALARM_ACTION_COUNT,
} w_alarm_action_t;

/* Which service flavour the tile talks to; AUTO sniffs the attributes. */
typedef enum {
    W_ALARM_BACKEND_AUTO = 0,
    W_ALARM_BACKEND_ALARMO,
    W_ALARM_BACKEND_BUILTIN,
} w_alarm_backend_t;

typedef struct {
    w_alarm_action_t action;
    lv_obj_t *button;
    lv_obj_t *label;
} w_alarm_button_t;

typedef struct {
    char entity_id[APP_MAX_ENTITY_ID_LEN];
    char code[APP_MAX_ALARM_CODE_LEN];
    char status_text[W_ALARM_STATUS_TEXT_LEN];
    char base_status[W_ALARM_STATUS_TEXT_LEN];
    char info_text[APP_MAX_NAME_LEN];
    /* Alarmo event text shown instead of info_text while it is fresh. */
    char alarmo_text[W_ALARM_SENSORS_TEXT_LEN];
    char alarmo_prev_status[W_ALARM_STATUS_TEXT_LEN];
    char sensors_text[W_ALARM_SENSORS_TEXT_LEN];
    char zone_label[APP_MAX_NAME_LEN];
    char confirm_code[APP_MAX_ALARM_CODE_LEN];
    lv_obj_t *card;
    lv_obj_t *dot;
    lv_obj_t *title_label;
    lv_obj_t *zone_obj;
    lv_obj_t *state_label;
    lv_obj_t *sensors_label;
    lv_obj_t *info_label;
    w_alarm_button_t buttons[W_ALARM_ACTION_COUNT];
    int button_count;
    lv_obj_t *overlay;
    lv_obj_t *code_label;
    lv_obj_t *hint_label;
    lv_obj_t *confirm_overlay;
    lv_timer_t *timer;
    char code_entry[APP_MAX_ALARM_CODE_LEN];
    w_alarm_action_t pending_action;
    w_alarm_action_t confirm_action;
    uint32_t last_accent;
    uint32_t busy_until_ms;
    uint32_t delay_anchor_ms;
    uint32_t delay_reported;
    /* Last Alarmo event seq this tile consumed (ha_alarm_events_take). */
    uint32_t alarmo_seq;
    uint32_t alarmo_text_until_ms;
    uint32_t alarmo_status_until_ms;
    uint32_t alarmo_prev_accent;
    /* Arm modes Alarmo reported (bit per w_alarm_action_t): known + ready. */
    uint32_t ready_known_mask;
    uint32_t ready_mask;
    int open_count;
    int bypassed_count;
    int delay_seconds;
    size_t status_len;
    w_alarm_backend_t backend;
    bool ask_code;
    bool unavailable;
    bool busy;
    bool have_status;
    bool show_sensors;
    bool show_bypassed;
    bool force_arm;
    bool skip_delay;
    bool delay_active;
    bool use_alarmo;
    bool alarmo_detected;
    bool alarmo_text_error;
    /* code_format present ("number"/"text") = the entity wants a code. */
    bool code_format_present;
    /* code_arm_required: arming without a code is refused (default: yes). */
    bool code_arm_required;
} w_alarm_tile_t;

/* Only one keypad can live on the top layer at a time. */
static w_alarm_tile_t *s_keypad_owner = NULL;

static void alarm_layout(w_alarm_tile_t *ctx);
static void alarm_render_status(w_alarm_tile_t *ctx);
static void alarm_dispatch(w_alarm_tile_t *ctx, w_alarm_action_t action, const char *code);

typedef struct {
    const char *state;
    const char *key;
    const char *fallback;
    uint32_t accent;
} w_alarm_state_map_t;

static const w_alarm_state_map_t W_ALARM_STATE_MAP[] = {
    {"disarmed", "alarm.disarmed", "Disarmed", 0x2ECC9A},
    {"armed_home", "alarm.armed_home", "Armed (home)", 0xFFB648},
    {"armed_away", "alarm.armed_away", "Armed (away)", 0xFF5252},
    {"armed_night", "alarm.armed_night", "Armed (night)", 0x7C6BFF},
    {"armed_vacation", "alarm.armed_vacation", "Armed (vacation)", 0xB07CFF},
    {"armed_custom_bypass", "alarm.armed_custom", "Armed (custom)", 0x41BDF5},
    {"arming", "alarm.arming", "Arming...", 0xFFA726},
    {"pending", "alarm.pending", "Pending...", 0xFFA726},
    {"disarming", "alarm.disarming", "Disarming...", 0x41BDF5},
    {"triggered", "alarm.triggered", "ALARM!", 0xFF1744},
};

static const char *alarm_i18n(const char *key, const char *fallback)
{
    const char *text = ui_i18n_get(key, NULL);
    return (text != NULL && text[0] != '\0') ? text : fallback;
}

static void alarm_text_appendf(char *buffer, size_t buffer_len, const char *format, ...)
{
    if (buffer == NULL || buffer_len == 0) {
        return;
    }
    const size_t used = strlen(buffer);
    if (used + 1 >= buffer_len) {
        return;
    }
    va_list args;
    va_start(args, format);
    vsnprintf(buffer + used, buffer_len - used, format, args);
    va_end(args);
}

/* Device class ("door", "window", ...) translated to the UI language. Anything
 * else - including sensor states like "open", which Alarmo puts in open_sensors -
 * returns NULL so the tile never prints an untranslated English word. */
static const char *alarm_sensor_kind_label(const char *kind)
{
    if (kind == NULL || kind[0] == '\0') {
        return NULL;
    }
    if (strcmp(kind, "door") == 0 || strcmp(kind, "garage_door") == 0) {
        return alarm_i18n("alarm.kind_door", "door");
    }
    if (strcmp(kind, "window") == 0) {
        return alarm_i18n("alarm.kind_window", "window");
    }
    if (strcmp(kind, "motion") == 0 || strcmp(kind, "moving") == 0 || strcmp(kind, "occupancy") == 0) {
        return alarm_i18n("alarm.kind_motion", "motion");
    }
    if (strcmp(kind, "smoke") == 0 || strcmp(kind, "gas") == 0 || strcmp(kind, "carbon_monoxide") == 0) {
        return alarm_i18n("alarm.kind_smoke", "smoke");
    }
    if (strcmp(kind, "moisture") == 0 || strcmp(kind, "water") == 0) {
        return alarm_i18n("alarm.kind_water", "water");
    }
    if (strcmp(kind, "tamper") == 0) {
        return alarm_i18n("alarm.kind_tamper", "tamper");
    }
    return NULL;
}

/* friendly_name when the entity is known, otherwise a readable entity_id.
 * kind_out (optional) receives the device class of the entity. */
static void alarm_sensor_name(const char *entity_id, char *out, size_t out_len, char *kind_out, size_t kind_len)
{
    if (out == NULL || out_len == 0) {
        return;
    }
    out[0] = '\0';
    if (kind_out != NULL && kind_len > 0) {
        kind_out[0] = '\0';
    }
    if (entity_id == NULL || entity_id[0] == '\0') {
        return;
    }

    ha_state_t state = {0};
    if (ha_model_get_state(entity_id, &state) && state.attributes_json[0] != '\0') {
        cJSON *attrs = cJSON_Parse(state.attributes_json);
        if (attrs != NULL) {
            const cJSON *friendly = cJSON_GetObjectItemCaseSensitive(attrs, "friendly_name");
            if (cJSON_IsString(friendly) && friendly->valuestring != NULL && friendly->valuestring[0] != '\0') {
                snprintf(out, out_len, "%s", friendly->valuestring);
            }
            if (kind_out != NULL && kind_len > 0) {
                const cJSON *device_class = cJSON_GetObjectItemCaseSensitive(attrs, "device_class");
                if (cJSON_IsString(device_class) && device_class->valuestring != NULL) {
                    snprintf(kind_out, kind_len, "%s", device_class->valuestring);
                }
            }
            cJSON_Delete(attrs);
        }
    }

    if (out[0] != '\0') {
        return;
    }
    const char *dot = strchr(entity_id, '.');
    const char *raw = (dot != NULL) ? (dot + 1) : entity_id;
    size_t i = 0;
    while (raw[i] != '\0' && (i + 1) < out_len) {
        out[i] = (raw[i] == '_') ? ' ' : raw[i];
        i++;
    }
    out[i] = '\0';
}

/* Fills sensors_text (rendered on the tile) plus open_count / bypassed_count. */
static void alarm_collect_sensors(w_alarm_tile_t *ctx, const cJSON *attrs)
{
    if (ctx == NULL || attrs == NULL) {
        return;
    }

    ctx->open_count = 0;
    ctx->bypassed_count = 0;
    ctx->sensors_text[0] = '\0';

    char names[W_ALARM_SENSORS_LISTED][APP_MAX_NAME_LEN];
    char kinds[W_ALARM_SENSORS_LISTED][24];
    int listed = 0;
    int total = 0;

    const cJSON *open = cJSON_GetObjectItemCaseSensitive((cJSON *)attrs, "open_sensors");
    if (cJSON_IsObject(open)) {
        for (const cJSON *item = open->child; item != NULL; item = item->next) {
            if (item->string == NULL || item->string[0] == '\0') {
                continue;
            }
            total++;
            if (listed < W_ALARM_SENSORS_LISTED) {
                alarm_sensor_name(item->string, names[listed], sizeof(names[listed]),
                    kinds[listed], sizeof(kinds[listed]));
                listed++;
            }
        }
    } else if (cJSON_IsArray(open)) {
        for (const cJSON *item = open->child; item != NULL; item = item->next) {
            if (!cJSON_IsString(item) || item->valuestring == NULL || item->valuestring[0] == '\0') {
                continue;
            }
            total++;
            if (listed < W_ALARM_SENSORS_LISTED) {
                alarm_sensor_name(item->valuestring, names[listed], sizeof(names[listed]),
                    kinds[listed], sizeof(kinds[listed]));
                listed++;
            }
        }
    }
    ctx->open_count = total;

    const cJSON *bypassed = cJSON_GetObjectItemCaseSensitive((cJSON *)attrs, "bypassed_sensors");
    if (cJSON_IsArray(bypassed)) {
        for (const cJSON *item = bypassed->child; item != NULL; item = item->next) {
            if (cJSON_IsString(item) && item->valuestring != NULL && item->valuestring[0] != '\0') {
                ctx->bypassed_count++;
            }
        }
    } else if (cJSON_IsObject(bypassed)) {
        for (const cJSON *item = bypassed->child; item != NULL; item = item->next) {
            if (item->string != NULL && item->string[0] != '\0') {
                ctx->bypassed_count++;
            }
        }
    }

    if (total == 0 || !ctx->show_sensors) {
        return;
    }

    const char *prefix = alarm_i18n("alarm.open_prefix", "Open");
    if (total == 1) {
        const char *kind = alarm_sensor_kind_label(kinds[0]);
        if (kind != NULL) {
            snprintf(ctx->sensors_text, sizeof(ctx->sensors_text), "%s: %s (%s)", prefix, names[0], kind);
        } else {
            snprintf(ctx->sensors_text, sizeof(ctx->sensors_text), "%s: %s", prefix, names[0]);
        }
        return;
    }

    alarm_text_appendf(ctx->sensors_text, sizeof(ctx->sensors_text), "%s (%d):", prefix, total);
    for (int i = 0; i < listed; i++) {
        alarm_text_appendf(ctx->sensors_text, sizeof(ctx->sensors_text), "%s %s", (i == 0) ? "" : ",", names[i]);
    }
    if (total > listed) {
        alarm_text_appendf(ctx->sensors_text, sizeof(ctx->sensors_text), " +%d", total - listed);
    }
}

static const lv_font_t *alarm_font_px(int px)
{
    if (px >= 28) {
        return APP_FONT_TEXT_28;
    }
    if (px >= 24) {
        return APP_FONT_TEXT_24;
    }
    if (px >= 22) {
        return APP_FONT_TEXT_22;
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

/* LVGL 9 dropped lv_label_set_max_lines(), so cap the label box instead: the
 * reserved height stops wrapped text from growing past the planned lines. */
static void alarm_label_set_max_lines(lv_obj_t *label, uint32_t max_lines)
{
    if (label == NULL || max_lines == 0) {
        return;
    }
    const lv_font_t *font = lv_obj_get_style_text_font(label, LV_PART_MAIN);
    const lv_coord_t line_h = (font != NULL && font->line_height > 0) ? (lv_coord_t)font->line_height : 16;
    lv_obj_set_height(label, line_h * (lv_coord_t)max_lines);
}

static const lv_font_t *alarm_fit_font(const char *text, int max_width, int max_height)
{
    static const int sizes[] = {28, 24, 22, 20, 18, 16, 14};
    const lv_font_t *result = alarm_font_px(14);

    for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
        const lv_font_t *font = alarm_font_px(sizes[i]);
        if (lv_font_get_line_height(font) > max_height) {
            continue;
        }
        lv_point_t size = {0, 0};
        lv_text_get_size(&size, text, font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
        if (size.x <= max_width && size.y <= max_height) {
            result = font;
            return result;
        }
    }
    return result;
}

static bool alarm_state_is_unavailable(const char *state)
{
    if (state == NULL) {
        return false;
    }
    return strcmp(state, "unavailable") == 0 || strcmp(state, "unknown") == 0;
}

/* "away,home,night" style list; an empty list falls back to the default set. */
static bool alarm_modes_has(const char *modes, const char *token)
{
    if (modes == NULL || modes[0] == '\0' || token == NULL) {
        return false;
    }

    const size_t token_len = strlen(token);
    const char *cursor = modes;
    while (cursor != NULL && cursor[0] != '\0') {
        while (cursor[0] == ' ' || cursor[0] == ',' || cursor[0] == ';') {
            cursor++;
        }
        const char *end = cursor;
        while (end[0] != '\0' && end[0] != ',' && end[0] != ';' && end[0] != ' ') {
            end++;
        }
        if ((size_t)(end - cursor) == token_len && strncmp(cursor, token, token_len) == 0) {
            return true;
        }
        cursor = (end[0] == '\0') ? NULL : end;
    }
    return false;
}

static const char *alarm_action_token(w_alarm_action_t action)
{
    switch (action) {
    case W_ALARM_ACTION_AWAY:
        return "away";
    case W_ALARM_ACTION_HOME:
        return "home";
    case W_ALARM_ACTION_NIGHT:
        return "night";
    case W_ALARM_ACTION_VACATION:
        return "vacation";
    case W_ALARM_ACTION_CUSTOM:
        return "custom";
    case W_ALARM_ACTION_DISARM:
    default:
        return "disarm";
    }
}

static const char *alarm_action_service(w_alarm_action_t action)
{
    switch (action) {
    case W_ALARM_ACTION_AWAY:
        return "alarm_arm_away";
    case W_ALARM_ACTION_HOME:
        return "alarm_arm_home";
    case W_ALARM_ACTION_NIGHT:
        return "alarm_arm_night";
    case W_ALARM_ACTION_VACATION:
        return "alarm_arm_vacation";
    case W_ALARM_ACTION_CUSTOM:
        return "alarm_arm_custom_bypass";
    case W_ALARM_ACTION_DISARM:
    default:
        return "alarm_disarm";
    }
}

static const char *alarm_action_label(w_alarm_action_t action)
{
    switch (action) {
    case W_ALARM_ACTION_AWAY:
        return alarm_i18n("alarm.btn_away", "Away");
    case W_ALARM_ACTION_HOME:
        return alarm_i18n("alarm.btn_home", "Home");
    case W_ALARM_ACTION_NIGHT:
        return alarm_i18n("alarm.btn_night", "Night");
    case W_ALARM_ACTION_VACATION:
        return alarm_i18n("alarm.btn_vacation", "Vacation");
    case W_ALARM_ACTION_CUSTOM:
        return alarm_i18n("alarm.btn_custom", "Custom");
    case W_ALARM_ACTION_DISARM:
    default:
        return alarm_i18n("alarm.btn_disarm", "Disarm");
    }
}

static uint32_t alarm_action_accent(w_alarm_action_t action)
{
    switch (action) {
    case W_ALARM_ACTION_DISARM:
        return 0x2ECC9A;
    case W_ALARM_ACTION_AWAY:
        return 0xFF5252;
    case W_ALARM_ACTION_NIGHT:
        return 0x7C6BFF;
    case W_ALARM_ACTION_VACATION:
        return 0x00BCD4;
    case W_ALARM_ACTION_CUSTOM:
        return 0x41BDF5;
    case W_ALARM_ACTION_HOME:
    default:
        return 0xFFB648;
    }
}

static void alarm_style_button(lv_obj_t *button, uint32_t bg, uint32_t border, uint32_t text, lv_opa_t opa)
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

static void alarm_render_status(w_alarm_tile_t *ctx)
{
    if (ctx == NULL || ctx->state_label == NULL) {
        return;
    }

    char text[W_ALARM_STATUS_TEXT_LEN];
    snprintf(text, sizeof(text), "%s", ctx->base_status);
    if (ctx->delay_active && ctx->delay_seconds > 0) {
        alarm_text_appendf(text, sizeof(text), "  %d s", ctx->delay_seconds);
    }

    if (strcmp(text, ctx->status_text) == 0) {
        return;
    }

    const size_t previous_len = ctx->status_len;
    snprintf(ctx->status_text, sizeof(ctx->status_text), "%s", text);
    ctx->status_len = strlen(ctx->status_text);
    lv_label_set_text(ctx->state_label, ctx->status_text);
    /* The fitted font depends on the text length, so relayout only when it changed. */
    if (ctx->status_len != previous_len) {
        alarm_layout(ctx);
    }
}

static void alarm_set_status(w_alarm_tile_t *ctx, const char *text, uint32_t accent)
{
    if (ctx == NULL || ctx->state_label == NULL) {
        return;
    }
    snprintf(ctx->base_status, sizeof(ctx->base_status), "%s", text != NULL ? text : "");
    ctx->last_accent = accent;
    ctx->have_status = true;

    lv_obj_set_style_text_color(ctx->state_label, lv_color_hex(accent), LV_PART_MAIN);
    if (ctx->dot != NULL) {
        lv_obj_set_style_bg_color(ctx->dot, lv_color_hex(accent), LV_PART_MAIN);
    }
    alarm_render_status(ctx);
}

/* Text for the info line: a fresh Alarmo event wins over the regular info
 * (changed_by / bypassed), which is restored once the event text expires. */
static const char *alarm_info_text(const w_alarm_tile_t *ctx)
{
    if (ctx->alarmo_text[0] != '\0' && ctx->alarmo_text_until_ms != 0) {
        return ctx->alarmo_text;
    }
    return (ctx->info_text[0] != '\0') ? ctx->info_text : NULL;
}

static void alarm_render_info(w_alarm_tile_t *ctx)
{
    if (ctx == NULL || ctx->info_label == NULL) {
        return;
    }
    const char *text = alarm_info_text(ctx);
    lv_label_set_text(ctx->info_label, text != NULL ? text : "");
    const bool is_event = (text != NULL && text == ctx->alarmo_text);
    lv_obj_set_style_text_color(ctx->info_label,
        lv_color_hex((is_event && ctx->alarmo_text_error) ? 0xFF5252 : APP_UI_COLOR_TEXT_SOFT), LV_PART_MAIN);
}

static void alarm_layout(w_alarm_tile_t *ctx)
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

    lv_coord_t row_h = 0;
    if (ctx->button_count > 0) {
        row_h = (lv_coord_t)(card_h / 5);
        if (row_h < 32) {
            row_h = 32;
        }
        if (row_h > 50) {
            row_h = 50;
        }
    }
    const lv_coord_t row_gap = 4;

    const lv_font_t *small_font = alarm_font_px(14);
    const lv_coord_t small_line = lv_font_get_line_height(small_font);
    const lv_coord_t text_w = card_w - (2 * pad);

    lv_coord_t header_h = 0;
    if (ctx->title_label != NULL) {
        header_h = (card_h >= 200) ? 24 : 20;
        lv_coord_t zone_w = 0;
        if (ctx->zone_obj != NULL) {
            if (ctx->zone_label[0] != '\0' && card_w >= 200) {
                lv_obj_clear_flag(ctx->zone_obj, LV_OBJ_FLAG_HIDDEN);
                lv_obj_set_style_text_font(ctx->zone_obj, alarm_font_px(12), LV_PART_MAIN);
                lv_obj_set_width(ctx->zone_obj, card_w / 3);
                lv_label_set_long_mode(ctx->zone_obj, LV_LABEL_LONG_DOT);
                lv_obj_align(ctx->zone_obj, LV_ALIGN_TOP_RIGHT, 0, 4);
                zone_w = (card_w / 3) + 6;
            } else {
                lv_obj_add_flag(ctx->zone_obj, LV_OBJ_FLAG_HIDDEN);
            }
        }
        lv_obj_set_style_text_font(ctx->title_label, alarm_font_px(header_h >= 24 ? 18 : 16), LV_PART_MAIN);
        lv_obj_set_width(ctx->title_label, text_w - (ctx->dot != NULL ? 16 : 0) - zone_w);
        lv_label_set_long_mode(ctx->title_label, LV_LABEL_LONG_DOT);
        lv_obj_align(ctx->title_label, LV_ALIGN_TOP_LEFT, ctx->dot != NULL ? 16 : 0, 0);
    }
    if (ctx->dot != NULL) {
        lv_obj_set_size(ctx->dot, 10, 10);
        lv_obj_align(ctx->dot, LV_ALIGN_TOP_LEFT, 1, (header_h - 10) / 2 + 2);
    }

    /* Bottom stack, from the buttons upwards: info line, then the sensors line. */
    const lv_coord_t stacked = pad + row_h + row_gap;
    lv_coord_t area_h = card_h - (pad + header_h) - stacked;

    lv_coord_t sensors_h = 0;
    int sensors_lines = 0;
    if (ctx->sensors_label != NULL && ctx->show_sensors && ctx->sensors_text[0] != '\0') {
        const int chars_per_line = (text_w > 18) ? (int)(text_w / 8) : 1;
        int lines = (int)((strlen(ctx->sensors_text) + (size_t)chars_per_line - 1) / (size_t)chars_per_line);
        const int max_lines = (card_h >= 190) ? 2 : 1;
        if (lines < 1) {
            lines = 1;
        }
        if (lines > max_lines) {
            lines = max_lines;
        }
        sensors_lines = lines;
        sensors_h = (small_line * lines) + 2;
    }

    lv_coord_t info_h = 0;
    if (ctx->info_label != NULL && alarm_info_text(ctx) != NULL) {
        info_h = small_line + 2;
    }

    /* Give the state text room first: drop the info line, then the sensors line. */
    if (info_h > 0 && (area_h - info_h - sensors_h) < 22) {
        info_h = 0;
    }
    if (sensors_h > 0 && (area_h - info_h - sensors_h) < 22) {
        sensors_h = 0;
        sensors_lines = 0;
    }
    area_h -= (info_h + sensors_h);
    if (area_h < 18) {
        area_h = 18;
    }

    if (ctx->sensors_label != NULL) {
        if (sensors_h > 0) {
            lv_obj_clear_flag(ctx->sensors_label, LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_style_text_font(ctx->sensors_label, small_font, LV_PART_MAIN);
            lv_obj_set_width(ctx->sensors_label, text_w);
            lv_label_set_long_mode(ctx->sensors_label, LV_LABEL_LONG_WRAP);
            alarm_label_set_max_lines(ctx->sensors_label, (uint32_t)sensors_lines);
            lv_obj_align(ctx->sensors_label, LV_ALIGN_BOTTOM_MID, 0, -stacked);
        } else {
            lv_obj_add_flag(ctx->sensors_label, LV_OBJ_FLAG_HIDDEN);
        }
    }

    if (ctx->info_label != NULL) {
        if (info_h > 0) {
            lv_obj_clear_flag(ctx->info_label, LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_style_text_font(ctx->info_label, small_font, LV_PART_MAIN);
            lv_obj_set_width(ctx->info_label, text_w);
            lv_label_set_long_mode(ctx->info_label, LV_LABEL_LONG_DOT);
            lv_obj_align(ctx->info_label, LV_ALIGN_BOTTOM_MID, 0, -(stacked + sensors_h));
        } else {
            lv_obj_add_flag(ctx->info_label, LV_OBJ_FLAG_HIDDEN);
        }
    }

    if (ctx->state_label != NULL) {
        const lv_coord_t top = pad + header_h;
        lv_obj_set_width(ctx->state_label, text_w);
        const lv_font_t *state_font = alarm_fit_font(ctx->status_text, text_w, area_h);
        lv_obj_set_style_text_font(ctx->state_label, state_font, LV_PART_MAIN);
        lv_obj_align(ctx->state_label, LV_ALIGN_TOP_MID, 0,
            top + (area_h - lv_font_get_line_height(state_font)) / 2);
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
            lv_obj_set_size(button, button_w, row_h);
            lv_obj_align(button, LV_ALIGN_BOTTOM_LEFT, pad + (i * (button_w + gap)), -pad);
            if (ctx->buttons[i].label != NULL) {
                lv_obj_set_style_text_font(ctx->buttons[i].label,
                    alarm_font_px(row_h >= 44 ? 16 : 14), LV_PART_MAIN);
            }
        }
    }
}

static void alarm_update_buttons(w_alarm_tile_t *ctx)
{
    if (ctx == NULL) {
        return;
    }

    const bool disabled = ctx->unavailable || ctx->busy;
    for (int i = 0; i < ctx->button_count; i++) {
        w_alarm_action_t action = ctx->buttons[i].action;
        const uint32_t accent = alarm_action_accent(action);
        const uint32_t bit = 1UL << (uint32_t)action;
        /* Alarmo told us this mode has blocking sensors: keep it usable (force
         * arm may still be wanted) but visibly dimmed. */
        const bool not_ready = (ctx->ready_known_mask & bit) != 0 && (ctx->ready_mask & bit) == 0;
        const bool dim = disabled || not_ready;
        const lv_opa_t opa = disabled ? LV_OPA_50 : (not_ready ? LV_OPA_60 : LV_OPA_COVER);
        uint32_t text_color = APP_UI_COLOR_TEXT_PRIMARY;
        if (action == W_ALARM_ACTION_DISARM) {
            alarm_style_button(ctx->buttons[i].button, disabled ? APP_UI_COLOR_NAV_BTN_BG_IDLE : 0x123A2C,
                accent, accent, opa);
            text_color = dim ? APP_UI_COLOR_TEXT_MUTED : accent;
        } else {
            alarm_style_button(ctx->buttons[i].button, APP_UI_COLOR_NAV_BTN_BG_IDLE,
                dim ? APP_UI_COLOR_CARD_BORDER : accent,
                APP_UI_COLOR_TEXT_PRIMARY, opa);
            text_color = dim ? APP_UI_COLOR_TEXT_MUTED : APP_UI_COLOR_TEXT_PRIMARY;
        }
        if (ctx->buttons[i].label != NULL) {
            lv_obj_set_style_text_color(ctx->buttons[i].label, lv_color_hex(text_color), LV_PART_MAIN);
        }
    }
}

static void alarm_send(w_alarm_tile_t *ctx, w_alarm_action_t action, const char *code, bool force)
{
    if (ctx == NULL) {
        return;
    }

    const bool has_code = (code != NULL && code[0] != '\0');
    char payload[APP_MAX_ENTITY_ID_LEN + APP_MAX_ALARM_CODE_LEN + 96];
    const char *domain = "alarm_control_panel";
    const char *service = alarm_action_service(action);

    if (ctx->use_alarmo) {
        /* Alarmo: alarmo.arm with a mode, alarmo.disarm without one. */
        domain = "alarmo";
        if (action == W_ALARM_ACTION_DISARM) {
            service = "disarm";
            if (has_code) {
                snprintf(payload, sizeof(payload), "{\"entity_id\":\"%s\",\"code\":\"%s\"}",
                    ctx->entity_id, code);
            } else {
                snprintf(payload, sizeof(payload), "{\"entity_id\":\"%s\"}", ctx->entity_id);
            }
        } else {
            service = "arm";
            char options[64];
            options[0] = '\0';
            if (force) {
                snprintf(options, sizeof(options), ",\"force\":true");
            }
            if (ctx->skip_delay) {
                alarm_text_appendf(options, sizeof(options), ",\"skip_delay\":true");
            }
            if (has_code) {
                snprintf(payload, sizeof(payload),
                    "{\"entity_id\":\"%s\",\"mode\":\"%s\"%s,\"code\":\"%s\"}",
                    ctx->entity_id, alarm_action_token(action), options, code);
            } else {
                snprintf(payload, sizeof(payload), "{\"entity_id\":\"%s\",\"mode\":\"%s\"%s}",
                    ctx->entity_id, alarm_action_token(action), options);
            }
        }
    } else if (has_code) {
        snprintf(payload, sizeof(payload), "{\"entity_id\":\"%s\",\"code\":\"%s\"}", ctx->entity_id, code);
    } else {
        snprintf(payload, sizeof(payload), "{\"entity_id\":\"%s\"}", ctx->entity_id);
    }

    esp_err_t err = ha_client_call_service(domain, service, payload);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "%s.%s failed for %s: %s", domain, service, ctx->entity_id, esp_err_to_name(err));
        alarm_set_status(ctx, alarm_i18n("alarm.send_failed", "Command failed"), 0xFF5252);
        alarm_layout(ctx);
        return;
    }

    ESP_LOGI(TAG, "%s.%s -> %s%s", domain, service, ctx->entity_id, force ? " (force)" : "");
    ctx->busy = true;
    ctx->busy_until_ms = lv_tick_get() + W_ALARM_BUSY_TIMEOUT_MS;
    alarm_set_status(ctx, alarm_i18n("alarm.busy", "Sending..."), 0x41BDF5);
    alarm_update_buttons(ctx);
    alarm_layout(ctx);
}

static void alarm_keypad_close(w_alarm_tile_t *ctx);
static void alarm_keypad_open(w_alarm_tile_t *ctx, w_alarm_action_t action);
static void alarm_confirm_close(w_alarm_tile_t *ctx);
static void alarm_confirm_open(w_alarm_tile_t *ctx, w_alarm_action_t action, const char *code);

/* Sends the command, or asks for confirmation when open sensors would block arming. */
static void alarm_dispatch(w_alarm_tile_t *ctx, w_alarm_action_t action, const char *code)
{
    if (ctx == NULL) {
        return;
    }
    if (action != W_ALARM_ACTION_DISARM && ctx->force_arm && ctx->open_count > 0) {
        alarm_confirm_open(ctx, action, code);
        return;
    }
    alarm_send(ctx, action, code, false);
}

/* Forwards the typed code to the action the keypad was opened for. */
static void alarm_keypad_confirm(w_alarm_tile_t *ctx)
{
    if (ctx == NULL) {
        return;
    }
    const w_alarm_action_t action = ctx->pending_action;
    char code[APP_MAX_ALARM_CODE_LEN];
    snprintf(code, sizeof(code), "%s", ctx->code_entry);
    alarm_keypad_close(ctx);
    if (code[0] == '\0' && ctx->code[0] != '\0') {
        /* Keypad confirmed empty - fall back to the stored code. */
        snprintf(code, sizeof(code), "%s", ctx->code);
    }
    alarm_dispatch(ctx, action, code);
}

/* The keypad is shown when the layout asks for it (alarm_ask_code), when HA
 * wants a code and none is stored, and for arming whenever code_arm_required
 * says so. Without those the action is sent straight away. */
static bool alarm_needs_keypad(const w_alarm_tile_t *ctx, w_alarm_action_t action)
{
    if (ctx->ask_code) {
        return true;
    }
    if (ctx->code[0] != '\0') {
        return false;
    }
    if (!ctx->code_format_present) {
        return false;
    }
    if (action == W_ALARM_ACTION_DISARM) {
        return true;
    }
    return ctx->code_arm_required;
}

static void alarm_request_action(w_alarm_tile_t *ctx, w_alarm_action_t action)
{
    if (ctx == NULL || ctx->unavailable || ctx->busy) {
        return;
    }
    if (alarm_needs_keypad(ctx, action)) {
        alarm_keypad_open(ctx, action);
        return;
    }
    alarm_dispatch(ctx, action, ctx->code);
}

static void alarm_button_event_cb(lv_event_t *event)
{
    w_alarm_tile_t *ctx = (w_alarm_tile_t *)lv_event_get_user_data(event);
    lv_obj_t *button = lv_event_get_target(event);
    if (ctx == NULL || button == NULL) {
        return;
    }
    alarm_request_action(ctx, (w_alarm_action_t)(intptr_t)lv_obj_get_user_data(button));
}

/* Alarmo mode token for the ready_to_arm_modes_updated payload; NULL for
 * actions Alarmo does not report readiness for. */
static const char *alarm_ready_mode_token(w_alarm_action_t action)
{
    switch (action) {
    case W_ALARM_ACTION_AWAY:
        return "away";
    case W_ALARM_ACTION_HOME:
        return "home";
    case W_ALARM_ACTION_NIGHT:
        return "night";
    case W_ALARM_ACTION_VACATION:
        return "vacation";
    case W_ALARM_ACTION_CUSTOM:
        return "custom_bypass";
    default:
        return NULL;
    }
}

/* Shows why an arming was refused. Alarmo never writes that into the entity
 * attributes - the bus event is the only source. */
static void alarm_show_failed_arm(w_alarm_tile_t *ctx, const cJSON *data)
{
    const cJSON *reason = cJSON_GetObjectItemCaseSensitive((cJSON *)data, "reason");
    const char *reason_str = (cJSON_IsString(reason) && reason->valuestring != NULL) ? reason->valuestring : "";
    char detail[W_ALARM_SENSORS_TEXT_LEN];
    detail[0] = '\0';

    if (strcmp(reason_str, "open_sensors") == 0) {
        const cJSON *sensors = cJSON_GetObjectItemCaseSensitive((cJSON *)data, "sensors");
        char names[W_ALARM_SENSORS_LISTED][APP_MAX_NAME_LEN];
        int stored = 0;
        int total = 0;
        if (cJSON_IsArray(sensors)) {
            for (const cJSON *item = sensors->child; item != NULL; item = item->next) {
                if (!cJSON_IsString(item) || item->valuestring == NULL || item->valuestring[0] == '\0') {
                    continue;
                }
                total++;
                if (stored < W_ALARM_SENSORS_LISTED) {
                    alarm_sensor_name(item->valuestring, names[stored], sizeof(names[stored]), NULL, 0);
                    stored++;
                }
            }
        }
        if (total == 0) {
            snprintf(detail, sizeof(detail), "%s", alarm_i18n("alarm.reason_open_sensors", "open sensors"));
        } else {
            for (int i = 0; i < stored; i++) {
                alarm_text_appendf(detail, sizeof(detail), "%s%s", (i == 0) ? "" : ", ", names[i]);
            }
            if (total > stored) {
                alarm_text_appendf(detail, sizeof(detail), " +%d", total - stored);
            }
        }
    } else if (strcmp(reason_str, "invalid_code") == 0) {
        snprintf(detail, sizeof(detail), "%s", alarm_i18n("alarm.reason_invalid_code", "invalid code"));
    } else if (strcmp(reason_str, "not_allowed") == 0) {
        snprintf(detail, sizeof(detail), "%s", alarm_i18n("alarm.reason_not_allowed", "not allowed now"));
    } else {
        snprintf(detail, sizeof(detail), "%s", reason_str);
    }

    const char *title = alarm_i18n("alarm.failed_arm", "Arming failed");
    if (detail[0] != '\0') {
        /* This line is truncated on purpose, but detail is a full alarmo_text sized
         * array, so gcc treats a plain "%s: %s" as losing the ": " separator and warns
         * (-Werror=format-truncation, fatal in debug builds). Going through the helper
         * keeps the build time format analysis out of the way. */
        snprintf(ctx->alarmo_text, sizeof(ctx->alarmo_text), "%s", title);
        alarm_text_appendf(ctx->alarmo_text, sizeof(ctx->alarmo_text), ": %s", detail);
    } else {
        snprintf(ctx->alarmo_text, sizeof(ctx->alarmo_text), "%s", title);
    }
    ctx->alarmo_text_error = true;
    ctx->alarmo_text_until_ms = lv_tick_get() + W_ALARM_EVENT_TEXT_MS;

    /* Red status for a moment; the previous text returns on expiry, because a
     * refused command often does not change the entity state at all. */
    if (ctx->alarmo_status_until_ms == 0) {
        snprintf(ctx->alarmo_prev_status, sizeof(ctx->alarmo_prev_status), "%s", ctx->base_status);
        ctx->alarmo_prev_accent = ctx->last_accent;
    }
    ctx->alarmo_status_until_ms = lv_tick_get() + W_ALARM_EVENT_TEXT_MS;

    ctx->busy = false;
    ESP_LOGW(TAG, "%s: %s (reason=%s)", ctx->entity_id, ctx->alarmo_text,
        (reason_str[0] != '\0') ? reason_str : "?");
    alarm_set_status(ctx, title, 0xFF5252);
    alarm_render_info(ctx);
    alarm_update_buttons(ctx);
    alarm_layout(ctx);
}

static void alarm_apply_ready_modes(w_alarm_tile_t *ctx, const cJSON *data)
{
    uint32_t known = 0;
    uint32_t ready = 0;
    for (int action = 0; action < W_ALARM_ACTION_COUNT; action++) {
        const char *token = alarm_ready_mode_token((w_alarm_action_t)action);
        if (token == NULL) {
            continue;
        }
        const cJSON *item = cJSON_GetObjectItemCaseSensitive((cJSON *)data, token);
        if (!cJSON_IsBool(item)) {
            continue;
        }
        known |= 1UL << (uint32_t)action;
        if (cJSON_IsTrue(item)) {
            ready |= 1UL << (uint32_t)action;
        }
    }
    if (known == 0 || (known == ctx->ready_known_mask && ready == ctx->ready_mask)) {
        return;
    }
    ctx->ready_known_mask = known;
    ctx->ready_mask = ready;
    alarm_update_buttons(ctx);
}

static void alarm_handle_alarmo_event(w_alarm_tile_t *ctx, const ha_alarm_event_t *event)
{
    cJSON *data = cJSON_Parse(event->payload);
    if (data == NULL) {
        return;
    }

    const cJSON *entity = cJSON_GetObjectItemCaseSensitive(data, "entity_id");
    if (cJSON_IsString(entity) && entity->valuestring != NULL && strcmp(entity->valuestring, ctx->entity_id) != 0) {
        /* Another zone of the same Alarmo instance - not our tile. */
        cJSON_Delete(data);
        return;
    }

    if (strcmp(event->type, "alarmo_failed_to_arm") == 0) {
        alarm_show_failed_arm(ctx, data);
    } else if (strcmp(event->type, "alarmo_command_success") == 0) {
        const cJSON *action = cJSON_GetObjectItemCaseSensitive(data, "action");
        ESP_LOGI(TAG, "%s: command accepted (%s)", ctx->entity_id,
            (cJSON_IsString(action) && action->valuestring != NULL) ? action->valuestring : "?");
        /* Accepted: stop waiting right away instead of after the timeout. */
        if (ctx->busy) {
            ctx->busy = false;
            alarm_update_buttons(ctx);
        }
        ctx->ready_known_mask = 0;
        ctx->ready_mask = 0;
    } else if (strcmp(event->type, "alarmo_ready_to_arm_modes_updated") == 0) {
        alarm_apply_ready_modes(ctx, data);
    }

    cJSON_Delete(data);
}

/* Drains the shared Alarmo event slot - one atomic compare when nothing new. */
static void alarm_poll_events(w_alarm_tile_t *ctx)
{
    ha_alarm_event_t event;
    while (ha_alarm_events_take(&ctx->alarmo_seq, &event)) {
        alarm_handle_alarmo_event(ctx, &event);
    }
}

static void alarm_timer_cb(lv_timer_t *timer)
{
    system_log_note_lvgl_cb("alarm_timer_cb");
    w_alarm_tile_t *ctx = (w_alarm_tile_t *)lv_timer_get_user_data(timer);
    if (ctx == NULL) {
        return;
    }

    alarm_poll_events(ctx);

    if (ctx->busy && (int32_t)(lv_tick_get() - ctx->busy_until_ms) >= 0) {
        ctx->busy = false;
        if (ctx->have_status) {
            alarm_set_status(ctx, ctx->base_status, ctx->last_accent);
        }
        alarm_update_buttons(ctx);
        alarm_layout(ctx);
    }

    if (ctx->alarmo_status_until_ms != 0 && (int32_t)(lv_tick_get() - ctx->alarmo_status_until_ms) >= 0) {
        ctx->alarmo_status_until_ms = 0;
        if (ctx->have_status) {
            alarm_set_status(ctx, ctx->alarmo_prev_status, ctx->alarmo_prev_accent);
        }
    }

    if (ctx->alarmo_text_until_ms != 0 && (int32_t)(lv_tick_get() - ctx->alarmo_text_until_ms) >= 0) {
        ctx->alarmo_text_until_ms = 0;
        alarm_render_info(ctx);
        alarm_layout(ctx);
    }

    /* Exit / entry delay countdown driven by the "delay" attribute. */
    if (ctx->delay_active) {
        const int32_t elapsed = (int32_t)((lv_tick_get() - ctx->delay_anchor_ms) / 1000U);
        int32_t remaining = (int32_t)ctx->delay_reported - elapsed;
        if (remaining < 0) {
            remaining = 0;
        }
        if (remaining != ctx->delay_seconds) {
            ctx->delay_seconds = (int)remaining;
            alarm_render_status(ctx);
        }
    }
}

static void alarm_card_event_cb(lv_event_t *event)
{
    const lv_event_code_t code = lv_event_get_code(event);
    w_alarm_tile_t *ctx = (w_alarm_tile_t *)lv_event_get_user_data(event);
    if (ctx == NULL) {
        return;
    }

    if (code == LV_EVENT_SIZE_CHANGED) {
        alarm_layout(ctx);
        return;
    }

    if (code == LV_EVENT_DELETE) {
        if (ctx->overlay != NULL) {
            lv_obj_del(ctx->overlay);
            ctx->overlay = NULL;
        }
        if (ctx->confirm_overlay != NULL) {
            lv_obj_del(ctx->confirm_overlay);
            ctx->confirm_overlay = NULL;
        }
        if (s_keypad_owner == ctx) {
            s_keypad_owner = NULL;
        }
        if (ctx->timer != NULL) {
            lv_timer_del(ctx->timer);
            ctx->timer = NULL;
        }
        free(ctx);
    }
}

static void alarm_update_code_display(w_alarm_tile_t *ctx)
{
    if (ctx == NULL || ctx->code_label == NULL) {
        return;
    }

    char masked[APP_MAX_ALARM_CODE_LEN * 2 + 8] = {0};
    size_t len = strlen(ctx->code_entry);
    if (len == 0) {
        snprintf(masked, sizeof(masked), "%s", alarm_i18n("alarm.code_hint", "----"));
    } else {
        size_t out = 0;
        for (size_t i = 0; i < len && out + 2 < sizeof(masked); i++) {
            masked[out++] = '*';
            masked[out++] = ' ';
        }
        if (out > 0) {
            masked[out - 1] = '\0';
        }
    }
    lv_label_set_text(ctx->code_label, masked);
}

static void alarm_keypad_key_cb(lv_event_t *event)
{
    w_alarm_tile_t *ctx = (w_alarm_tile_t *)lv_event_get_user_data(event);
    lv_obj_t *button = lv_event_get_target(event);
    if (ctx == NULL || button == NULL) {
        return;
    }

    const int key = (int)(intptr_t)lv_obj_get_user_data(button);
    const size_t len = strlen(ctx->code_entry);

    if (key >= 0 && key <= 9) {
        if (len + 1 < sizeof(ctx->code_entry)) {
            ctx->code_entry[len] = (char)('0' + key);
            ctx->code_entry[len + 1] = '\0';
            alarm_update_code_display(ctx);
        }
        return;
    }

    if (key == 10) { /* backspace */
        if (len > 0) {
            ctx->code_entry[len - 1] = '\0';
            alarm_update_code_display(ctx);
        }
        return;
    }

    if (key == 11) { /* confirm */
        alarm_keypad_confirm(ctx);
        return;
    }

    alarm_keypad_close(ctx);
}

static void alarm_keypad_overlay_event_cb(lv_event_t *event)
{
    w_alarm_tile_t *ctx = (w_alarm_tile_t *)lv_event_get_user_data(event);
    if (ctx == NULL) {
        return;
    }
    const lv_event_code_t code = lv_event_get_code(event);
    if (code == LV_EVENT_DELETE) {
        if (ctx->overlay == lv_event_get_target(event)) {
            ctx->overlay = NULL;
            ctx->code_label = NULL;
            ctx->hint_label = NULL;
        }
        if (s_keypad_owner == ctx) {
            s_keypad_owner = NULL;
        }
        return;
    }
    if (code == LV_EVENT_CLICKED && lv_event_get_target(event) == ctx->overlay) {
        /* Tapping the dimmed backdrop cancels. */
        alarm_keypad_close(ctx);
    }
}

static lv_obj_t *alarm_keypad_make_key(lv_obj_t *parent, const char *text, int key, uint32_t bg, uint32_t text_color,
                                       w_alarm_tile_t *ctx)
{
    lv_obj_t *button = lv_btn_create(parent);
    lv_obj_clear_flag(button, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_user_data(button, (void *)(intptr_t)key);
    alarm_style_button(button, bg, APP_UI_COLOR_CARD_BORDER, text_color, LV_OPA_COVER);
    lv_obj_add_event_cb(button, alarm_keypad_key_cb, LV_EVENT_CLICKED, ctx);

    lv_obj_t *label = lv_label_create(button);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, APP_FONT_TEXT_22, LV_PART_MAIN);
    lv_obj_set_style_text_color(label, lv_color_hex(text_color), LV_PART_MAIN);
    lv_obj_center(label);
    return button;
}

static void alarm_keypad_close(w_alarm_tile_t *ctx)
{
    if (ctx == NULL) {
        return;
    }
    if (ctx->overlay != NULL) {
        lv_obj_del(ctx->overlay);
        ctx->overlay = NULL;
        ctx->code_label = NULL;
        ctx->hint_label = NULL;
    }
    if (s_keypad_owner == ctx) {
        s_keypad_owner = NULL;
    }
}

static void alarm_keypad_open(w_alarm_tile_t *ctx, w_alarm_action_t action)
{
    if (ctx == NULL || ctx->overlay != NULL || ctx->unavailable) {
        return;
    }
    if (s_keypad_owner != NULL && s_keypad_owner != ctx) {
        return;
    }

    lv_obj_t *screen = lv_scr_act();
    lv_obj_update_layout(screen);
    int screen_w = lv_obj_get_width(screen);
    int screen_h = lv_obj_get_height(screen);
    if (screen_w <= 0) {
        screen_w = 480;
    }
    if (screen_h <= 0) {
        screen_h = 480;
    }

    ctx->pending_action = action;
    memset(ctx->code_entry, 0, sizeof(ctx->code_entry));

    lv_obj_t *overlay = lv_obj_create(lv_layer_top());
    if (overlay == NULL) {
        return;
    }
    ctx->overlay = overlay;
    s_keypad_owner = ctx;
    lv_obj_set_size(overlay, screen_w, screen_h);
    lv_obj_align(overlay, LV_ALIGN_CENTER, 0, 0);
    lv_obj_clear_flag(overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(overlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(overlay, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(overlay, LV_OPA_60, LV_PART_MAIN);
    lv_obj_set_style_border_width(overlay, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(overlay, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(overlay, 0, LV_PART_MAIN);
    lv_obj_add_event_cb(overlay, alarm_keypad_overlay_event_cb, LV_EVENT_DELETE, ctx);
    lv_obj_add_event_cb(overlay, alarm_keypad_overlay_event_cb, LV_EVENT_CLICKED, ctx);

    int panel_w = 320;
    if (panel_w > screen_w - 20) {
        panel_w = screen_w - 20;
    }
    int panel_h = 430;
    if (panel_h > screen_h - 20) {
        panel_h = screen_h - 20;
    }

    lv_obj_t *panel = lv_obj_create(overlay);
    lv_obj_set_size(panel, panel_w, panel_h);
    lv_obj_center(panel);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_set_style_radius(panel, 16, LV_PART_MAIN);
    lv_obj_set_style_bg_color(panel, lv_color_hex(APP_UI_COLOR_CONTENT_BG), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(panel, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(panel, lv_color_hex(APP_UI_COLOR_CONTENT_BORDER), LV_PART_MAIN);
    lv_obj_set_style_pad_all(panel, 14, LV_PART_MAIN);

    lv_obj_t *title = lv_label_create(panel);
    lv_label_set_text(title, alarm_i18n("alarm.code_title", "Enter code"));
    lv_obj_set_style_text_font(title, APP_FONT_TEXT_20, LV_PART_MAIN);
    lv_obj_set_style_text_color(title, lv_color_hex(APP_UI_COLOR_TEXT_PRIMARY), LV_PART_MAIN);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 0, 0);

    lv_obj_t *close = lv_btn_create(panel);
    lv_obj_set_size(close, 38, 38);
    lv_obj_set_user_data(close, (void *)(intptr_t)12);
    alarm_style_button(close, APP_UI_COLOR_NAV_BTN_BG_IDLE, APP_UI_COLOR_CARD_BORDER, APP_UI_COLOR_TEXT_PRIMARY,
                       LV_OPA_COVER);
    lv_obj_align(close, LV_ALIGN_TOP_RIGHT, 0, -2);
    lv_obj_add_event_cb(close, alarm_keypad_key_cb, LV_EVENT_CLICKED, ctx);
    lv_obj_t *close_label = lv_label_create(close);
    lv_label_set_text(close_label, "X");
    lv_obj_set_style_text_font(close_label, APP_FONT_TEXT_16, LV_PART_MAIN);
    lv_obj_set_style_text_color(close_label, lv_color_hex(APP_UI_COLOR_TEXT_PRIMARY), LV_PART_MAIN);
    lv_obj_center(close_label);

    ctx->hint_label = lv_label_create(panel);
    lv_label_set_text(ctx->hint_label, alarm_action_label(action));
    lv_obj_set_style_text_font(ctx->hint_label, APP_FONT_TEXT_16, LV_PART_MAIN);
    lv_obj_set_style_text_color(ctx->hint_label, lv_color_hex(alarm_action_accent(action)), LV_PART_MAIN);
    lv_obj_align(ctx->hint_label, LV_ALIGN_TOP_LEFT, 0, 30);

    ctx->code_label = lv_label_create(panel);
    lv_obj_set_style_text_font(ctx->code_label, APP_FONT_TEXT_28, LV_PART_MAIN);
    lv_obj_set_style_text_color(ctx->code_label, lv_color_hex(APP_UI_COLOR_TEXT_PRIMARY), LV_PART_MAIN);
    lv_obj_align(ctx->code_label, LV_ALIGN_TOP_MID, 0, 62);
    alarm_update_code_display(ctx);

    static const char *key_texts[12] = {"1", "2", "3", "4", "5", "6", "7", "8", "9", "<", "0", "OK"};
    const int cols = 3;
    const int rows = 4;
    const int gap = 8;
    const int grid_w = panel_w - 28;
    const int key_w = (grid_w - (gap * (cols - 1))) / cols;
    const int key_h = 58;
    const int grid_y = 112;

    for (int index = 0; index < cols * rows; index++) {
        const int row = index / cols;
        const int col = index % cols;
        const char *text = key_texts[index];
        uint32_t bg = APP_UI_COLOR_NAV_BTN_BG_IDLE;
        uint32_t text_color = APP_UI_COLOR_TEXT_PRIMARY;
        if (index == 9) {
            bg = 0x3A1418;
            text_color = 0xFF8A8A;
        } else if (index == 11) {
            bg = 0x123A2C;
            text_color = 0x2ECC9A;
        }
        lv_obj_t *button = alarm_keypad_make_key(panel, text, (index == 9) ? 10 : ((index == 11) ? 11 : (text[0] - '0')),
                                                 bg, text_color, ctx);
        lv_obj_set_size(button, key_w, key_h);
        lv_obj_align(button, LV_ALIGN_TOP_LEFT, col * (key_w + gap), grid_y + (row * (key_h + gap)));
    }
}

static void alarm_confirm_close(w_alarm_tile_t *ctx)
{
    if (ctx == NULL) {
        return;
    }
    if (ctx->confirm_overlay != NULL) {
        lv_obj_del(ctx->confirm_overlay);
        ctx->confirm_overlay = NULL;
    }
}

static void alarm_confirm_overlay_event_cb(lv_event_t *event)
{
    w_alarm_tile_t *ctx = (w_alarm_tile_t *)lv_event_get_user_data(event);
    if (ctx == NULL) {
        return;
    }
    const lv_event_code_t code = lv_event_get_code(event);
    if (code == LV_EVENT_DELETE) {
        if (ctx->confirm_overlay == lv_event_get_target(event)) {
            ctx->confirm_overlay = NULL;
        }
        return;
    }
    if (code == LV_EVENT_CLICKED && lv_event_get_target(event) == ctx->confirm_overlay) {
        alarm_confirm_close(ctx);
    }
}

static void alarm_confirm_button_cb(lv_event_t *event)
{
    w_alarm_tile_t *ctx = (w_alarm_tile_t *)lv_event_get_user_data(event);
    lv_obj_t *button = lv_event_get_target(event);
    if (ctx == NULL || button == NULL) {
        return;
    }
    const bool confirmed = ((intptr_t)lv_obj_get_user_data(button) != 0);
    const w_alarm_action_t action = ctx->confirm_action;
    char code[APP_MAX_ALARM_CODE_LEN];
    snprintf(code, sizeof(code), "%s", ctx->confirm_code);
    alarm_confirm_close(ctx);
    if (confirmed) {
        /* Forced arm - Alarmo bypasses the open sensors when force=true. */
        alarm_send(ctx, action, code, true);
    }
}

static void alarm_confirm_open(w_alarm_tile_t *ctx, w_alarm_action_t action, const char *code)
{
    if (ctx == NULL || ctx->confirm_overlay != NULL || ctx->unavailable) {
        return;
    }

    lv_obj_t *screen = lv_scr_act();
    lv_obj_update_layout(screen);
    int screen_w = lv_obj_get_width(screen);
    int screen_h = lv_obj_get_height(screen);
    if (screen_w <= 0) {
        screen_w = 480;
    }
    if (screen_h <= 0) {
        screen_h = 480;
    }

    ctx->confirm_action = action;
    snprintf(ctx->confirm_code, sizeof(ctx->confirm_code), "%s", code != NULL ? code : "");

    lv_obj_t *overlay = lv_obj_create(lv_layer_top());
    if (overlay == NULL) {
        return;
    }
    ctx->confirm_overlay = overlay;
    lv_obj_set_size(overlay, screen_w, screen_h);
    lv_obj_align(overlay, LV_ALIGN_CENTER, 0, 0);
    lv_obj_clear_flag(overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(overlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(overlay, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(overlay, LV_OPA_60, LV_PART_MAIN);
    lv_obj_set_style_border_width(overlay, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(overlay, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(overlay, 0, LV_PART_MAIN);
    lv_obj_add_event_cb(overlay, alarm_confirm_overlay_event_cb, LV_EVENT_DELETE, ctx);
    lv_obj_add_event_cb(overlay, alarm_confirm_overlay_event_cb, LV_EVENT_CLICKED, ctx);

    int panel_w = 340;
    if (panel_w > screen_w - 20) {
        panel_w = screen_w - 20;
    }
    int panel_h = 220;
    if (panel_h > screen_h - 20) {
        panel_h = screen_h - 20;
    }

    lv_obj_t *panel = lv_obj_create(overlay);
    lv_obj_set_size(panel, panel_w, panel_h);
    lv_obj_center(panel);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_set_style_radius(panel, 16, LV_PART_MAIN);
    lv_obj_set_style_bg_color(panel, lv_color_hex(APP_UI_COLOR_CONTENT_BG), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(panel, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(panel, lv_color_hex(APP_UI_COLOR_CONTENT_BORDER), LV_PART_MAIN);
    lv_obj_set_style_pad_all(panel, 16, LV_PART_MAIN);

    lv_obj_t *title = lv_label_create(panel);
    lv_label_set_text(title, alarm_i18n("alarm.force_title", "Sensors are open"));
    lv_obj_set_style_text_font(title, APP_FONT_TEXT_20, LV_PART_MAIN);
    lv_obj_set_style_text_color(title, lv_color_hex(0xFFB648), LV_PART_MAIN);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 0);

    char hint_text[W_ALARM_SENSORS_TEXT_LEN + 64];
    snprintf(hint_text, sizeof(hint_text), "%s", alarm_i18n("alarm.force_hint", "Arm anyway?"));
    if (ctx->sensors_text[0] != '\0') {
        alarm_text_appendf(hint_text, sizeof(hint_text), "\n%s", ctx->sensors_text);
    }
    lv_obj_t *hint = lv_label_create(panel);
    lv_label_set_text(hint, hint_text);
    lv_obj_set_style_text_font(hint, alarm_font_px(14), LV_PART_MAIN);
    lv_obj_set_style_text_color(hint, lv_color_hex(APP_UI_COLOR_TEXT_MUTED), LV_PART_MAIN);
    lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_width(hint, panel_w - 32);
    lv_label_set_long_mode(hint, LV_LABEL_LONG_WRAP);
    alarm_label_set_max_lines(hint, 3);
    lv_obj_align(hint, LV_ALIGN_TOP_MID, 0, 32);

    struct {
        const char *key;
        const char *fallback;
        uint32_t bg;
        uint32_t fg;
        bool confirm;
    } items[2] = {
        {"alarm.force_confirm", "Arm anyway", 0x3A2410, 0xFFB648, true},
        {"alarm.cancel", "Cancel", APP_UI_COLOR_NAV_BTN_BG_IDLE, APP_UI_COLOR_TEXT_PRIMARY, false},
    };
    const int gap = 10;
    const int button_w = (panel_w - 32 - gap) / 2;
    const int button_h = 46;
    for (int i = 0; i < 2; i++) {
        lv_obj_t *button = lv_btn_create(panel);
        lv_obj_clear_flag(button, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_user_data(button, (void *)(intptr_t)(items[i].confirm ? 1 : 0));
        alarm_style_button(button, items[i].bg, APP_UI_COLOR_CARD_BORDER, items[i].fg, LV_OPA_COVER);
        lv_obj_set_size(button, button_w, button_h);
        lv_obj_align(button, LV_ALIGN_BOTTOM_LEFT, i * (button_w + gap), 0);
        lv_obj_add_event_cb(button, alarm_confirm_button_cb, LV_EVENT_CLICKED, ctx);

        lv_obj_t *label = lv_label_create(button);
        lv_label_set_text(label, alarm_i18n(items[i].key, items[i].fallback));
        lv_obj_set_style_text_font(label, alarm_font_px(16), LV_PART_MAIN);
        lv_obj_set_style_text_color(label, lv_color_hex(items[i].fg), LV_PART_MAIN);
        lv_obj_center(label);
    }
}

esp_err_t w_alarm_tile_create(const ui_widget_def_t *def, lv_obj_t *parent, ui_widget_instance_t *out_instance)
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
    lv_obj_set_style_border_color(card, lv_color_hex(APP_UI_COLOR_CARD_BORDER), LV_PART_MAIN);
    lv_obj_set_style_pad_all(card, 10, LV_PART_MAIN);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    /* set_pos/set_size must come after remove_style_all() - see w_binary_sensor. */
    lv_obj_set_pos(card, def->x, def->y);
    lv_obj_set_size(card, def->w, def->h);

    w_alarm_tile_t *ctx = (w_alarm_tile_t *)ui_calloc_prefer_psram(1, sizeof(w_alarm_tile_t));
    if (ctx == NULL) {
        lv_obj_del(card);
        return ESP_ERR_NO_MEM;
    }
    ctx->card = card;
    snprintf(ctx->entity_id, sizeof(ctx->entity_id), "%s", def->entity_id);
    snprintf(ctx->code, sizeof(ctx->code), "%s", def->alarm_code);
    ctx->ask_code = def->alarm_ask_code;
    ctx->unavailable = true;
    ctx->last_accent = APP_UI_COLOR_TEXT_MUTED;

    ctx->show_sensors = def->alarm_show_sensors;
    ctx->show_bypassed = def->alarm_show_bypassed;
    ctx->force_arm = def->alarm_force_arm;
    ctx->skip_delay = def->alarm_skip_delay;
    if (strcmp(def->alarm_backend, "alarmo") == 0) {
        ctx->backend = W_ALARM_BACKEND_ALARMO;
    } else if (strcmp(def->alarm_backend, "builtin") == 0) {
        ctx->backend = W_ALARM_BACKEND_BUILTIN;
    } else {
        ctx->backend = W_ALARM_BACKEND_AUTO;
    }
    ctx->use_alarmo = (ctx->backend == W_ALARM_BACKEND_ALARMO);
    snprintf(ctx->zone_label, sizeof(ctx->zone_label), "%s", def->alarm_zone_label);

    char modes[APP_MAX_ALARM_MODES_LEN] = {0};
    snprintf(modes, sizeof(modes), "%s", (def->alarm_modes[0] != '\0') ? def->alarm_modes : W_ALARM_DEFAULT_MODES);

    const bool title_is_auto_id = (def->title[0] != '\0' && strcmp(def->title, def->id) == 0);
    if (def->title[0] != '\0' && !title_is_auto_id) {
        ctx->title_label = lv_label_create(card);
        lv_obj_add_flag(ctx->title_label, LV_OBJ_FLAG_USER_1);
        /* Explicit colour: the card style carries no text colour, so an unstyled
         * label would fall back to the LVGL default (#212121) and vanish on the
         * dark card. */
        lv_obj_set_style_text_color(ctx->title_label, lv_color_hex(APP_UI_COLOR_TEXT_PRIMARY), LV_PART_MAIN);
        lv_label_set_text(ctx->title_label, def->title);
    }

    ctx->dot = lv_obj_create(card);
    lv_obj_remove_style_all(ctx->dot);
    lv_obj_set_style_radius(ctx->dot, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(ctx->dot, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(ctx->dot, lv_color_hex(APP_UI_COLOR_TEXT_MUTED), LV_PART_MAIN);
    lv_obj_clear_flag(ctx->dot, LV_OBJ_FLAG_SCROLLABLE);

    ctx->state_label = lv_label_create(card);
    lv_obj_add_flag(ctx->state_label, LV_OBJ_FLAG_USER_3);
    lv_obj_set_style_text_color(ctx->state_label, lv_color_hex(APP_UI_COLOR_TEXT_MUTED), LV_PART_MAIN);
    lv_label_set_long_mode(ctx->state_label, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_align(ctx->state_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_style_text_font(ctx->state_label, APP_FONT_TEXT_20, LV_PART_MAIN);
    lv_label_set_text(ctx->state_label, alarm_i18n("alarm.waiting", "Waiting..."));

    ctx->info_label = lv_label_create(card);
    lv_obj_add_flag(ctx->info_label, LV_OBJ_FLAG_USER_2);
    lv_obj_set_style_text_color(ctx->info_label, lv_color_hex(APP_UI_COLOR_TEXT_SOFT), LV_PART_MAIN);
    lv_obj_set_style_text_align(ctx->info_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_style_text_font(ctx->info_label, APP_FONT_TEXT_14, LV_PART_MAIN);
    lv_label_set_text(ctx->info_label, "");

    ctx->sensors_label = lv_label_create(card);
    lv_obj_add_flag(ctx->sensors_label, LV_OBJ_FLAG_USER_2);
    lv_obj_set_style_text_color(ctx->sensors_label, lv_color_hex(APP_UI_COLOR_TEXT_MUTED), LV_PART_MAIN);
    lv_obj_set_style_text_align(ctx->sensors_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_style_text_font(ctx->sensors_label, APP_FONT_TEXT_14, LV_PART_MAIN);
    lv_label_set_text(ctx->sensors_label, "");
    lv_obj_add_flag(ctx->sensors_label, LV_OBJ_FLAG_HIDDEN);

    if (ctx->zone_label[0] != '\0') {
        ctx->zone_obj = lv_label_create(card);
        lv_obj_add_flag(ctx->zone_obj, LV_OBJ_FLAG_USER_2);
        lv_obj_set_style_text_color(ctx->zone_obj, lv_color_hex(APP_UI_COLOR_TEXT_MUTED), LV_PART_MAIN);
        lv_label_set_text(ctx->zone_obj, ctx->zone_label);
        lv_obj_add_flag(ctx->zone_obj, LV_OBJ_FLAG_HIDDEN);
    }

    static const w_alarm_action_t action_order[W_ALARM_ACTION_COUNT] = {
        W_ALARM_ACTION_AWAY,
        W_ALARM_ACTION_HOME,
        W_ALARM_ACTION_NIGHT,
        W_ALARM_ACTION_VACATION,
        W_ALARM_ACTION_CUSTOM,
        W_ALARM_ACTION_DISARM,
    };

    for (int i = 0; i < W_ALARM_ACTION_COUNT; i++) {
        const w_alarm_action_t action = action_order[i];
        if (!alarm_modes_has(modes, alarm_action_token(action))) {
            continue;
        }
        lv_obj_t *button = lv_btn_create(card);
        lv_obj_clear_flag(button, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_user_data(button, (void *)(intptr_t)action);
        lv_obj_add_event_cb(button, alarm_button_event_cb, LV_EVENT_CLICKED, ctx);

        lv_obj_t *label = lv_label_create(button);
        lv_label_set_text(label, alarm_action_label(action));
        lv_obj_set_style_text_font(label, APP_FONT_TEXT_16, LV_PART_MAIN);
        lv_obj_set_style_text_color(label, lv_color_hex(APP_UI_COLOR_TEXT_PRIMARY), LV_PART_MAIN);
        lv_obj_center(label);

        ctx->buttons[ctx->button_count].action = action;
        ctx->buttons[ctx->button_count].button = button;
        ctx->buttons[ctx->button_count].label = label;
        ctx->button_count++;
    }

    lv_obj_add_event_cb(card, alarm_card_event_cb, LV_EVENT_SIZE_CHANGED, ctx);
    lv_obj_add_event_cb(card, alarm_card_event_cb, LV_EVENT_DELETE, ctx);
    ctx->timer = lv_timer_create(alarm_timer_cb, 500, ctx);

    alarm_set_status(ctx, alarm_i18n("alarm.waiting", "Waiting..."), APP_UI_COLOR_TEXT_MUTED);
    alarm_update_buttons(ctx);
    alarm_layout(ctx);

    out_instance->obj = card;
    out_instance->ctx = ctx;
    return ESP_OK;
}

void w_alarm_tile_apply_state(ui_widget_instance_t *instance, const ha_state_t *state)
{
    if (instance == NULL || instance->obj == NULL || state == NULL) {
        return;
    }

    w_alarm_tile_t *ctx = (w_alarm_tile_t *)instance->ctx;
    if (ctx == NULL) {
        return;
    }

    ctx->busy = false;

    if (alarm_state_is_unavailable(state->state)) {
        ctx->unavailable = true;
        ctx->info_text[0] = '\0';
        ctx->sensors_text[0] = '\0';
        ctx->open_count = 0;
        ctx->bypassed_count = 0;
        ctx->delay_active = false;
        ctx->delay_seconds = 0;
        ctx->delay_reported = 0;
        ctx->ready_known_mask = 0;
        ctx->ready_mask = 0;
        ctx->alarmo_text_until_ms = 0;
        ctx->alarmo_status_until_ms = 0;
        if (ctx->sensors_label != NULL) {
            lv_label_set_text(ctx->sensors_label, "");
        }
        alarm_render_info(ctx);
        alarm_set_status(ctx, alarm_i18n("alarm.unavailable", "Unavailable"), APP_UI_COLOR_TEXT_MUTED);
        alarm_update_buttons(ctx);
        alarm_layout(ctx);
        return;
    }

    ctx->unavailable = false;
    ctx->info_text[0] = '\0';
    ctx->sensors_text[0] = '\0';
    ctx->delay_active = false;
    ctx->delay_seconds = 0;

    if (state->attributes_json[0] != '\0') {
        cJSON *attrs = cJSON_Parse(state->attributes_json);
        if (attrs != NULL) {
            if (ctx->backend == W_ALARM_BACKEND_AUTO) {
                /* Alarmo only - the flag decides between alarmo.* and alarm_control_panel.* services. */
                const bool detected = cJSON_GetObjectItemCaseSensitive(attrs, "open_sensors") != NULL ||
                                      cJSON_GetObjectItemCaseSensitive(attrs, "bypassed_sensors") != NULL ||
                                      cJSON_GetObjectItemCaseSensitive(attrs, "arm_mode") != NULL ||
                                      cJSON_GetObjectItemCaseSensitive(attrs, "ready_to_arm") != NULL;
                if (detected != ctx->alarmo_detected) {
                    ESP_LOGI(TAG, "%s: Alarmo attributes %s", ctx->entity_id, detected ? "detected" : "gone");
                }
                ctx->alarmo_detected = detected;
                ctx->use_alarmo = detected;
            } else {
                ctx->use_alarmo = (ctx->backend == W_ALARM_BACKEND_ALARMO);
            }

            alarm_collect_sensors(ctx, attrs);

            /* HA tells us whether the entity wants a PIN - no layout flag needed. */
            const cJSON *code_format = cJSON_GetObjectItemCaseSensitive(attrs, "code_format");
            ctx->code_format_present = cJSON_IsString(code_format) && code_format->valuestring != NULL &&
                                       code_format->valuestring[0] != '\0';
            const cJSON *code_arm = cJSON_GetObjectItemCaseSensitive(attrs, "code_arm_required");
            /* Absent attribute: assume arming needs the code (safer than sending none). */
            ctx->code_arm_required = cJSON_IsBool(code_arm) ? cJSON_IsTrue(code_arm) : true;

            const cJSON *delay = cJSON_GetObjectItemCaseSensitive(attrs, "delay");
            int reported = 0;
            if (cJSON_IsNumber(delay)) {
                reported = (int)delay->valuedouble;
            } else if (cJSON_IsString(delay) && delay->valuestring != NULL) {
                reported = atoi(delay->valuestring);
            }
            if (reported > 0) {
                if ((uint32_t)reported != ctx->delay_reported) {
                    ctx->delay_reported = (uint32_t)reported;
                    ctx->delay_anchor_ms = lv_tick_get();
                }
                ctx->delay_active = true;
                ctx->delay_seconds = reported;
            } else {
                ctx->delay_reported = 0;
            }

            cJSON *changed_by = cJSON_GetObjectItemCaseSensitive(attrs, "changed_by");
            if (cJSON_IsString(changed_by) && changed_by->valuestring != NULL && changed_by->valuestring[0] != '\0') {
                snprintf(ctx->info_text, sizeof(ctx->info_text), "%s %s",
                         alarm_i18n("alarm.changed_by", "by"), changed_by->valuestring);
            }
            cJSON_Delete(attrs);
        }
    }

    /* Bypassed sensors are the more interesting information while arming. */
    if (ctx->show_bypassed && ctx->bypassed_count > 0) {
        snprintf(ctx->info_text, sizeof(ctx->info_text), "%s: %d",
                 alarm_i18n("alarm.bypassed", "Bypassed"), ctx->bypassed_count);
    }

    const w_alarm_state_map_t *mapped = NULL;
    for (size_t i = 0; i < sizeof(W_ALARM_STATE_MAP) / sizeof(W_ALARM_STATE_MAP[0]); i++) {
        if (strcmp(W_ALARM_STATE_MAP[i].state, state->state) == 0) {
            mapped = &W_ALARM_STATE_MAP[i];
            break;
        }
    }

    if (mapped != NULL) {
        alarm_set_status(ctx, alarm_i18n(mapped->key, mapped->fallback), mapped->accent);
    } else {
        alarm_set_status(ctx, state->state, APP_UI_COLOR_TEXT_PRIMARY);
    }

    if (ctx->sensors_label != NULL) {
        lv_label_set_text(ctx->sensors_label, ctx->sensors_text);
    }
    if (ctx->info_label != NULL) {
        alarm_render_info(ctx);
    }

    alarm_update_buttons(ctx);
    alarm_layout(ctx);
}

void w_alarm_tile_mark_unavailable(ui_widget_instance_t *instance)
{
    if (instance == NULL || instance->obj == NULL) {
        return;
    }

    w_alarm_tile_t *ctx = (w_alarm_tile_t *)instance->ctx;
    if (ctx == NULL) {
        return;
    }

    ctx->busy = false;
    ctx->unavailable = true;
    ctx->info_text[0] = '\0';
    ctx->sensors_text[0] = '\0';
    ctx->open_count = 0;
    ctx->bypassed_count = 0;
    ctx->delay_active = false;
    ctx->delay_seconds = 0;
    ctx->delay_reported = 0;
    ctx->ready_known_mask = 0;
    ctx->ready_mask = 0;
    ctx->alarmo_text_until_ms = 0;
    ctx->alarmo_status_until_ms = 0;
    if (ctx->info_label != NULL) {
        lv_label_set_text(ctx->info_label, "");
    }
    if (ctx->sensors_label != NULL) {
        lv_label_set_text(ctx->sensors_label, "");
    }
    alarm_set_status(ctx, alarm_i18n("alarm.unavailable", "Unavailable"), APP_UI_COLOR_TEXT_MUTED);
    alarm_update_buttons(ctx);
    alarm_layout(ctx);
}
