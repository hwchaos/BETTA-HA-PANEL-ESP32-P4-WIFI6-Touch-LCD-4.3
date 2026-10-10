/* SPDX-License-Identifier: LicenseRef-FNCL-1.1 */
#pragma once

/* Visual effects that go beyond a colour palette: a generated backdrop with
 * soft colour glows, translucent tiles with a hairline edge and a glowing
 * "on" state. Only the themes listed in theme_fx.c carry effects; for every
 * other theme theme_fx_active() returns NULL and nothing changes. */

#include <stdbool.h>
#include <stdint.h>

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t color;
    int16_t cx_pct; /* centre, percent of the screen width / height */
    int16_t cy_pct;
    int16_t radius_pct; /* falloff radius, percent of the screen width */
    uint8_t strength;   /* 0..255 peak intensity */
} theme_fx_glow_t;

typedef struct {
    /* Backdrop: vertical base gradient plus up to two radial glows, rendered
     * once into a dithered RGB565 image behind the whole screen. */
    uint32_t backdrop_top;
    uint32_t backdrop_bottom;
    theme_fx_glow_t glows[2];
    /* Top and bottom bar opacity over the backdrop. */
    lv_opa_t bar_opa;
    /* Tiles in their normal state. */
    lv_opa_t card_opa;
    uint32_t card_edge;
    lv_opa_t card_edge_opa;
    int16_t card_radius;
    /* Tiles in their "on" state: gradient fill, coloured edge and glow. */
    uint32_t on_from;
    uint32_t on_to;
    uint32_t on_edge;
    lv_opa_t on_edge_opa;
    uint32_t on_glow;
    lv_opa_t on_glow_opa;
    int16_t on_glow_width;
    /* > 0: "on" tiles breathe - glow and edge pulse slowly with this period. */
    uint16_t breathe_ms;
} theme_fx_t;

/* Effects of the active theme, or NULL when it has none. */
const theme_fx_t *theme_fx_active(void);

/* Full-screen backdrop image for the active theme (APP_SCREEN_WIDTH x
 * APP_SCREEN_HEIGHT), generated on first use and cached per theme. NULL when
 * the theme has no effects or the buffer could not be allocated. */
const lv_image_dsc_t *theme_fx_backdrop(void);

/* Applies the tile finish of the active theme. `is_on` selects the glowing
 * state. No-op without effects. */
void theme_fx_style_tile(lv_obj_t *tile, bool is_on);

/* True when the tile currently shows its "on" background (the widget painted
 * it with the palette's card_bg_on colour). */
bool theme_fx_tile_is_on(lv_obj_t *tile);

#ifdef __cplusplus
}
#endif
