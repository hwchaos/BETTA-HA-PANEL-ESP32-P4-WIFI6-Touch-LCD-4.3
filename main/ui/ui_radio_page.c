/* SPDX-License-Identifier: LicenseRef-FNCL-1.1
 * Copyright (c) 2026 Cpt_Kirk
 *
 * Full screen internet-radio page.
 *
 * A station is only a stream URL plus a player.  The header chip selects the
 * player: the panel's own speaker (main/radio/panel_radio.c decodes the MP3 and
 * writes it to the ES8311) or a Home Assistant media_player
 * (media_player.play_media with media_content_id = <url>).
 *
 * Layout of the page (all inside the page container, which is already the
 * content box):
 *
 *   +--------------------------------------------------------------+
 *   | now playing / state badge               [ player chip ]      |  header
 *   +--------------------------------------------------------------+
 *   | [ station ] [ station ] [ station ]                          |
 *   | [ station ] [ station ] [ station ]                          |  grid (only
 *   | ...                                       (scrolls)          |  scrolling
 *   +--------------------------------------------------------------+  part)
 *   | volume icon  [=====|-------]  35%           [  STOP  ]       |  footer
 *   +--------------------------------------------------------------+
 *
 * The player chip opens a full-page chooser with the built-in speaker and the
 * media_players Home Assistant knows about.
 */
#include "ui/ui_radio_page.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "nvs.h"

#include "app_config.h"
#include "diag/storage_guard.h"
#include "diag/system_log.h"
#include "ha/ha_client.h"
#include "radio/panel_radio.h"
#include "ui/fonts/app_text_fonts.h"
#include "ui/fonts/mdi_font_registry.h"
#include "ui/theme/theme_default.h"
#include "ui/ui_i18n.h"
#include "ui/ui_memory.h"
#include "ui/ui_radio_stations.h"
#include "ui/ui_slider_touch.h"
#include "util/log_tags.h"
#include "xiaozhi/xiaozhi_audio.h"

#define TAG TAG_RADIO

/* MDI glyphs used by the page (Material Design Icons codepoints). */
#define RADIO_ICON_VOLUME_HIGH 0xF057EU
#define RADIO_ICON_STOP 0xF04DBU

/*
 * The chosen player is a device-local preference (it says which hardware plays
 * the stream), so it lives in NVS rather than in the layout document that the
 * WebUI edits.
 */
#define RADIO_NVS_NAMESPACE "radio_prefs"
#define RADIO_NVS_KEY_MODE "player_mode"
#define RADIO_NVS_KEY_ENTITY "player_entity"
#define RADIO_MODE_PANEL "panel"
#define RADIO_MODE_HA "ha"

/* How often the page asks the on-panel engine what it is doing. */
#define RADIO_POLL_MS 500
/* Player chooser overlay. */
#define RADIO_PICK_MAX_ROWS 12
#define RADIO_PICK_MAX_W 720
#define RADIO_PICK_MAX_H 440

/*
 * Geometry.  Every panel variant uses the whole content box; the small 480x360
 * variant only shrinks the strips and the tiles.  The numbers are percentages
 * of the content box in spirit, not in code: the header is a fixed strip, the
 * grid takes what is left and the footer is pinned to the bottom edge.
 */
#if defined(CONFIG_APP_PANEL_VARIANT_S3_480)
#define RADIO_MARGIN 8
#define RADIO_GAP 6
#define RADIO_HEADER_H 44
#define RADIO_FOOTER_H 50
#define RADIO_TILE_H 58
#define RADIO_TILE_RADIUS 10
#define RADIO_HEADER_FONT APP_FONT_TEXT_16
#define RADIO_STATE_FONT APP_FONT_TEXT_12
#define RADIO_TILE_FONT APP_FONT_TEXT_14
#define RADIO_BUTTON_FONT APP_FONT_TEXT_14
#define RADIO_SLIDER_W 150
#define RADIO_STOP_W 96
/* Widths of the header columns, as insets from the strip width. */
#define RADIO_NOW_W_INSET 210
#define RADIO_STATE_X_INSET 205
#define RADIO_STATE_W 110
#define RADIO_ENTITY_W 96
#define RADIO_PICK_ROW_H 40
#else
#define RADIO_MARGIN 10
#define RADIO_GAP 8
#define RADIO_HEADER_H 54
#define RADIO_FOOTER_H 62
#define RADIO_TILE_H 88
#define RADIO_TILE_RADIUS 14
#define RADIO_HEADER_FONT APP_FONT_TEXT_20
#define RADIO_STATE_FONT APP_FONT_TEXT_14
#define RADIO_TILE_FONT APP_FONT_TEXT_16
#define RADIO_BUTTON_FONT APP_FONT_TEXT_16
#define RADIO_SLIDER_W 240
#define RADIO_STOP_W 132
#define RADIO_NOW_W_INSET 420
#define RADIO_STATE_X_INSET 400
#define RADIO_STATE_W 140
#define RADIO_ENTITY_W 240
#define RADIO_PICK_ROW_H 56
#endif

/* Player states that mean "nothing is playing right now". */
#define RADIO_STATE_IDLE "idle"
#define RADIO_STATE_OFF "off"
#define RADIO_STATE_STANDBY "standby"
#define RADIO_STATE_UNAVAILABLE "unavailable"
#define RADIO_STATE_UNKNOWN "unknown"

typedef struct radio_ctx_s {
    /* Back pointer so the runtime never keeps a dangling context (see
     * radio_delete_cb). */
    ui_radio_page_instance_t *owner;

    ui_radio_page_config_t config;

    lv_obj_t *root;
    lv_obj_t *now_label;
    lv_obj_t *state_label;
    lv_obj_t *player_btn; /* header chip: tap opens the chooser */
    lv_obj_t *entity_label;
    lv_obj_t *grid;
    lv_obj_t *tiles[UI_RADIO_MAX_STATIONS];
    lv_obj_t *tile_labels[UI_RADIO_MAX_STATIONS];
    lv_obj_t *footer;
    lv_obj_t *vol_icon;
    lv_obj_t *vol_slider;
    lv_obj_t *vol_label;
    lv_obj_t *stop_btn;
    lv_obj_t *stop_label;
    lv_obj_t *msg_box;
    lv_obj_t *msg_label;
    lv_obj_t *picker;      /* player chooser overlay, NULL when closed */
    lv_obj_t *picker_list;
    lv_timer_t *poll_timer; /* polls the on-panel engine in panel mode */

    /* Default player of the page, resolved from the config or from the HA
     * model when the layout did not name one. */
    char player[APP_MAX_ENTITY_ID_LEN];
    /* media_player the last command was sent to (may be a per-station
     * override). */
    char active[APP_MAX_ENTITY_ID_LEN];
    /* Station shown as playing, -1 when none of the tiles matches. */
    int playing_index;
    int volume_pct;
    /* Current state as text, kept so a redraw needs no HA round trip. */
    char state_text[32];
    char now_text[UI_RADIO_STATION_NAME_LEN];
    bool is_playing;
    /* Command sent, waiting for the state to come back. */
    bool pending;
    bool volume_dragging;

    /* true = the stream is decoded on the panel itself. */
    bool panel_mode;
    /* Last state reported by the on-panel engine, so the poll only redraws on a
     * change. */
    panel_radio_state_t panel_state;
    bool panel_state_known;
    char panel_title[PANEL_RADIO_TITLE_MAX];
    char panel_error[PANEL_RADIO_TEXT_MAX];
} radio_ctx_t;

/* One row of the player chooser; freed when the row is deleted. */
typedef struct {
    radio_ctx_t *ctx;
    bool panel;
    char entity[APP_MAX_ENTITY_ID_LEN];
} radio_pick_row_t;

/*
 * Live pages.  ui_radio_page_on_shown() only gets the page id and the
 * LV_EVENT_DELETE handler must not rely on user data (ui_runtime deinits the
 * page before the containers are deleted), so the contexts are tracked here.
 */
static radio_ctx_t *s_live[APP_MAX_PAGES];
static size_t s_live_count;

/* ---------------------------------------------------------------- helpers */

static int radio_clamp_int(int value, int min_value, int max_value)
{
    if (value < min_value) {
        return min_value;
    }
    if (value > max_value) {
        return max_value;
    }
    return value;
}

static void radio_register(radio_ctx_t *ctx)
{
    for (size_t i = 0; i < s_live_count; i++) {
        if (s_live[i] == ctx) {
            return;
        }
    }
    if (s_live_count < APP_MAX_PAGES) {
        s_live[s_live_count++] = ctx;
    }
}

static void radio_unregister(radio_ctx_t *ctx)
{
    for (size_t i = 0; i < s_live_count; i++) {
        if (s_live[i] != ctx) {
            continue;
        }
        s_live[i] = s_live[s_live_count - 1];
        s_live_count--;
        return;
    }
}

static radio_ctx_t *radio_find_by_root(lv_obj_t *root)
{
    if (root == NULL) {
        return NULL;
    }
    for (size_t i = 0; i < s_live_count; i++) {
        if (s_live[i] != NULL && s_live[i]->root == root) {
            return s_live[i];
        }
    }
    return NULL;
}
/* ---------------------------------------------------------- player prefs */

