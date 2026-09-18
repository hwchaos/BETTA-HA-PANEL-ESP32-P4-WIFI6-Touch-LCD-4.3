/* SPDX-License-Identifier: LicenseRef-FNCL-1.1
 * Copyright (c) 2026 Cpt_Kirk
 *
 * Internet radio page.
 *
 * A station is a stream URL plus the player that should play it.  The page can
 * use either of two players, chosen with the chip in the header (the choice is
 * stored in NVS and survives a reboot):
 *
 * - a Home Assistant media_player: the URL is handed over through
 *   media_player.play_media, so the page is a pure controller and works with
 *   whatever speakers the user already has in HA;
 * - the panel itself: main/radio/panel_radio.c streams and decodes the MP3 on
 *   the built-in ES8311 speaker, so the radio works with no speaker in HA.
 *
 * The station list comes from the page's own JSON object ("radio" in the layout
 * document).  When that list is empty the compiled-in table in
 * ui_radio_stations.c is used, so the page is usable on a fresh panel without
 * any configuration at all.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "lvgl.h"

#include "app_config.h"
#include "ha/ha_model.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Station list limits; the layout validator accepts the same numbers. */
#define UI_RADIO_MAX_STATIONS 24
#define UI_RADIO_MIN_COLUMNS 2
#define UI_RADIO_MAX_COLUMNS 4
#define UI_RADIO_DEFAULT_COLUMNS 3
/* Longest station name / stream URL a page keeps.  Anything longer is dropped
 * by the validator, so the tile label and the play_media body always fit. */
#define UI_RADIO_STATION_NAME_LEN 48
#define UI_RADIO_STATION_URL_LEN 160

/* One station of the page.  `entity` is optional: when set, that media_player
 * plays this station instead of the page's default player. */
typedef struct {
    char name[UI_RADIO_STATION_NAME_LEN];
    char url[UI_RADIO_STATION_URL_LEN];
    char entity[APP_MAX_ENTITY_ID_LEN];
} ui_radio_station_config_t;

/* Configuration of a "radio" page.  An empty player_entity_id makes the page
 * use the first media_player Home Assistant knows about; `columns` is clamped
 * to UI_RADIO_MIN_COLUMNS..UI_RADIO_MAX_COLUMNS. */
typedef struct {
    char page_id[APP_MAX_PAGE_ID_LEN];
    char title[APP_MAX_NAME_LEN];
    char player_entity_id[APP_MAX_ENTITY_ID_LEN];
    /* true = the panel decodes the stream on its own speaker, false = the URL
     * goes to a media_player.  A player picked on the panel overrides this. */
    bool use_panel_speaker;
    uint8_t columns;
    uint8_t station_count;
    ui_radio_station_config_t stations[UI_RADIO_MAX_STATIONS];
} ui_radio_page_config_t;

typedef struct {
    char page_id[APP_MAX_PAGE_ID_LEN];
    void *ctx;
    lv_obj_t *obj;
} ui_radio_page_instance_t;

/* Build the radio page inside `parent` (a page container). */
esp_err_t ui_radio_page_create(
    const ui_radio_page_config_t *config, lv_obj_t *parent, ui_radio_page_instance_t *out_instance);

/* Release the per-page context (PSRAM) and cancel pending HA callbacks. */
void ui_radio_page_deinit(ui_radio_page_instance_t *instance);

/* Refresh the "now playing" strip, the highlighted tile and the volume from the
 * page's current player.  apply_state() ignores states of other entities, so the
 * runtime may hand it every changed entity. */
void ui_radio_page_apply_state(ui_radio_page_instance_t *instance, const ha_state_t *state);
void ui_radio_page_apply_all_states(ui_radio_page_instance_t *instance);

/* Called when the page becomes visible: re-reads the current player state and
 * re-resolves the player when the page still had none. */
void ui_radio_page_on_shown(const char *page_id);

#ifdef __cplusplus
}
#endif