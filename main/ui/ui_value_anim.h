/* SPDX-License-Identifier: LicenseRef-FNCL-1.1
 * Copyright (c) 2026 Cpt_Kirk
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "lvgl.h"

#include "settings/runtime_settings.h"

/* Value animation: how a tile readout reacts when the entity state changes.
 * Widgets push new text through ui_value_anim_set_text() instead of
 * lv_label_set_text() and this module picks the effect, so the look can be
 * changed from the web editor without touching the widgets. With the effect
 * switched off the call is a plain lv_label_set_text(). */

/* Loads the stored settings. */
void ui_value_anim_init(void);

/* Applies new settings immediately (no reboot). */
void ui_value_anim_apply_settings(const runtime_settings_t *settings);

/* Sets the label text, animating the change according to the current mode.
 * Safe to call with a NULL label or text; an unchanged text is a no-op. */
void ui_value_anim_set_text(lv_obj_t *label, const char *text);

/* True while any animation started by this module is still running. */
bool ui_value_anim_is_running(void);

/* Current mode name, e.g. "count"; used for logging and diagnostics. */
const char *ui_value_anim_mode(void);

uint16_t ui_value_anim_duration_ms(void);