/*
 * The player picked on the panel wins over the layout configuration: the layout
 * only provides a default, while a tap here is an explicit decision (and the
 * WebUI cannot know whether the panel's speaker exists).
 */
static void radio_prefs_load(radio_ctx_t *ctx)
{
    nvs_handle_t handle = 0;
    if (nvs_open(RADIO_NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) {
        return;
    }

    char mode[8] = {0};
    size_t len = sizeof(mode);
    if (nvs_get_str(handle, RADIO_NVS_KEY_MODE, mode, &len) != ESP_OK) {
        /* Nothing picked yet: keep the layout configuration. */
        nvs_close(handle);
        return;
    }
    ctx->panel_mode = strcmp(mode, RADIO_MODE_PANEL) == 0;

    len = sizeof(ctx->player);
    if (nvs_get_str(handle, RADIO_NVS_KEY_ENTITY, ctx->player, &len) != ESP_OK || ctx->player[0] == '\0') {
        ctx->player[0] = '\0';
    }
    nvs_close(handle);

    if (!ctx->panel_mode && ctx->player[0] != '\0') {
        snprintf(ctx->config.player_entity_id, sizeof(ctx->config.player_entity_id), "%s", ctx->player);
    }
    ESP_LOGI(TAG, "radio page '%s' keeps its player choice (%s %s)", ctx->config.page_id,
        ctx->panel_mode ? "panel" : "ha", ctx->panel_mode ? "" : ctx->player);
}

static void radio_prefs_save(radio_ctx_t *ctx)
{
    nvs_handle_t handle = 0;
    if (nvs_open(RADIO_NVS_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK) {
        ESP_LOGW(TAG, "cannot store the player choice (NVS unavailable)");
        return;
    }
    (void)nvs_set_str(handle, RADIO_NVS_KEY_MODE, ctx->panel_mode ? RADIO_MODE_PANEL : RADIO_MODE_HA);
    if (ctx->panel_mode || ctx->player[0] == '\0') {
        (void)nvs_erase_key(handle, RADIO_NVS_KEY_ENTITY);
    } else {
        (void)nvs_set_str(handle, RADIO_NVS_KEY_ENTITY, ctx->player);
    }
    storage_guard_flash_op_begin(FLASH_OP_NVS);
    (void)nvs_commit(handle);
    storage_guard_flash_op_end(FLASH_OP_NVS);
    nvs_close(handle);
}

/* ---------------------------------------------------------------- helpers */

static bool radio_panel_mode(const radio_ctx_t *ctx)
{
    return ctx->panel_mode;
}


static void radio_utf8_encode(uint32_t codepoint, char *out, size_t out_len)
{
    if (out == NULL || out_len < 5) {
        return;
    }
    if (codepoint < 0x80U) {
        out[0] = (char)codepoint;
        out[1] = '\0';
    } else if (codepoint < 0x800U) {
        out[0] = (char)(0xC0U | (codepoint >> 6));
        out[1] = (char)(0x80U | (codepoint & 0x3FU));
        out[2] = '\0';
    } else if (codepoint < 0x10000U) {
        out[0] = (char)(0xE0U | (codepoint >> 12));
        out[1] = (char)(0x80U | ((codepoint >> 6) & 0x3FU));
        out[2] = (char)(0x80U | (codepoint & 0x3FU));
        out[3] = '\0';
    } else {
        out[0] = (char)(0xF0U | (codepoint >> 18));
        out[1] = (char)(0x80U | ((codepoint >> 12) & 0x3FU));
        out[2] = (char)(0x80U | ((codepoint >> 6) & 0x3FU));
        out[3] = (char)(0x80U | (codepoint & 0x3FU));
        out[4] = '\0';
    }
}

/* Show an MDI glyph when the icon font is built in and really carries it, and
 * the caller's text otherwise.  Returns true when the glyph was used. */
static bool radio_set_icon(lv_obj_t *label, uint32_t codepoint, const char *fallback, const lv_font_t *fallback_font)
{
    if (label == NULL) {
        return false;
    }
    const lv_font_t *icon_font = mdi_font_icon_42();
    if (icon_font != NULL) {
        lv_font_glyph_dsc_t dsc = {0};
        if (lv_font_get_glyph_dsc(icon_font, &dsc, codepoint, 0)) {
            char buffer[8] = {0};
            radio_utf8_encode(codepoint, buffer, sizeof(buffer));
            lv_obj_set_style_text_font(label, icon_font, LV_PART_MAIN);
            lv_label_set_text(label, buffer);
            return true;
        }
    }
    lv_obj_set_style_text_font(label, fallback_font != NULL ? fallback_font : APP_FONT_TEXT_14, LV_PART_MAIN);
    lv_label_set_text(label, fallback != NULL ? fallback : "");
    return false;
}

/* ------------------------------------------------------------- stations */

/* Number of tiles: the configured stations, or the compiled-in table. */
static size_t radio_station_count(const radio_ctx_t *ctx)
{
    if (ctx->config.station_count > 0) {
        return (size_t)ctx->config.station_count;
    }
    return ui_radio_stations_count();
}

/* Station name / url, or "" when the index is out of range.  Names and urls live
 * in the context or in flash, so the pointers stay valid. */
static const char *radio_station_name(const radio_ctx_t *ctx, size_t index)
{
    if (index >= radio_station_count(ctx)) {
        return "";
    }
    if (ctx->config.station_count > 0) {
        return ctx->config.stations[index].name;
    }
    const ui_radio_station_t *station = ui_radio_stations_get(index);
    return (station != NULL && station->name != NULL) ? station->name : "";
}

static const char *radio_station_url(const radio_ctx_t *ctx, size_t index)
{
    if (index >= radio_station_count(ctx)) {
        return "";
    }
    if (ctx->config.station_count > 0) {
        return ctx->config.stations[index].url;
    }
    const ui_radio_station_t *station = ui_radio_stations_get(index);
    return (station != NULL && station->url != NULL) ? station->url : "";
}

/* Optional per-station player override. */
static const char *radio_station_entity(const radio_ctx_t *ctx, size_t index)
{
    if (ctx->config.station_count == 0 || index >= (size_t)ctx->config.station_count) {
        return "";
    }
    return ctx->config.stations[index].entity;
}

/* media_player that should play `index`: the station override, else the page
 * default. */
static const char *radio_target_entity(const radio_ctx_t *ctx, size_t index)
{
    const char *entity = radio_station_entity(ctx, index);
    if (entity[0] != '\0') {
        return entity;
    }
    return ctx->player;
}

/* Player the transport commands go to. */
static const char *radio_active_entity(const radio_ctx_t *ctx)
{
    if (ctx->active[0] != '\0') {
        return ctx->active;
    }
    return ctx->player;
}

/* Does the page have any media_player at all (own, station override, or
 * resolved from the HA model)?  Panel playback needs no player at all. */
static bool radio_has_target(const radio_ctx_t *ctx)
{
    if (radio_panel_mode(ctx)) {
        return true;
    }
    if (ctx->player[0] != '\0') {
        return true;
    }
    for (size_t i = 0; i < radio_station_count(ctx); i++) {
        if (radio_station_entity(ctx, i)[0] != '\0') {
            return true;
        }
    }
    return false;
}

/* Player name shown in the header chip. */
static const char *radio_player_label(const radio_ctx_t *ctx, char *buffer, size_t buffer_len)
{
    if (radio_panel_mode(ctx)) {
        snprintf(buffer, buffer_len, "%s", ui_i18n_get("radio.player_panel_short", "Głośnik panelu"));
        return buffer;
    }

    const char *entity = radio_active_entity(ctx);
    if (entity[0] == '\0') {
        snprintf(buffer, buffer_len, "%s", ui_i18n_get("radio.no_player", "Brak odtwarzacza w HA"));
        return buffer;
    }
    /* Entities read better without the domain prefix. */
    const char *short_id = entity;
    if (strncmp(short_id, "media_player.", 13) == 0) {
        short_id += 13;
    }
    snprintf(buffer, buffer_len, "%s", short_id);
    return buffer;
}

/* Index of the station whose name matches `title`, -1 when none does. */
static int radio_index_for_title(const radio_ctx_t *ctx, const char *title)
{
    if (title == NULL || title[0] == '\0') {
        return -1;
    }
    for (size_t i = 0; i < radio_station_count(ctx); i++) {
        const char *name = radio_station_name(ctx, i);
        if (name[0] != '\0' && strcmp(name, title) == 0) {
            return (int)i;
        }
    }
    return -1;
}

/* Resolve the page default player: the configured one wins, otherwise the first
 * media_player in the HA model.  A player that was found earlier is kept so the
 * choice stays stable. */
static void radio_resolve_player(radio_ctx_t *ctx)
{
    if (ctx->config.player_entity_id[0] != '\0') {
        if (strncmp(ctx->player, ctx->config.player_entity_id, sizeof(ctx->player)) != 0) {
            snprintf(ctx->player, sizeof(ctx->player), "%s", ctx->config.player_entity_id);
            ESP_LOGI(TAG, "radio page '%s' uses configured player %s", ctx->config.page_id, ctx->player);
        }
        return;
    }
    if (ctx->player[0] != '\0') {
        return;
    }

    ha_entity_info_t *infos = ui_calloc_prefer_psram(8, sizeof(ha_entity_info_t));
    if (infos == NULL) {
        return;
    }
    const size_t count = ha_model_list_entities("media_player", NULL, infos, 8);
    for (size_t i = 0; i < count; i++) {
        if (infos[i].id[0] == '\0') {
            continue;
        }
        snprintf(ctx->player, sizeof(ctx->player), "%s", infos[i].id);
        ESP_LOGI(TAG, "radio page '%s' picked player %s", ctx->config.page_id, ctx->player);
        break;
    }
    heap_caps_free(infos);
}

/* ------------------------------------------------------------- commands */

static bool radio_send_service(const char *entity, const char *service, cJSON *body)
{
    if (body == NULL) {
        return false;
    }
    char *json = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);
    if (json == NULL) {
        return false;
    }
    const esp_err_t err = ha_client_call_service("media_player", service, json);
    cJSON_free(json);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "media_player.%s on %s failed (%s)", service, entity != NULL ? entity : "?", esp_err_to_name(err));
        system_log_event(TAG, "media_player.%s failed (%s)", service, esp_err_to_name(err));
        return false;
    }
    return true;
}

