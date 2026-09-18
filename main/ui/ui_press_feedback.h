/* SPDX-License-Identifier: LicenseRef-FNCL-1.1
 * Copyright (c) 2026 Cpt_Kirk
 */
#pragma once

#include <stdbool.h>

#include "lvgl.h"

#include "settings/runtime_settings.h"

/* Tap feedback: while an interactive tile is held down LVGL puts it into
 * LV_STATE_PRESSED, and this module owns the matching style. Nothing else
 * touches the tile root, so the effect composes with the per-tile look without
 * fighting it, and it costs nothing at runtime when disabled. */

/* Loads the stored settings and applies them to any tile that already exists. */
void ui_press_feedback_init(void);

/* Applies new settings to tiles that are already on screen (no reboot). */
void ui_press_feedback_apply_settings(const runtime_settings_t *settings);

/* Registers a tile root so the pressed style follows later setting changes.
 * Non-interactive widget types are ignored. */
void ui_press_feedback_attach(lv_obj_t *tile, const char *widget_type);

/* True when the widget type reacts to a touch on the tile itself. */
bool ui_press_feedback_is_interactive(const char *widget_type);

/* Current mode name, e.g. "both"; used for logging and diagnostics. */
const char *ui_press_feedback_mode(void);
