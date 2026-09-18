/* SPDX-License-Identifier: LicenseRef-FNCL-1.1
 * Copyright (c) 2026 Cpt_Kirk
 */
#pragma once

#include <stdbool.h>

#include "esp_err.h"
#include "lvgl.h"

#include "app_config.h"
#include "ha/ha_model.h"

#define UI_MUSIC_MAX_PLAYERS 16

/*
 * Configuration for a full-screen "music_assistant" page. The page renders a
 * large Now Playing view: album art (with dominant-color accenting), title /
 * artist, transport controls, a seek/position bar and a volume slider. The
 * player selector chip cycles through the configured media players.
 *
 * When `players` is empty the page auto-discovers media_player entities from
 * the HA model. `player_entity_id` (optional) picks the initial player.
 */
typedef struct {
    char page_id[APP_MAX_PAGE_ID_LEN];
    char title[APP_MAX_NAME_LEN];
    char player_entity_id[APP_MAX_ENTITY_ID_LEN];
    char players[UI_MUSIC_MAX_PLAYERS][APP_MAX_ENTITY_ID_LEN];
    int player_count;
} ui_music_page_config_t;

typedef struct {
    char page_id[APP_MAX_PAGE_ID_LEN];
    void *ctx;
    lv_obj_t *obj;
} ui_music_page_instance_t;

esp_err_t ui_music_page_create(
    const ui_music_page_config_t *config, lv_obj_t *parent, ui_music_page_instance_t *out_instance);
bool ui_music_page_apply_state(ui_music_page_instance_t *instance, const ha_state_t *state);
/* Apply the state and report whether it is a transition to "playing" for the
 * page's current player. Used to auto-open the music tab when playback starts. */
bool ui_music_page_apply_state_detect_play_start(ui_music_page_instance_t *instance, const ha_state_t *state);
void ui_music_page_apply_all_states(ui_music_page_instance_t *instance);