static void radio_show_state(radio_ctx_t *ctx, const char *key, const char *fallback, bool playing)
{
    snprintf(ctx->state_text, sizeof(ctx->state_text), "%s", ui_i18n_get(key, fallback));
    ctx->is_playing = playing;
}

static void radio_render(radio_ctx_t *ctx)
{
    const bool has_target = radio_has_target(ctx);

    /* Message instead of the grid when HA has no media_player at all. */
    if (ctx->msg_box != NULL) {
        if (has_target) {
            lv_obj_add_flag(ctx->msg_box, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_clear_flag(ctx->msg_box, LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (ctx->grid != NULL) {
        if (has_target) {
            lv_obj_clear_flag(ctx->grid, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(ctx->grid, LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (ctx->footer != NULL) {
        if (has_target) {
            lv_obj_clear_flag(ctx->footer, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(ctx->footer, LV_OBJ_FLAG_HIDDEN);
        }
    }

    /* Header: page title / station name, state, player entity. */
    if (ctx->now_label != NULL) {
        const char *text = NULL;
        if (!has_target) {
            text = ui_i18n_get("radio.no_player", "Brak odtwarzacza w HA");
        } else if (ctx->now_text[0] != '\0') {
            text = ctx->now_text;
        } else if (ctx->config.title[0] != '\0') {
            text = ctx->config.title;
        } else {
            text = ui_i18n_get("radio.idle", "Nic nie jest odtwarzane");
        }
        lv_label_set_text(ctx->now_label, text);
    }
    if (ctx->state_label != NULL) {
        lv_label_set_text(ctx->state_label, has_target ? ctx->state_text : "");
        const uint32_t color = ctx->is_playing ? APP_UI_COLOR_STATE_ON : APP_UI_COLOR_TEXT_MUTED;
        lv_obj_set_style_text_color(ctx->state_label, lv_color_hex(color), LV_PART_MAIN);
    }
    if (ctx->entity_label != NULL) {
        char buffer[APP_MAX_ENTITY_ID_LEN] = {0};
        lv_label_set_text(ctx->entity_label, has_target ? radio_player_label(ctx, buffer, sizeof(buffer)) : "");
        lv_obj_set_style_text_color(ctx->entity_label,
            lv_color_hex(has_target ? APP_UI_COLOR_TEXT_PRIMARY : APP_UI_COLOR_TEXT_SOFT), LV_PART_MAIN);
    }
    if (ctx->player_btn != NULL) {
        lv_obj_set_style_border_color(ctx->player_btn,
            lv_color_hex(has_target ? APP_UI_COLOR_STATE_ON : APP_UI_COLOR_CARD_BORDER), LV_PART_MAIN);
    }

    /* Tiles: highlight the playing one. */
    for (size_t i = 0; i < radio_station_count(ctx); i++) {
        lv_obj_t *tile = ctx->tiles[i];
        if (tile == NULL) {
            continue;
        }
        const bool playing = ctx->is_playing && ctx->playing_index == (int)i;
        lv_obj_set_style_bg_color(
            tile, lv_color_hex(playing ? APP_UI_COLOR_CARD_BG_ON : APP_UI_COLOR_CARD_BG_OFF), LV_PART_MAIN);
        lv_obj_set_style_border_color(
            tile, lv_color_hex(playing ? APP_UI_COLOR_STATE_ON : APP_UI_COLOR_CARD_BORDER), LV_PART_MAIN);
        lv_obj_set_style_border_width(tile, playing ? 2 : 1, LV_PART_MAIN);
        if (ctx->tile_labels[i] != NULL) {
            lv_obj_set_style_text_color(
                ctx->tile_labels[i], lv_color_hex(playing ? APP_UI_COLOR_STATE_ON : APP_UI_COLOR_TEXT_PRIMARY), LV_PART_MAIN);
        }
    }

    /* Footer: volume. */
    if (ctx->vol_slider != NULL && !ctx->volume_dragging) {
        lv_slider_set_value(ctx->vol_slider, radio_clamp_int(ctx->volume_pct, 0, 100), LV_ANIM_OFF);
    }
    if (ctx->vol_label != NULL) {
        char buffer[16] = {0};
        snprintf(buffer, sizeof(buffer), "%d%%", radio_clamp_int(ctx->volume_pct, 0, 100));
        lv_label_set_text(ctx->vol_label, buffer);
    }
}

/*
 * Panel playback is asynchronous: the engine reports through its getters and
 * there is no state event to wait for, so the page polls it and redraws only
 * when something really changed.
 */
static void radio_panel_sync(radio_ctx_t *ctx, bool force)
{
    const panel_radio_state_t state = panel_radio_get_state();
    char title[PANEL_RADIO_TITLE_MAX] = {0};
    char error[PANEL_RADIO_TEXT_MAX] = {0};
    panel_radio_get_title(title, sizeof(title));
    panel_radio_get_error(error, sizeof(error));

    bool changed = force || !ctx->panel_state_known || state != ctx->panel_state ||
                   strncmp(title, ctx->panel_title, sizeof(ctx->panel_title)) != 0 ||
                   strncmp(error, ctx->panel_error, sizeof(ctx->panel_error)) != 0;
    if (!changed) {
        return;
    }

    ctx->panel_state = state;
    ctx->panel_state_known = true;
    snprintf(ctx->panel_title, sizeof(ctx->panel_title), "%s", title);
    snprintf(ctx->panel_error, sizeof(ctx->panel_error), "%s", error);

    switch (state) {
    case PANEL_RADIO_PLAYING:
        ctx->pending = false;
        if (title[0] != '\0') {
            snprintf(ctx->now_text, sizeof(ctx->now_text), "%s", title);
            if (ctx->playing_index < 0) {
                ctx->playing_index = radio_index_for_title(ctx, title);
            }
        }
        radio_show_state(ctx, "radio.playing", "Odtwarzanie", true);
        break;
    case PANEL_RADIO_BUFFERING:
        radio_show_state(ctx, "radio.connecting", "Łączenie...", false);
        break;
    case PANEL_RADIO_ERROR:
        ctx->pending = false;
        ctx->playing_index = -1;
        radio_show_state(ctx, "radio.error", "Błąd stacji", false);
        ESP_LOGW(TAG, "panel radio error: %s", ctx->panel_error);
        break;
    case PANEL_RADIO_IDLE:
    default:
        /* A stream can end without an error: a voice session took the speaker,
         * or the user stopped it somewhere else. */
        if (ctx->pending || ctx->is_playing) {
            ctx->pending = false;
            ctx->playing_index = -1;
            radio_show_state(ctx, "radio.stopped", "Zatrzymane", false);
        }
        break;
    }

    radio_render(ctx);
}

static void radio_poll_timer_cb(lv_timer_t *timer)
{
    radio_ctx_t *ctx = (radio_ctx_t *)lv_timer_get_user_data(timer);
    if (ctx != NULL && radio_panel_mode(ctx)) {
        radio_panel_sync(ctx, false);
    }
}

/* The poll timer only exists while the panel's own speaker is the player; in HA
 * mode the state arrives through the runtime's entity updates. */
static void radio_poll_control(radio_ctx_t *ctx)
{
    if (ctx == NULL) {
        return;
    }
    if (!radio_panel_mode(ctx)) {
        if (ctx->poll_timer != NULL) {
            lv_timer_del(ctx->poll_timer);
            ctx->poll_timer = NULL;
        }
        return;
    }
    if (ctx->poll_timer == NULL) {
        ctx->poll_timer = lv_timer_create(radio_poll_timer_cb, RADIO_POLL_MS, ctx);
    }
}

static void radio_update_volume_label(radio_ctx_t *ctx)
{
    if (ctx->vol_label == NULL) {
        return;
    }
    char buffer[16] = {0};
    snprintf(buffer, sizeof(buffer), "%d%%", radio_clamp_int(ctx->volume_pct, 0, 100));
    lv_label_set_text(ctx->vol_label, buffer);
}

static void radio_play_station(radio_ctx_t *ctx, size_t index)
{
    const char *url = radio_station_url(ctx, index);
    const char *name = radio_station_name(ctx, index);
    if (url[0] == '\0') {
        ESP_LOGW(TAG, "station '%s' has no stream url", name);
        return;
    }

    if (radio_panel_mode(ctx)) {
        const esp_err_t err = panel_radio_play(url, name);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "panel radio refused '%s': %s", name, esp_err_to_name(err));
            system_log_event(TAG, "panel radio play failed (%s)", esp_err_to_name(err));
            radio_show_state(ctx, "radio.error", "Błąd stacji", false);
            radio_render(ctx);
            return;
        }
        snprintf(ctx->now_text, sizeof(ctx->now_text), "%s", name);
        ctx->playing_index = (int)index;
        ctx->pending = true;
        ctx->is_playing = false;
        radio_show_state(ctx, "radio.connecting", "Łączenie...", false);
        radio_render(ctx);
        ESP_LOGI(TAG, "panel radio playing '%s' (%s)", name, url);
        return;
    }

    const char *entity = radio_target_entity(ctx, index);
    if (entity[0] == '\0') {
        ESP_LOGW(TAG, "no media_player for station '%s'", name);
        radio_render(ctx);
        return;
    }

    cJSON *body = cJSON_CreateObject();
    if (body == NULL) {
        return;
    }
    cJSON_AddStringToObject(body, "entity_id", entity);
    cJSON_AddStringToObject(body, "media_content_id", url);
    cJSON_AddStringToObject(body, "media_content_type", "music");
    if (!radio_send_service(entity, "play_media", body)) {
        return;
    }

    snprintf(ctx->active, sizeof(ctx->active), "%s", entity);
    snprintf(ctx->now_text, sizeof(ctx->now_text), "%s", name);
    ctx->playing_index = (int)index;
    ctx->pending = true;
    radio_show_state(ctx, "radio.connecting", "Łączenie...", false);
    radio_render(ctx);
    ESP_LOGI(TAG, "play_media '%s' -> %s", name, entity);
}

static void radio_stop(radio_ctx_t *ctx)
{
    if (radio_panel_mode(ctx)) {
        const bool was_active = panel_radio_is_active();
        panel_radio_stop();
        ctx->pending = false;
        ctx->playing_index = -1;
        ctx->panel_state = PANEL_RADIO_IDLE;
        ctx->panel_state_known = true;
        ctx->panel_title[0] = '\0';
        ctx->panel_error[0] = '\0';
        radio_show_state(ctx, "radio.stopped", "Zatrzymane", false);
        radio_render(ctx);
        if (was_active) {
            ESP_LOGI(TAG, "panel radio stopped");
        }
        return;
    }

    const char *entity = radio_active_entity(ctx);
    if (entity[0] == '\0') {
        return;
    }
    cJSON *body = cJSON_CreateObject();
    if (body == NULL) {
        return;
    }
    cJSON_AddStringToObject(body, "entity_id", entity);
    if (!radio_send_service(entity, "media_stop", body)) {
        return;
    }
    ctx->pending = false;
    ctx->playing_index = -1;
    radio_show_state(ctx, "radio.stopped", "Zatrzymane", false);
    radio_render(ctx);
}

static void radio_send_volume(radio_ctx_t *ctx, int pct)
{
    const int volume = radio_clamp_int(pct, 0, 100);

    if (radio_panel_mode(ctx)) {
        /* The panel speaker volume is stored by the audio backend, so it
         * survives a reboot on its own. */
        (void)xz_audio_set_volume(volume);
        return;
    }

    const char *entity = radio_active_entity(ctx);
    if (entity[0] == '\0') {
        return;
    }
    cJSON *body = cJSON_CreateObject();
    if (body == NULL) {
        return;
    }
    cJSON_AddStringToObject(body, "entity_id", entity);
    cJSON_AddNumberToObject(body, "volume_level", (double)volume / 100.0);
    (void)radio_send_service(entity, "volume_set", body);
}

/* Stop whatever the current player is doing; used when the player changes. */
static void radio_stop_playback(radio_ctx_t *ctx)
{
    if (radio_panel_mode(ctx)) {
        panel_radio_stop();
        return;
    }
    if (radio_active_entity(ctx)[0] != '\0') {
        radio_stop(ctx);
    }
}

/* --------------------------------------------------------------- events */

static void radio_tile_event_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_CLICKED) {
        return;
    }
    radio_ctx_t *ctx = (radio_ctx_t *)lv_event_get_user_data(event);
    if (ctx == NULL) {
        return;
    }
    lv_obj_t *tile = lv_event_get_current_target(event);
    for (size_t i = 0; i < radio_station_count(ctx); i++) {
        if (ctx->tiles[i] != tile) {
            continue;
        }
        if (ctx->is_playing && ctx->playing_index == (int)i) {
            /* Tapping the playing station stops it. */
            radio_stop(ctx);
        } else {
            radio_play_station(ctx, i);
        }
        return;
    }
}

static void radio_stop_event_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_CLICKED) {
        return;
    }
    radio_ctx_t *ctx = (radio_ctx_t *)lv_event_get_user_data(event);
    if (ctx != NULL) {
        radio_stop(ctx);
    }
}

static void radio_volume_event_cb(lv_event_t *event)
{
    radio_ctx_t *ctx = (radio_ctx_t *)lv_event_get_user_data(event);
    if (ctx == NULL || ctx->vol_slider == NULL) {
        return;
    }

    const lv_event_code_t code = lv_event_get_code(event);
    switch (code) {
    case LV_EVENT_PRESSED:
    case LV_EVENT_PRESSING:
        ctx->volume_dragging = true;
        ctx->volume_pct = radio_clamp_int((int)lv_slider_get_value(ctx->vol_slider), 0, 100);
        radio_update_volume_label(ctx);
        break;
    case LV_EVENT_VALUE_CHANGED:
        ctx->volume_pct = radio_clamp_int((int)lv_slider_get_value(ctx->vol_slider), 0, 100);
        radio_update_volume_label(ctx);
        break;
    case LV_EVENT_RELEASED:
    case LV_EVENT_PRESS_LOST: {
        ctx->volume_dragging = false;
        const int pct = radio_clamp_int((int)lv_slider_get_value(ctx->vol_slider), 0, 100);
        ctx->volume_pct = pct;
        radio_update_volume_label(ctx);
        radio_send_volume(ctx, pct);
        break;
    }
    default:
        break;
    }
}

/* The page container is deleted after ui_radio_page_deinit() ran, so a context
 * that is no longer registered here was already freed by the runtime. */
static void radio_delete_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_DELETE) {
        return;
    }
    radio_ctx_t *ctx = radio_find_by_root(lv_event_get_target(event));
    if (ctx == NULL) {
        return;
    }
    if (ctx->poll_timer != NULL) {
        lv_timer_del(ctx->poll_timer);
        ctx->poll_timer = NULL;
    }
    radio_unregister(ctx);
    if (ctx->owner != NULL) {
        ctx->owner->ctx = NULL;
    }
    heap_caps_free(ctx);
}

/* ------------------------------------------------------------- builders */

static void radio_style_tile(lv_obj_t *tile)
{
    lv_obj_set_style_bg_color(tile, lv_color_hex(APP_UI_COLOR_CARD_BG_OFF), LV_PART_MAIN);
    lv_obj_set_style_bg_color(tile, lv_color_hex(APP_UI_COLOR_CARD_BG_ON), LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(tile, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(tile, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(tile, lv_color_hex(APP_UI_COLOR_CARD_BORDER), LV_PART_MAIN);
    lv_obj_set_style_border_color(tile, lv_color_hex(APP_UI_COLOR_STATE_ON), LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_set_style_radius(tile, RADIO_TILE_RADIUS, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(tile, 0, LV_PART_MAIN);
    lv_obj_set_style_outline_width(tile, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(tile, 4, LV_PART_MAIN);
    lv_obj_clear_flag(tile, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(tile, LV_OBJ_FLAG_EVENT_BUBBLE);
}

static void radio_style_button(lv_obj_t *button)
{
    lv_obj_set_style_bg_color(button, lv_color_hex(APP_UI_COLOR_CARD_BG_OFF), LV_PART_MAIN);
    lv_obj_set_style_bg_color(button, lv_color_hex(APP_UI_COLOR_CARD_BG_ON), LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(button, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(button, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(button, lv_color_hex(APP_UI_COLOR_CARD_BORDER), LV_PART_MAIN);
    lv_obj_set_style_border_color(button, lv_color_hex(APP_UI_COLOR_STATE_ON), LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_set_style_radius(button, RADIO_TILE_RADIUS, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(button, 0, LV_PART_MAIN);
    lv_obj_set_style_outline_width(button, 0, LV_PART_MAIN);
    lv_obj_clear_flag(button, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(button, LV_OBJ_FLAG_EVENT_BUBBLE);
}

/* ------------------------------------------------------ the player chooser */

/*
 * The player is picked on the panel because only the panel knows whether it has
 * its own speaker: "Głośnik panelu" decodes the stream on the ESP32-P4 and
 * plays it through the ES8311, "Home Assistant" hands the station to one of the
 * media_player entities.
 */
static void radio_pick_close(radio_ctx_t *ctx)
{
    if (ctx == NULL || ctx->picker == NULL) {
        return;
    }
    lv_obj_del(ctx->picker);
    ctx->picker = NULL;
    ctx->picker_list = NULL;
}

static void radio_pick_cancel_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_CLICKED) {
        return;
    }
    radio_pick_close((radio_ctx_t *)lv_event_get_user_data(event));
}

static void radio_pick_scrim_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_CLICKED) {
        return;
    }
    radio_pick_close((radio_ctx_t *)lv_event_get_user_data(event));
}

/* Deleting the overlay deletes the rows, so the per-row payload goes with them. */
static void radio_pick_row_delete_cb(lv_event_t *event)
{
    radio_pick_row_t *row = (radio_pick_row_t *)lv_event_get_user_data(event);
    if (row != NULL) {
        heap_caps_free(row);
    }
}

static bool radio_pick_is_current(const radio_ctx_t *ctx, bool panel, const char *entity)
{
    if (panel != radio_panel_mode(ctx)) {
        return false;
    }
    if (panel) {
        return true;
    }
    return entity != NULL && strcmp(entity, radio_active_entity(ctx)) == 0;
}

/* Switch to the picked player: stop what plays, store the choice, rebuild. */
static void radio_pick_apply(radio_ctx_t *ctx, bool panel, const char *entity)
{
    radio_pick_close(ctx);

    if (radio_pick_is_current(ctx, panel, entity)) {
        return;
    }

    /* The stream that is playing belongs to the player that is going away. */
    radio_stop_playback(ctx);

    ctx->panel_mode = panel;
    ctx->player[0] = '\0';
    ctx->active[0] = '\0';
    ctx->playing_index = -1;
    ctx->pending = false;
    ctx->is_playing = false;
    ctx->now_text[0] = '\0';
    ctx->panel_state = PANEL_RADIO_IDLE;
    ctx->panel_state_known = false;
    ctx->panel_title[0] = '\0';
    ctx->panel_error[0] = '\0';

    if (panel) {
        ctx->config.player_entity_id[0] = '\0';
        ctx->volume_pct = radio_clamp_int(xz_audio_get_volume(), 0, 100);
    } else {
        snprintf(ctx->player, sizeof(ctx->player), "%s", entity);
        /* A tap is an explicit decision and has to survive radio_resolve_player(),
         * which applies the layout player again on every state refresh. */
        snprintf(ctx->config.player_entity_id, sizeof(ctx->config.player_entity_id), "%s", entity);
    }

    radio_show_state(ctx, "radio.stopped", "Zatrzymane", false);
    radio_prefs_save(ctx);
    radio_poll_control(ctx);
    radio_render(ctx);

    ESP_LOGI(TAG, "radio page '%s' player -> %s %s", ctx->config.page_id, panel ? "panel" : "ha",
        panel ? "" : entity);
    system_log_event(TAG, "radio player -> %s %s", panel ? "panel" : "ha", panel ? "" : entity);
}

static void radio_pick_row_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_CLICKED) {
        return;
    }
    radio_pick_row_t *row = (radio_pick_row_t *)lv_event_get_user_data(event);
    if (row == NULL || row->ctx == NULL) {
        return;
    }
    radio_pick_apply(row->ctx, row->panel, row->entity);
}

static void radio_pick_add_row(radio_ctx_t *ctx, lv_obj_t *list, const char *title, const char *subtitle,
    bool panel, const char *entity)
{
    radio_pick_row_t *row_data = ui_calloc_prefer_psram(1, sizeof(*row_data));
    if (row_data == NULL) {
        return;
    }
    row_data->ctx = ctx;
    row_data->panel = panel;
    if (!panel && entity != NULL) {
        snprintf(row_data->entity, sizeof(row_data->entity), "%s", entity);
    }

    const bool current = radio_pick_is_current(ctx, panel, entity);

    lv_obj_t *row = lv_btn_create(list);
    lv_obj_set_size(row, LV_PCT(100), RADIO_PICK_ROW_H);
    radio_style_button(row);
    lv_obj_set_style_radius(row, RADIO_TILE_RADIUS - 4, LV_PART_MAIN);
    if (current) {
        lv_obj_set_style_border_width(row, 2, LV_PART_MAIN);
        lv_obj_set_style_border_color(row, lv_color_hex(APP_UI_COLOR_STATE_ON), LV_PART_MAIN);
    }
    lv_obj_add_event_cb(row, radio_pick_row_cb, LV_EVENT_CLICKED, row_data);
    lv_obj_add_event_cb(row, radio_pick_row_delete_cb, LV_EVENT_DELETE, row_data);

    lv_obj_t *label = lv_label_create(row);
    lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_font(label, RADIO_BUTTON_FONT, LV_PART_MAIN);
    lv_obj_set_style_text_color(
        label, lv_color_hex(current ? APP_UI_COLOR_STATE_ON : APP_UI_COLOR_TEXT_PRIMARY), LV_PART_MAIN);
    lv_label_set_text(label, title != NULL ? title : "");
    lv_obj_set_width(label, LV_PCT(58));
    lv_obj_align(label, LV_ALIGN_LEFT_MID, 4, 0);

    if (subtitle == NULL || subtitle[0] == '\0') {
        return;
    }
    lv_obj_t *sub = lv_label_create(row);
    lv_label_set_long_mode(sub, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_font(sub, RADIO_STATE_FONT, LV_PART_MAIN);
    lv_obj_set_style_text_color(sub, lv_color_hex(APP_UI_COLOR_TEXT_MUTED), LV_PART_MAIN);
    lv_obj_set_style_text_align(sub, LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN);
    lv_label_set_text(sub, subtitle);
    lv_obj_set_width(sub, LV_PCT(36));
    lv_obj_align(sub, LV_ALIGN_RIGHT_MID, -4, 0);
}

static void radio_add_ha_player_rows(radio_ctx_t *ctx, lv_obj_t *list)
{
    /* One row per media_player the HA model knows; a stack copy would not fit. */
    ha_entity_info_t *infos = ui_calloc_prefer_psram(RADIO_PICK_MAX_ROWS, sizeof(ha_entity_info_t));
    if (infos == NULL) {
        return;
    }

    const size_t count = ha_model_list_entities("media_player", NULL, infos, RADIO_PICK_MAX_ROWS);
    for (size_t i = 0; i < count; i++) {
        if (infos[i].id[0] == '\0') {
            continue;
        }
        const char *short_id = infos[i].id;
        if (strncmp(short_id, "media_player.", 13) == 0) {
            short_id += 13;
        }
        const char *title = infos[i].name[0] != '\0' ? infos[i].name : short_id;
        radio_pick_add_row(ctx, list, title, short_id, false, infos[i].id);
    }
    heap_caps_free(infos);

    if (count == 0) {
        lv_obj_t *empty = lv_label_create(list);
        lv_obj_set_style_text_font(empty, RADIO_STATE_FONT, LV_PART_MAIN);
        lv_obj_set_style_text_color(empty, lv_color_hex(APP_UI_COLOR_TEXT_MUTED), LV_PART_MAIN);
        lv_label_set_long_mode(empty, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(empty, LV_PCT(100));
        lv_label_set_text(empty, ui_i18n_get("radio.player_none", "Brak odtwarzaczy media_player w HA"));
    }
}

static void radio_picker_open(radio_ctx_t *ctx);

static void radio_player_event_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_CLICKED) {
        return;
    }
    radio_picker_open((radio_ctx_t *)lv_event_get_user_data(event));
}

static void radio_picker_open(radio_ctx_t *ctx)
{
    if (ctx == NULL || ctx->root == NULL || ctx->picker != NULL) {
        return;
    }

    /* Dimming scrim over the whole page; a tap outside the card cancels. */
    lv_obj_t *overlay = lv_obj_create(ctx->root);
    lv_obj_remove_style_all(overlay);
    lv_obj_set_size(overlay, APP_CONTENT_BOX_WIDTH, APP_CONTENT_BOX_HEIGHT);
    lv_obj_set_pos(overlay, 0, 0);
    lv_obj_set_style_bg_color(overlay, lv_color_hex(APP_UI_COLOR_CONTENT_BG), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(overlay, LV_OPA_90, LV_PART_MAIN);
    lv_obj_clear_flag(overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(overlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(overlay, radio_pick_scrim_cb, LV_EVENT_CLICKED, ctx);
    ctx->picker = overlay;

    lv_coord_t card_w = RADIO_PICK_MAX_W;
    if (card_w > APP_CONTENT_BOX_WIDTH - 2 * RADIO_MARGIN - 20) {
        card_w = APP_CONTENT_BOX_WIDTH - 2 * RADIO_MARGIN - 20;
    }
    /* Never reach into the gutter: the chooser sits above the navigation bar. */
    lv_coord_t card_h = RADIO_PICK_MAX_H;
    if (card_h > APP_CONTENT_BOX_USABLE_HEIGHT - 20) {
        card_h = APP_CONTENT_BOX_USABLE_HEIGHT - 20;
    }

    lv_obj_t *card = lv_obj_create(overlay);
    lv_obj_remove_style_all(card);
    lv_obj_set_size(card, card_w, card_h);
    lv_obj_center(card);
    lv_obj_set_style_bg_color(card, lv_color_hex(APP_UI_COLOR_CARD_BG_OFF), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(card, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(card, lv_color_hex(APP_UI_COLOR_CARD_BORDER), LV_PART_MAIN);
    lv_obj_set_style_radius(card, RADIO_TILE_RADIUS, LV_PART_MAIN);
    lv_obj_set_style_pad_all(card, 16, LV_PART_MAIN);
    lv_obj_set_style_pad_row(card, 8, LV_PART_MAIN);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *title = lv_label_create(card);
    lv_obj_set_style_text_font(title, RADIO_HEADER_FONT, LV_PART_MAIN);
    lv_obj_set_style_text_color(title, lv_color_hex(APP_UI_COLOR_TEXT_PRIMARY), LV_PART_MAIN);
    lv_label_set_text(title, ui_i18n_get("radio.player_title", "Odtwarzacz"));

    lv_obj_t *hint = lv_label_create(card);
    lv_obj_set_style_text_font(hint, RADIO_STATE_FONT, LV_PART_MAIN);
    lv_obj_set_style_text_color(hint, lv_color_hex(APP_UI_COLOR_TEXT_MUTED), LV_PART_MAIN);
    lv_obj_set_width(hint, LV_PCT(100));
    lv_label_set_long_mode(hint, LV_LABEL_LONG_WRAP);
    lv_label_set_text(hint, ui_i18n_get("radio.player_hint", "Wybierz, gdzie ma grać stacja"));

    ctx->picker_list = lv_obj_create(card);
    lv_obj_remove_style_all(ctx->picker_list);
    lv_obj_set_width(ctx->picker_list, LV_PCT(100));
    lv_obj_set_flex_grow(ctx->picker_list, 1);
    lv_obj_set_style_bg_opa(ctx->picker_list, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(ctx->picker_list, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(ctx->picker_list, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_right(ctx->picker_list, 6, LV_PART_MAIN);
    lv_obj_set_style_pad_row(ctx->picker_list, RADIO_GAP, LV_PART_MAIN);
    lv_obj_set_flex_flow(ctx->picker_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(ctx->picker_list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_scroll_dir(ctx->picker_list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(ctx->picker_list, LV_SCROLLBAR_MODE_AUTO);

    radio_pick_add_row(ctx, ctx->picker_list, ui_i18n_get("radio.player_panel", "Głośnik panelu"),
        ui_i18n_get("radio.player_panel_hint", "w panelu"), true, NULL);
    radio_add_ha_player_rows(ctx, ctx->picker_list);

    lv_obj_t *cancel = lv_btn_create(card);
    lv_obj_set_size(cancel, LV_PCT(100), 44);
    radio_style_button(cancel);
    lv_obj_add_event_cb(cancel, radio_pick_cancel_cb, LV_EVENT_CLICKED, ctx);
    lv_obj_t *cancel_label = lv_label_create(cancel);
    lv_obj_set_style_text_font(cancel_label, RADIO_BUTTON_FONT, LV_PART_MAIN);
    lv_obj_set_style_text_color(cancel_label, lv_color_hex(APP_UI_COLOR_TEXT_PRIMARY), LV_PART_MAIN);
    lv_label_set_text(cancel_label, ui_i18n_get("radio.cancel", "Anuluj"));
    lv_obj_center(cancel_label);

    ESP_LOGI(TAG, "radio player chooser opened on page '%s' (current: %s)", ctx->config.page_id,
        radio_panel_mode(ctx) ? "panel" : "ha");
}

static void radio_build_grid(radio_ctx_t *ctx, lv_obj_t *parent, lv_coord_t width, lv_coord_t height, int columns)
{
    const lv_coord_t tile_w = (width - (lv_coord_t)(columns - 1) * RADIO_GAP) / (lv_coord_t)columns;

    ctx->grid = lv_obj_create(parent);
    lv_obj_set_size(ctx->grid, width, height);
    lv_obj_set_pos(ctx->grid, RADIO_MARGIN, RADIO_HEADER_H + RADIO_GAP);
    lv_obj_set_style_bg_opa(ctx->grid, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(ctx->grid, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(ctx->grid, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_row(ctx->grid, RADIO_GAP, LV_PART_MAIN);
    lv_obj_set_style_pad_column(ctx->grid, RADIO_GAP, LV_PART_MAIN);
    lv_obj_set_flex_flow(ctx->grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(ctx->grid, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_scroll_dir(ctx->grid, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(ctx->grid, LV_SCROLLBAR_MODE_AUTO);

    const size_t count = radio_station_count(ctx);
    for (size_t i = 0; i < count && i < UI_RADIO_MAX_STATIONS; i++) {
        lv_obj_t *tile = lv_btn_create(ctx->grid);
        if (tile == NULL) {
            break;
        }
        lv_obj_set_size(tile, tile_w, RADIO_TILE_H);
        radio_style_tile(tile);
        lv_obj_add_event_cb(tile, radio_tile_event_cb, LV_EVENT_CLICKED, ctx);
        ctx->tiles[i] = tile;

        lv_obj_t *label = lv_label_create(tile);
        lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(label, tile_w - 16);
        lv_obj_set_style_text_font(label, RADIO_TILE_FONT, LV_PART_MAIN);
        lv_obj_set_style_text_color(label, lv_color_hex(APP_UI_COLOR_TEXT_PRIMARY), LV_PART_MAIN);
        lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
        lv_label_set_text(label, radio_station_name(ctx, i));
        lv_obj_center(label);
        ctx->tile_labels[i] = label;
    }
}

static void radio_build_footer(radio_ctx_t *ctx, lv_obj_t *parent, lv_coord_t width)
{
    const lv_coord_t icon_x = 16;
    const lv_coord_t slider_x = icon_x + 44;
    const lv_coord_t label_x = slider_x + RADIO_SLIDER_W + 10;

    ctx->footer = lv_obj_create(parent);
    lv_obj_set_size(ctx->footer, width, RADIO_FOOTER_H);
    /* Pinned to the bottom of the usable area, not of the content box: a card
     * that reached the last row sat directly on the navigation bar's border and
     * looked cut off by the menu (same rule the runtime applies to widgets). */
    lv_obj_set_pos(ctx->footer, RADIO_MARGIN, APP_CONTENT_BOX_USABLE_HEIGHT - RADIO_FOOTER_H);
    lv_obj_set_style_bg_color(ctx->footer, lv_color_hex(APP_UI_COLOR_CARD_BG_OFF), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(ctx->footer, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(ctx->footer, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(ctx->footer, lv_color_hex(APP_UI_COLOR_CARD_BORDER), LV_PART_MAIN);
    lv_obj_set_style_radius(ctx->footer, RADIO_TILE_RADIUS, LV_PART_MAIN);
    lv_obj_set_style_pad_all(ctx->footer, 0, LV_PART_MAIN);
    lv_obj_clear_flag(ctx->footer, LV_OBJ_FLAG_SCROLLABLE);

    ctx->vol_icon = lv_label_create(ctx->footer);
    lv_obj_align(ctx->vol_icon, LV_ALIGN_LEFT_MID, icon_x, 0);
    lv_obj_set_style_text_color(ctx->vol_icon, lv_color_hex(APP_UI_COLOR_TEXT_MUTED), LV_PART_MAIN);
    (void)radio_set_icon(ctx->vol_icon, RADIO_ICON_VOLUME_HIGH, "VOL", APP_FONT_TEXT_14);

    ctx->vol_slider = lv_slider_create(ctx->footer);
    lv_obj_set_pos(ctx->vol_slider, slider_x, (RADIO_FOOTER_H - 14) / 2);
    lv_obj_set_size(ctx->vol_slider, RADIO_SLIDER_W, 14);
    lv_slider_set_range(ctx->vol_slider, 0, 100);
    lv_slider_set_value(ctx->vol_slider, ctx->volume_pct, LV_ANIM_OFF);
    lv_obj_set_style_radius(ctx->vol_slider, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_radius(ctx->vol_slider, LV_RADIUS_CIRCLE, LV_PART_INDICATOR);
    lv_obj_set_style_radius(ctx->vol_slider, LV_RADIUS_CIRCLE, LV_PART_KNOB);
    lv_obj_set_style_bg_color(ctx->vol_slider, lv_color_hex(APP_UI_COLOR_CONTENT_BG), LV_PART_MAIN);
    lv_obj_set_style_bg_color(ctx->vol_slider, lv_color_hex(APP_UI_COLOR_STATE_ON), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(ctx->vol_slider, lv_color_hex(APP_UI_COLOR_TEXT_PRIMARY), LV_PART_KNOB);
    lv_obj_set_style_border_width(ctx->vol_slider, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(ctx->vol_slider, 0, LV_PART_INDICATOR);
    lv_obj_set_style_border_width(ctx->vol_slider, 0, LV_PART_KNOB);
    lv_obj_set_style_pad_all(ctx->vol_slider, 0, LV_PART_MAIN);
    lv_obj_set_style_clip_corner(ctx->vol_slider, true, LV_PART_MAIN);
    lv_obj_set_style_outline_width(ctx->vol_slider, 0, LV_PART_KNOB);
    lv_obj_set_style_shadow_width(ctx->vol_slider, 0, LV_PART_KNOB);
    lv_obj_set_style_transform_width(ctx->vol_slider, -4, LV_PART_KNOB);
    lv_obj_set_style_transform_height(ctx->vol_slider, -4, LV_PART_KNOB);
    lv_obj_add_event_cb(ctx->vol_slider, radio_volume_event_cb, LV_EVENT_PRESSED, ctx);
    lv_obj_add_event_cb(ctx->vol_slider, radio_volume_event_cb, LV_EVENT_PRESSING, ctx);
    lv_obj_add_event_cb(ctx->vol_slider, radio_volume_event_cb, LV_EVENT_VALUE_CHANGED, ctx);
    lv_obj_add_event_cb(ctx->vol_slider, radio_volume_event_cb, LV_EVENT_RELEASED, ctx);
    lv_obj_add_event_cb(ctx->vol_slider, radio_volume_event_cb, LV_EVENT_PRESS_LOST, ctx);
    ui_slider_touch_enable(ctx->vol_slider);

    ctx->vol_label = lv_label_create(ctx->footer);
    lv_obj_set_style_text_font(ctx->vol_label, APP_FONT_TEXT_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(ctx->vol_label, lv_color_hex(APP_UI_COLOR_TEXT_MUTED), LV_PART_MAIN);
    lv_obj_align(ctx->vol_label, LV_ALIGN_LEFT_MID, label_x, 0);
    radio_update_volume_label(ctx);

    ctx->stop_btn = lv_btn_create(ctx->footer);
    lv_obj_set_size(ctx->stop_btn, RADIO_STOP_W, RADIO_FOOTER_H - 16);
    lv_obj_align(ctx->stop_btn, LV_ALIGN_RIGHT_MID, -8, 0);
    radio_style_button(ctx->stop_btn);
    lv_obj_add_event_cb(ctx->stop_btn, radio_stop_event_cb, LV_EVENT_CLICKED, ctx);
    ctx->stop_label = lv_label_create(ctx->stop_btn);
    lv_obj_set_style_text_font(ctx->stop_label, RADIO_BUTTON_FONT, LV_PART_MAIN);
    lv_obj_set_style_text_color(ctx->stop_label, lv_color_hex(APP_UI_COLOR_TEXT_PRIMARY), LV_PART_MAIN);
    if (!radio_set_icon(ctx->stop_label, RADIO_ICON_STOP, NULL, RADIO_BUTTON_FONT)) {
        lv_label_set_text(ctx->stop_label, ui_i18n_get("radio.stop", "STOP"));
    }
    lv_obj_center(ctx->stop_label);
}

/* ------------------------------------------------------------- public API */

esp_err_t ui_radio_page_create(
    const ui_radio_page_config_t *config, lv_obj_t *parent, ui_radio_page_instance_t *out_instance)
{
    if (config == NULL || parent == NULL || out_instance == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    radio_ctx_t *ctx = ui_calloc_prefer_psram(1, sizeof(radio_ctx_t));
    if (ctx == NULL) {
        ESP_LOGE(TAG, "no memory for the radio page context");
        return ESP_ERR_NO_MEM;
    }

    memcpy(&ctx->config, config, sizeof(ctx->config));
    if (ctx->config.page_id[0] == '\0') {
        /* Fallback id; ui_runtime normally passes the layout page id. */
        snprintf(ctx->config.page_id, sizeof(ctx->config.page_id), "radio");
    }
    if (ctx->config.columns < UI_RADIO_MIN_COLUMNS || ctx->config.columns > UI_RADIO_MAX_COLUMNS) {
        ctx->config.columns = UI_RADIO_DEFAULT_COLUMNS;
    }
    if (ctx->config.station_count > UI_RADIO_MAX_STATIONS) {
        ctx->config.station_count = UI_RADIO_MAX_STATIONS;
    }
    ctx->playing_index = -1;
    /* The layout picks the default player; a choice made on the panel (NVS)
     * overrides it, because the WebUI cannot know about the built-in speaker. */
    ctx->panel_mode = ctx->config.use_panel_speaker;
    radio_prefs_load(ctx);
    ctx->volume_pct = ctx->panel_mode ? radio_clamp_int(xz_audio_get_volume(), 0, 100) : 50;
    radio_show_state(ctx, "radio.stopped", "Zatrzymane", false);

    /* Root: fills the content box exactly, no border, no padding, no scroll. */
    ctx->root = lv_obj_create(parent);
    lv_obj_remove_style_all(ctx->root);
    lv_obj_set_size(ctx->root, APP_CONTENT_BOX_WIDTH, APP_CONTENT_BOX_HEIGHT);
    lv_obj_set_pos(ctx->root, 0, 0);
    lv_obj_clear_flag(ctx->root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(ctx->root, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(ctx->root, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(ctx->root, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(ctx->root, 0, LV_PART_MAIN);

    const lv_coord_t inner_w = APP_CONTENT_BOX_WIDTH - 2 * RADIO_MARGIN;
    /* The grid stops where the footer starts, and the footer stops one gutter
     * above the navigation bar (see radio_build_footer). */
    const lv_coord_t grid_h = APP_CONTENT_BOX_USABLE_HEIGHT - RADIO_HEADER_H - RADIO_FOOTER_H - 2 * RADIO_GAP;
    const int columns = (int)ctx->config.columns;

    /* Header strip: now playing + state + player entity. */
    lv_obj_t *header = lv_obj_create(ctx->root);
    lv_obj_set_size(header, inner_w, RADIO_HEADER_H);
    lv_obj_set_pos(header, RADIO_MARGIN, 0);
    lv_obj_set_style_bg_opa(header, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(header, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(header, 0, LV_PART_MAIN);
    lv_obj_clear_flag(header, LV_OBJ_FLAG_SCROLLABLE);

    ctx->now_label = lv_label_create(header);
    lv_obj_set_style_text_font(ctx->now_label, RADIO_HEADER_FONT, LV_PART_MAIN);
    lv_obj_set_style_text_color(ctx->now_label, lv_color_hex(APP_UI_COLOR_TEXT_PRIMARY), LV_PART_MAIN);
    lv_label_set_long_mode(ctx->now_label, LV_LABEL_LONG_DOT);
    lv_obj_set_width(ctx->now_label, inner_w - RADIO_NOW_W_INSET);
    lv_obj_align(ctx->now_label, LV_ALIGN_LEFT_MID, 0, 0);

    ctx->state_label = lv_label_create(header);
    lv_obj_set_style_text_font(ctx->state_label, RADIO_STATE_FONT, LV_PART_MAIN);
    lv_obj_set_style_text_color(ctx->state_label, lv_color_hex(APP_UI_COLOR_TEXT_MUTED), LV_PART_MAIN);
    lv_label_set_long_mode(ctx->state_label, LV_LABEL_LONG_DOT);
    lv_obj_set_width(ctx->state_label, RADIO_STATE_W);
    lv_obj_align(ctx->state_label, LV_ALIGN_LEFT_MID, inner_w - RADIO_STATE_X_INSET, 0);

    ctx->player_btn = lv_btn_create(header);
    lv_obj_set_size(ctx->player_btn, RADIO_ENTITY_W, RADIO_HEADER_H - 10);
    lv_obj_align(ctx->player_btn, LV_ALIGN_RIGHT_MID, 0, 0);
    radio_style_button(ctx->player_btn);
    lv_obj_add_event_cb(ctx->player_btn, radio_player_event_cb, LV_EVENT_CLICKED, ctx);

    /* The entity label doubles as the caption of the player chip: tapping it
     * opens the player chooser. */
    ctx->entity_label = lv_label_create(ctx->player_btn);
    lv_obj_set_style_text_font(ctx->entity_label, RADIO_STATE_FONT, LV_PART_MAIN);
    lv_obj_set_style_text_color(ctx->entity_label, lv_color_hex(APP_UI_COLOR_TEXT_SOFT), LV_PART_MAIN);
    lv_label_set_long_mode(ctx->entity_label, LV_LABEL_LONG_DOT);
    lv_obj_set_width(ctx->entity_label, RADIO_ENTITY_W - 20);
    lv_obj_set_style_text_align(ctx->entity_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_center(ctx->entity_label);

    radio_build_grid(ctx, ctx->root, inner_w, grid_h, columns);
    radio_build_footer(ctx, ctx->root, inner_w);

    /* "No player" card, shown instead of the grid. */
    ctx->msg_box = lv_obj_create(ctx->root);
    lv_obj_set_size(ctx->msg_box, inner_w, grid_h + RADIO_GAP + RADIO_FOOTER_H);
    lv_obj_set_pos(ctx->msg_box, RADIO_MARGIN, RADIO_HEADER_H + RADIO_GAP);
    lv_obj_set_style_bg_color(ctx->msg_box, lv_color_hex(APP_UI_COLOR_CARD_BG_OFF), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(ctx->msg_box, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(ctx->msg_box, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(ctx->msg_box, lv_color_hex(APP_UI_COLOR_CARD_BORDER), LV_PART_MAIN);
    lv_obj_set_style_radius(ctx->msg_box, RADIO_TILE_RADIUS, LV_PART_MAIN);
    lv_obj_clear_flag(ctx->msg_box, LV_OBJ_FLAG_SCROLLABLE);

    ctx->msg_label = lv_label_create(ctx->msg_box);
    lv_obj_set_style_text_font(ctx->msg_label, APP_FONT_TEXT_18, LV_PART_MAIN);
    lv_obj_set_style_text_color(ctx->msg_label, lv_color_hex(APP_UI_COLOR_TEXT_MUTED), LV_PART_MAIN);
    lv_obj_set_style_text_align(ctx->msg_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_label_set_long_mode(ctx->msg_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(ctx->msg_label, inner_w - 60);
    lv_label_set_text_fmt(ctx->msg_label, "%s\n\n%s", ui_i18n_get("radio.no_player", "Brak odtwarzacza w HA"),
        ui_i18n_get("radio.no_player_hint", "Dodaj odtwarzacz media_player do Home Assistant albo wskaż go w edytorze strony."));
    lv_obj_center(ctx->msg_label);

    memset(out_instance, 0, sizeof(*out_instance));
    snprintf(out_instance->page_id, sizeof(out_instance->page_id), "%s", ctx->config.page_id);
    out_instance->obj = ctx->root;
    out_instance->ctx = ctx;
    ctx->owner = out_instance;

    radio_register(ctx);
    lv_obj_add_event_cb(ctx->root, radio_delete_cb, LV_EVENT_DELETE, NULL);

    radio_resolve_player(ctx);
    radio_render(ctx);
    radio_poll_control(ctx);

    /* Prime the view from the HA model right away. */
    ui_radio_page_apply_all_states(out_instance);

    system_log_event(TAG, "radio page '%s' created", ctx->config.page_id);
    ESP_LOGI(TAG, "radio page '%s' created (%u stations, %d columns)", ctx->config.page_id,
        (unsigned)radio_station_count(ctx), columns);
    return ESP_OK;
}

void ui_radio_page_deinit(ui_radio_page_instance_t *instance)
{
    if (instance == NULL || instance->ctx == NULL) {
        return;
    }
    radio_ctx_t *ctx = (radio_ctx_t *)instance->ctx;
    instance->ctx = NULL;

    if (ctx->poll_timer != NULL) {
        lv_timer_del(ctx->poll_timer);
        ctx->poll_timer = NULL;
    }
    /* The overlay is a child of the page container and is deleted with it; the
     * pointers must not survive into radio_delete_cb(). */
    ctx->picker = NULL;
    ctx->picker_list = NULL;

    /* The LV_EVENT_DELETE handler of the root may run later; it must not touch
     * this context any more. */
    ctx->owner = NULL;
    radio_unregister(ctx);
    heap_caps_free(ctx);
}

void ui_radio_page_apply_state(ui_radio_page_instance_t *instance, const ha_state_t *state)
{
    if (instance == NULL || instance->ctx == NULL || state == NULL) {
        return;
    }
    radio_ctx_t *ctx = (radio_ctx_t *)instance->ctx;
    if (state->entity_id[0] == '\0') {
        return;
    }

    /* The panel decodes and plays on its own: media_player states of the HA
     * side say nothing about what the built-in speaker is doing. */
    if (radio_panel_mode(ctx)) {
        return;
    }

    bool relevant = false;
    if (ctx->player[0] != '\0' && strcmp(state->entity_id, ctx->player) == 0) {
        relevant = true;
    }
    if (!relevant && ctx->active[0] != '\0' && strcmp(state->entity_id, ctx->active) == 0) {
        relevant = true;
    }
    for (size_t i = 0; !relevant && i < (size_t)ctx->config.station_count; i++) {
        const char *entity = ctx->config.stations[i].entity;
        if (entity[0] != '\0' && strcmp(state->entity_id, entity) == 0) {
            relevant = true;
        }
    }
    if (!relevant) {
        return;
    }

    const bool playing = strcmp(state->state, "playing") == 0;
    const bool paused = strcmp(state->state, "paused") == 0;
    const bool known = strcmp(state->state, RADIO_STATE_IDLE) != 0 && strcmp(state->state, RADIO_STATE_OFF) != 0 &&
                       strcmp(state->state, RADIO_STATE_STANDBY) != 0 &&
                       strcmp(state->state, RADIO_STATE_UNAVAILABLE) != 0 &&
                       strcmp(state->state, RADIO_STATE_UNKNOWN) != 0;

    char content_id[UI_RADIO_STATION_URL_LEN] = {0};
    char media_title[APP_MAX_NAME_LEN] = {0};
    int volume_pct = ctx->volume_pct;
    bool have_volume = false;

    cJSON *attrs = cJSON_Parse(state->attributes_json);
    if (attrs != NULL) {
        const cJSON *item = cJSON_GetObjectItemCaseSensitive(attrs, "media_content_id");
        if (cJSON_IsString(item) && item->valuestring != NULL) {
            snprintf(content_id, sizeof(content_id), "%s", item->valuestring);
        }
        item = cJSON_GetObjectItemCaseSensitive(attrs, "media_title");
        if (cJSON_IsString(item) && item->valuestring != NULL) {
            snprintf(media_title, sizeof(media_title), "%s", item->valuestring);
        }
        item = cJSON_GetObjectItemCaseSensitive(attrs, "volume_level");
        if (cJSON_IsNumber(item)) {
            volume_pct = (int)(item->valuedouble * 100.0 + 0.5);
            have_volume = true;
        }
        item = cJSON_GetObjectItemCaseSensitive(attrs, "is_volume_muted");
        if (cJSON_IsBool(item) && cJSON_IsTrue(item)) {
            volume_pct = 0;
            have_volume = true;
        }
        cJSON_Delete(attrs);
    }

    /* Match the stream url of a tile so the highlight survives a rebuild or a
     * panel restart; fall back to the reported media title. */
    int index = -1;
    if (content_id[0] != '\0') {
        for (size_t i = 0; i < radio_station_count(ctx); i++) {
            const char *url = radio_station_url(ctx, i);
            if (url[0] != '\0' && strcmp(url, content_id) == 0) {
                index = (int)i;
                break;
            }
        }
    }
    if (index < 0 && media_title[0] != '\0') {
        for (size_t i = 0; i < radio_station_count(ctx); i++) {
            const char *name = radio_station_name(ctx, i);
            if (name[0] != '\0' && strcmp(name, media_title) == 0) {
                index = (int)i;
                break;
            }
        }
    }

    ctx->pending = false;
    ctx->playing_index = playing ? index : -1;
    ctx->is_playing = playing;
    if (playing) {
        const char *name = (index >= 0) ? radio_station_name(ctx, (size_t)index) : media_title;
        snprintf(ctx->now_text, sizeof(ctx->now_text), "%s", name);
    }

    if (playing) {
        radio_show_state(ctx, "radio.playing", "Odtwarzanie", true);
    } else if (paused) {
        radio_show_state(ctx, "radio.paused", "Pauza", false);
    } else if (!known) {
        radio_show_state(ctx, "lvgl.common.unavailable", "niedostępny", false);
    } else {
        radio_show_state(ctx, "radio.stopped", "Zatrzymane", false);
    }

    if (have_volume && !ctx->volume_dragging) {
        ctx->volume_pct = radio_clamp_int(volume_pct, 0, 100);
    }

    radio_render(ctx);
}

void ui_radio_page_apply_all_states(ui_radio_page_instance_t *instance)
{
    if (instance == NULL || instance->ctx == NULL) {
        return;
    }
    radio_ctx_t *ctx = (radio_ctx_t *)instance->ctx;

    /* Panel playback is polled, not subscribed: read the engine and stop. */
    if (radio_panel_mode(ctx)) {
        radio_poll_control(ctx);
        if (!ctx->volume_dragging) {
            ctx->volume_pct = radio_clamp_int(xz_audio_get_volume(), 0, 100);
        }
        radio_panel_sync(ctx, true);
        return;
    }

    radio_resolve_player(ctx);

    const char *entity = radio_active_entity(ctx);
    if (entity[0] == '\0') {
        radio_render(ctx);
        return;
    }

    ha_state_t state = {0};
    if (ha_model_get_state(entity, &state)) {
        ui_radio_page_apply_state(instance, &state);
        return;
    }
    radio_render(ctx);
}

void ui_radio_page_on_shown(const char *page_id)
{
    if (page_id == NULL || page_id[0] == '\0') {
        return;
    }
    for (size_t i = 0; i < s_live_count; i++) {
        radio_ctx_t *ctx = s_live[i];
        if (ctx == NULL || strncmp(ctx->config.page_id, page_id, sizeof(ctx->config.page_id)) != 0) {
            continue;
        }
        ui_radio_page_instance_t instance = {0};
        snprintf(instance.page_id, sizeof(instance.page_id), "%s", ctx->config.page_id);
        instance.obj = ctx->root;
        instance.ctx = ctx;
        ui_radio_page_apply_all_states(&instance);
    }
}
