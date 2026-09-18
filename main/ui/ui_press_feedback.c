/* SPDX-License-Identifier: LicenseRef-FNCL-1.1
 * Copyright (c) 2026 Cpt_Kirk
 */
#include "ui/ui_press_feedback.h"

#include <string.h>

#include "drivers/display_init.h"
#include "diag/system_log.h"
#include "util/log_tags.h"

/* The render task holds the display lock for the largest part of a frame, so a
 * user triggered change needs the same budget as the screenshot endpoint. */
#define PRESS_FX_LOCK_TIMEOUT_MS 1500U
/* Tiles registered for feedback; a busy page holds far fewer than this. */
#define PRESS_FX_MAX_TILES 96U

typedef enum {
    PRESS_FX_NONE = 0,
    PRESS_FX_DIM,
    PRESS_FX_SCALE,
    PRESS_FX_BOTH,
} press_fx_kind_t;

static press_fx_kind_t s_kind = PRESS_FX_BOTH;
static uint8_t s_dim_percent = APP_TILE_PRESS_FX_DIM_DEFAULT;
static uint8_t s_scale_percent = APP_TILE_PRESS_FX_SCALE_DEFAULT;
static lv_obj_t *s_tiles[PRESS_FX_MAX_TILES];
static bool s_settings_loaded;

static press_fx_kind_t kind_from_name(const char *name)
{
    if (name == NULL) {
        return PRESS_FX_NONE;
    }
    if (strcmp(name, "dim") == 0) {
        return PRESS_FX_DIM;
    }
    if (strcmp(name, "scale") == 0) {
        return PRESS_FX_SCALE;
    }
    if (strcmp(name, "both") == 0) {
        return PRESS_FX_BOTH;
    }
    return PRESS_FX_NONE;
}

const char *ui_press_feedback_mode(void)
{
    switch (s_kind) {
    case PRESS_FX_DIM:
        return "dim";
    case PRESS_FX_SCALE:
        return "scale";
    case PRESS_FX_BOTH:
        return "both";
    case PRESS_FX_NONE:
    default:
        return "none";
    }
}

bool ui_press_feedback_is_interactive(const char *widget_type)
{
    if (widget_type == NULL) {
        return false;
    }
    /* Only tiles whose root handles the touch: the other widgets react through
     * a child button/slider, so dimming the whole tile would be a lie. */
    return strcmp(widget_type, "light_tile") == 0 || strcmp(widget_type, "heating_tile") == 0 ||
           strcmp(widget_type, "button") == 0 || strcmp(widget_type, "cover_tile") == 0 ||
           strcmp(widget_type, "scene_tile") == 0 || strcmp(widget_type, "timer_tile") == 0;
}

static lv_opa_t pressed_opa(void)
{
    int opacity = 100 - (int)s_dim_percent;
    if (opacity < 0) {
        opacity = 0;
    }
    if (opacity > 100) {
        opacity = 100;
    }
    return (lv_opa_t)((opacity * 255) / 100);
}

static void style_clear(lv_obj_t *tile)
{
    const lv_style_selector_t pressed = LV_PART_MAIN | LV_STATE_PRESSED;
    lv_obj_remove_local_style_prop(tile, LV_STYLE_OPA, pressed);
    lv_obj_remove_local_style_prop(tile, LV_STYLE_TRANSFORM_SCALE_X, pressed);
    lv_obj_remove_local_style_prop(tile, LV_STYLE_TRANSFORM_SCALE_Y, pressed);
    lv_obj_remove_local_style_prop(tile, LV_STYLE_TRANSFORM_PIVOT_X, pressed);
    lv_obj_remove_local_style_prop(tile, LV_STYLE_TRANSFORM_PIVOT_Y, pressed);
}

