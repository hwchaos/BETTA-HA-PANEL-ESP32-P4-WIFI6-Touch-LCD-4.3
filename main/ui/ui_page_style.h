/* SPDX-License-Identifier: LicenseRef-FNCL-1.1
 * Copyright (c) 2026 Cpt_Kirk
 */
#pragma once

#include <stdbool.h>

#include "app_config.h"
#include "lvgl.h"

/* Per-page background look coming from the layout JSON ("page_*" keys):
 *   page_bg_color       "#RRGGBB" solid background ("" = panel background)
 *   page_bg_grad_color  "#RRGGBB" gradient end colour ("" = no gradient)
 *   page_bg_grad_dir    "none" | "hor" | "ver"
 *   page_wallpaper      true = paint the screensaver wallpaper behind the tiles
 *   page_dim            0..90: darkens the wallpaper so tiles stay readable
 * A fully empty style leaves the page container transparent, so the page looks
 * exactly like before this feature existed. */
typedef struct {
    char bg_color[APP_MAX_COLOR_STR_LEN];
    char bg_grad_color[APP_MAX_COLOR_STR_LEN];
    char bg_grad_dir[APP_MAX_UI_OPTION_LEN];
    bool wallpaper;
    int dim;
} ui_page_style_t;

/* Fills the struct with "nothing configured". */
void ui_page_style_init(ui_page_style_t *style);

/* Returns true when nothing at all would be painted. */
bool ui_page_style_is_empty(const ui_page_style_t *style);

/* Forgets the page containers that paint the wallpaper. Call it while the pages
 * are rebuilt: the containers are deleted and the stale pointers must go. */
void ui_page_style_reset(void);

/* Applies the look to a page container (the object returned by ui_pages_add). */
void ui_page_style_apply(lv_obj_t *page_container, const ui_page_style_t *style);

/* Re-points (or clears) every wallpaper page after the wallpaper buffer was
 * replaced or deleted. Requires the display lock to be held. */
void ui_page_style_reload_wallpaper(void);
