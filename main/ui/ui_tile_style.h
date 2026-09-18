/* SPDX-License-Identifier: LicenseRef-FNCL-1.1
 * Copyright (c) 2026 Cpt_Kirk
 */
#pragma once

#include <stdbool.h>

#include "lvgl.h"

/* Widgets tag their labels with LV_OBJ_FLAG_USER_1 (title), LV_OBJ_FLAG_USER_2
 * (entity name/description) and LV_OBJ_FLAG_USER_3 (state/value readout) so the
 * per-role colours below land on the right label. */

/* Per-tile visual overrides coming from the layout JSON ("tile_*" keys).
 * Empty strings and -1 mean "inherit from the active theme", so a tile with a
 * fully empty style renders exactly like before this feature existed. */
typedef struct {
    const char *bg_color;      /* "" = theme card background */
    const char *bg_grad_color; /* "" = solid background */
    const char *bg_grad_dir;   /* "none" | "hor" | "ver" */
    const char *border_color;  /* "" = theme border */
    const char *text_color;    /* "" = theme text colour (all non-icon labels) */
    const char *title_color;   /* "" = follow text_color (tile title label) */
    const char *label_color;   /* "" = follow text_color (entity name/description) */
    const char *value_color;   /* "" = follow text_color (state/value readout) */
    const char *icon_color;    /* "" = theme icon colour (MDI/LV_SYMBOL glyphs) */
    const char *font_scale;    /* "auto" | "s" | "m" | "l" | "xl" */
    int border_width;          /* -1 = theme border width */
    int radius;                /* -1 = theme corner radius */
    int opacity;               /* -1 = theme background opacity, else 0..100 */
    bool shadow;               /* true = force a soft drop shadow */
} ui_tile_style_t;

/* Returns true when nothing at all would be overridden. */
bool ui_tile_style_is_empty(const ui_tile_style_t *style);

/* Parses "#RRGGBB"/"RRGGBB"/"0xRRGGBB" into 0xRRGGBB. False when the text is
 * empty or malformed. Shared with the per-page background styling. */
bool ui_tile_style_hex_to_rgb(const char *text, uint32_t *out_rgb);

/* Applies background, border, radius, opacity, shadow, text/icon colours and the
 * font scale. Call after the widget has built/updated itself. */
void ui_tile_style_apply(lv_obj_t *tile, const ui_tile_style_t *style);

/* Cheap subset used after every state update: background colour + gradient only. */
void ui_tile_style_apply_bg(lv_obj_t *tile, const ui_tile_style_t *style);

/* Cheap, idempotent subset used after every state update: the text/icon colours
 * only. Widgets repaint their labels with theme colours on each update, so this
 * has to run again after their state handler to keep the per-role overrides. */
void ui_tile_style_apply_text_colors(lv_obj_t *tile, const ui_tile_style_t *style);

/* Same as ui_tile_style_apply_text_colors(), but leaves icon glyphs to the
 * widget. Widgets that colour their icon from the entity state (lights, power
 * buttons, covers, scenes, heating, persons) call this after a state update,
 * otherwise the static override would mask the on/off colour and e.g. keep a
 * switched off light yellow. */
void ui_tile_style_apply_state_text_colors(lv_obj_t *tile, const ui_tile_style_t *style);