static void style_apply(lv_obj_t *tile)
{
    style_clear(tile);

    const lv_style_selector_t pressed = LV_PART_MAIN | LV_STATE_PRESSED;
    /* A tile that is already translucent keeps its own level: the feedback only
     * ever fades further, never back up. */
    lv_opa_t opacity = pressed_opa();
    const lv_opa_t base = lv_obj_get_style_opa(tile, LV_PART_MAIN);
    if (base < opacity) {
        opacity = base;
    }
    if ((s_kind == PRESS_FX_DIM || s_kind == PRESS_FX_BOTH) && s_dim_percent > 0) {
        lv_obj_set_style_opa(tile, opacity, pressed);
    }
    if ((s_kind == PRESS_FX_SCALE || s_kind == PRESS_FX_BOTH) && s_scale_percent < 100) {
        const int32_t scale = (int32_t)(((uint32_t)s_scale_percent * 256U + 50U) / 100U);
        /* Percentage pivots keep the tile shrinking towards its own centre even
         * after a layout change resizes it. */
        lv_obj_set_style_transform_pivot_x(tile, lv_pct(50), pressed);
        lv_obj_set_style_transform_pivot_y(tile, lv_pct(50), pressed);
        lv_obj_set_style_transform_scale_x(tile, scale, pressed);
        lv_obj_set_style_transform_scale_y(tile, scale, pressed);
    }
}

static void tile_deleted_cb(lv_event_t *event)
{
    lv_obj_t *tile = lv_event_get_target(event);
    for (size_t i = 0; i < PRESS_FX_MAX_TILES; i++) {
        if (s_tiles[i] == tile) {
            s_tiles[i] = NULL;
        }
    }
}

static void remember(lv_obj_t *tile)
{
    for (size_t i = 0; i < PRESS_FX_MAX_TILES; i++) {
        if (s_tiles[i] == tile) {
            return;
        }
    }
    for (size_t i = 0; i < PRESS_FX_MAX_TILES; i++) {
        if (s_tiles[i] == NULL) {
            lv_obj_add_event_cb(tile, tile_deleted_cb, LV_EVENT_DELETE, NULL);
            s_tiles[i] = tile;
            return;
        }
    }
}

static void settings_load(void)
{
    /* Same reason as the top bar: the settings read runs from the page build path, so
     * the struct must not sit on the already deep main task stack. Callers run under
     * the display lock. */
    static runtime_settings_t settings;
    runtime_settings_set_defaults(&settings);
    if (runtime_settings_load(&settings) != ESP_OK) {
        runtime_settings_set_defaults(&settings);
    }
    ui_press_feedback_apply_settings(&settings);
}

static void settings_ensure_loaded(void)
{
    if (!s_settings_loaded) {
        settings_load();
    }
}

void ui_press_feedback_attach(lv_obj_t *tile, const char *widget_type)
{
    if (tile == NULL || !ui_press_feedback_is_interactive(widget_type)) {
        return;
    }
    settings_ensure_loaded();
    remember(tile);
    style_apply(tile);
}

void ui_press_feedback_apply_settings(const runtime_settings_t *settings)
{
    if (settings == NULL) {
        return;
    }

    const press_fx_kind_t kind = kind_from_name(settings->display_tile_press_fx);
    const uint8_t dim = settings->display_tile_press_fx_dim;
    uint8_t scale = settings->display_tile_press_fx_scale;
    if (scale > 100) {
        scale = 100;
    }
    const bool changed =
        !s_settings_loaded || kind != s_kind || dim != s_dim_percent || scale != s_scale_percent;

    s_settings_loaded = true;
    s_kind = kind;
    s_dim_percent = dim;
    s_scale_percent = scale;

    if (changed) {
        system_log_write_info(TAG_PRESS_FX, "tap feedback '%s' (dim %u%%, scale %u%%)",
                              ui_press_feedback_mode(), (unsigned)s_dim_percent,
                              (unsigned)s_scale_percent);
    }

    if (!display_lock(PRESS_FX_LOCK_TIMEOUT_MS)) {
        system_log_write(TAG_PRESS_FX, "tap feedback not applied: display lock busy");
        return;
    }
    for (size_t i = 0; i < PRESS_FX_MAX_TILES; i++) {
        if (s_tiles[i] != NULL) {
            style_apply(s_tiles[i]);
        }
    }
    display_unlock();
}

void ui_press_feedback_init(void)
{
    settings_load();
}
