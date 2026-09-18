/* SPDX-License-Identifier: LicenseRef-FNCL-1.1
 * Copyright (c) 2026 Cpt_Kirk
 */
#include "ui/ui_page_transition.h"

#include <string.h>

#include "diag/system_log.h"
#include "util/log_tags.h"

typedef enum {
    PAGE_TRANSITION_NONE = 0,
    PAGE_TRANSITION_FADE,
    PAGE_TRANSITION_SLIDE,
    PAGE_TRANSITION_SLIDE_UP,
    PAGE_TRANSITION_FADE_SLIDE,
} page_transition_kind_t;

static page_transition_kind_t s_kind = PAGE_TRANSITION_FADE;
static uint16_t s_duration_ms = APP_DISPLAY_PAGE_TRANSITION_DEFAULT_MS;
static bool s_settings_loaded = false;

static void animation_opa_cb(void *var, int32_t value)
{
    lv_obj_set_style_opa((lv_obj_t *)var, (lv_opa_t)value, LV_PART_MAIN);
}

static void animation_x_cb(void *var, int32_t value)
{
    lv_obj_set_x((lv_obj_t *)var, (lv_coord_t)value);
}

static void animation_y_cb(void *var, int32_t value)
{
    lv_obj_set_y((lv_obj_t *)var, (lv_coord_t)value);
}

static void animation_start(lv_obj_t *obj, lv_anim_exec_xcb_t exec_cb, int32_t from, int32_t to)
{
    lv_anim_t anim;
    lv_anim_init(&anim);
    lv_anim_set_var(&anim, obj);
    lv_anim_set_exec_cb(&anim, exec_cb);
    lv_anim_set_values(&anim, from, to);
    lv_anim_set_duration(&anim, s_duration_ms);
    lv_anim_set_path_cb(&anim, lv_anim_path_ease_out);
    lv_anim_start(&anim);
}

static page_transition_kind_t kind_from_name(const char *name)
{
    if (name == NULL) {
        return PAGE_TRANSITION_FADE;
    }
    if (strcmp(name, "none") == 0) {
        return PAGE_TRANSITION_NONE;
    }
    if (strcmp(name, "slide") == 0) {
        return PAGE_TRANSITION_SLIDE;
    }
    if (strcmp(name, "slide_up") == 0) {
        return PAGE_TRANSITION_SLIDE_UP;
    }
    if (strcmp(name, "fade_slide") == 0) {
        return PAGE_TRANSITION_FADE_SLIDE;
    }
    return PAGE_TRANSITION_FADE;
}

static void settings_load(void)
{
    runtime_settings_t settings;
    runtime_settings_set_defaults(&settings);
    if (runtime_settings_load(&settings) != ESP_OK) {
        runtime_settings_set_defaults(&settings);
    }
    s_settings_loaded = true;
    ui_page_transition_apply_settings(&settings);
}

static void settings_ensure_loaded(void)
{
    if (!s_settings_loaded) {
        settings_load();
    }
}

void ui_page_transition_init(void)
{
    settings_load();
}

void ui_page_transition_apply_settings(const runtime_settings_t *settings)
{
    if (settings == NULL) {
        return;
    }
    s_settings_loaded = true;
    s_kind = kind_from_name(settings->display_page_transition);
    s_duration_ms = settings->display_page_transition_ms;
}

void ui_page_transition_reset(lv_obj_t *page)
{
    if (page == NULL) {
        return;
    }
    /* Animations first: removing the property while one is still running would
     * let the next animation tick put it straight back. */
    lv_anim_delete(page, NULL);
    lv_obj_remove_local_style_prop(page, LV_STYLE_OPA, LV_PART_MAIN);
    lv_obj_set_pos(page, 0, 0);
}

void ui_page_transition_run(lv_obj_t *incoming, int from_index, int to_index)
{
    if (incoming == NULL) {
        return;
    }

    settings_ensure_loaded();

    /* A container that is still animating (fast repeated page changes) would
     * fight with the previous transition; always start from a clean state. */
    ui_page_transition_reset(incoming);

    if (s_kind == PAGE_TRANSITION_NONE || s_duration_ms == 0 || from_index < 0 || from_index == to_index) {
        return;
    }

    const bool forward = (to_index > from_index);
    const lv_coord_t height = lv_obj_get_height(incoming);
    lv_coord_t slide = lv_obj_get_width(incoming);
    if (slide <= 0) {
        slide = APP_CONTENT_BOX_WIDTH;
    }

    switch (s_kind) {
    case PAGE_TRANSITION_FADE:
        lv_obj_set_style_opa(incoming, LV_OPA_TRANSP, LV_PART_MAIN);
        animation_start(incoming, animation_opa_cb, LV_OPA_TRANSP, LV_OPA_COVER);
        break;
    case PAGE_TRANSITION_SLIDE:
        lv_obj_set_x(incoming, forward ? slide : -slide);
        animation_start(incoming, animation_x_cb, forward ? slide : -slide, 0);
        break;
    case PAGE_TRANSITION_SLIDE_UP:
        lv_obj_set_y(incoming, forward ? height : -height);
        animation_start(incoming, animation_y_cb, forward ? height : -height, 0);
        break;
    case PAGE_TRANSITION_FADE_SLIDE: {
        const lv_coord_t distance = slide / 4;
        lv_obj_set_style_opa(incoming, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_set_x(incoming, forward ? distance : -distance);
        animation_start(incoming, animation_opa_cb, LV_OPA_TRANSP, LV_OPA_COVER);
        animation_start(incoming, animation_x_cb, forward ? distance : -distance, 0);
        break;
    }
    case PAGE_TRANSITION_NONE:
    default:
        return;
    }

    system_log_write_info(TAG_PAGE_TRANSITION, "page %d -> %d: '%s' %u ms",
                          from_index, to_index, ui_page_transition_mode(), (unsigned)s_duration_ms);
}

const char *ui_page_transition_mode(void)
{
    switch (s_kind) {
    case PAGE_TRANSITION_NONE:
        return "none";
    case PAGE_TRANSITION_SLIDE:
        return "slide";
    case PAGE_TRANSITION_SLIDE_UP:
        return "slide_up";
    case PAGE_TRANSITION_FADE_SLIDE:
        return "fade_slide";
    case PAGE_TRANSITION_FADE:
    default:
        return "fade";
    }
}

uint16_t ui_page_transition_duration_ms(void)
{
    return s_duration_ms;
}
