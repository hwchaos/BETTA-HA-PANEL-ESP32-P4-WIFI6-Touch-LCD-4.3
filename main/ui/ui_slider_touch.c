/* SPDX-License-Identifier: LicenseRef-FNCL-1.1
 * Copyright (c) 2026 Cpt_Kirk
 */
#include "ui/ui_slider_touch.h"

#include "core/lv_obj_event_private.h"

/* A finger needs roughly 44 px of room. lv_slider keeps working on the whole
 * widget box, but its class only claims a touch while the finger sits on the
 * knob, so the touch band is widened here. */
#define UI_SLIDER_TOUCH_CROSS_TARGET LV_DPX(44)
#define UI_SLIDER_TOUCH_CROSS_MIN    LV_DPX(8) /* lv_slider's own ext_click_pad */
#define UI_SLIDER_TOUCH_CROSS_MAX    LV_DPX(16)

typedef struct {
    lv_coord_t pad_along; /* extra room along the track (x on horizontal sliders) */
    lv_coord_t pad_cross; /* extra room across the track (y on horizontal sliders) */
    bool pad_cross_auto;  /* pad_cross follows the size (sliders sized after us) */
} ui_slider_touch_ctx_t;

static bool ui_slider_touch_is_horizontal(lv_obj_t *slider)
{
    lv_slider_orientation_t orientation = lv_slider_get_orientation(slider);
    if (orientation == LV_SLIDER_ORIENTATION_VERTICAL) {
        return false;
    }
    if (orientation == LV_SLIDER_ORIENTATION_HORIZONTAL) {
        return true;
    }
    /* AUTO: lv_slider treats the wider side as the track. */
    return lv_obj_get_width(slider) >= lv_obj_get_height(slider);
}

static lv_coord_t ui_slider_touch_cross_pad(lv_obj_t *slider)
{
    const lv_coord_t cross =
        ui_slider_touch_is_horizontal(slider) ? lv_obj_get_height(slider) : lv_obj_get_width(slider);
    const lv_coord_t pad = (UI_SLIDER_TOUCH_CROSS_TARGET - cross) / 2;
    return LV_CLAMP(UI_SLIDER_TOUCH_CROSS_MIN, pad, UI_SLIDER_TOUCH_CROSS_MAX);
}

/* lv_obj_hit_test() discards the point before LV_EVENT_HIT_TEST is even sent
 * when it falls outside lv_obj_get_click_area(), so the object's own click area
 * has to cover the whole widened band too. It grows both axes by the same
 * amount, therefore the biggest padding decides. */
static void ui_slider_touch_apply_click_area(lv_obj_t *slider, const ui_slider_touch_ctx_t *ctx)
{
    lv_coord_t pad = LV_MAX(ctx->pad_along, ctx->pad_cross);
    if (pad < UI_SLIDER_TOUCH_CROSS_MIN) {
        pad = UI_SLIDER_TOUCH_CROSS_MIN;
    }
    lv_obj_set_ext_click_area(slider, pad);
}

static void ui_slider_touch_event_cb(lv_event_t *event)
{
    ui_slider_touch_ctx_t *ctx = (ui_slider_touch_ctx_t *)lv_event_get_user_data(event);
    lv_obj_t *slider = lv_event_get_target_obj(event);
    if (ctx == NULL || slider == NULL) {
        return;
    }

    switch (lv_event_get_code(event)) {
    case LV_EVENT_HIT_TEST: {
        lv_hit_test_info_t *info = lv_event_get_hit_test_info(event);
        if (info == NULL || info->point == NULL) {
            return;
        }
        /* info->point is a screen position (lv_obj_transform_point only undoes
         * transform matrices), exactly like lv_obj_get_coords(). The class has
         * already rejected the point when the knob was missed, so accept it here
         * for the full band and let LVGL move the knob on release. */
        const bool is_horizontal = ui_slider_touch_is_horizontal(slider);
        const lv_coord_t pad_x = is_horizontal ? ctx->pad_along : ctx->pad_cross;
        const lv_coord_t pad_y = is_horizontal ? ctx->pad_cross : ctx->pad_along;

        lv_area_t coords;
        lv_obj_get_coords(slider, &coords);
        if (info->point->x >= coords.x1 - pad_x && info->point->x <= coords.x2 + pad_x &&
            info->point->y >= coords.y1 - pad_y && info->point->y <= coords.y2 + pad_y) {
            info->res = true;
        }
        break;
    }
    case LV_EVENT_SIZE_CHANGED:
        if (ctx->pad_cross_auto) {
            ctx->pad_cross = ui_slider_touch_cross_pad(slider);
            ui_slider_touch_apply_click_area(slider, ctx);
        }
        break;
    case LV_EVENT_DELETE:
        lv_free(ctx);
        break;
    default:
        break;
    }
}

static void ui_slider_touch_attach(lv_obj_t *slider, lv_coord_t pad_along, lv_coord_t pad_cross, bool pad_cross_auto)
{
    if (slider == NULL) {
        return;
    }
    ui_slider_touch_ctx_t *ctx = lv_malloc(sizeof(*ctx));
    if (ctx == NULL) {
        return;
    }
    ctx->pad_along = pad_along < 0 ? 0 : pad_along;
    ctx->pad_cross = pad_cross < 0 ? 0 : pad_cross;
    ctx->pad_cross_auto = pad_cross_auto;

    /* LV_EVENT_HIT_TEST only reaches objects that ask for it and lv_slider keeps
     * the flag clear, so set it before widening the band. */
    lv_obj_add_flag(slider, LV_OBJ_FLAG_ADV_HITTEST);
    ui_slider_touch_apply_click_area(slider, ctx);

    lv_obj_add_event_cb(slider, ui_slider_touch_event_cb, LV_EVENT_HIT_TEST, ctx);
    lv_obj_add_event_cb(slider, ui_slider_touch_event_cb, LV_EVENT_SIZE_CHANGED, ctx);
    lv_obj_add_event_cb(slider, ui_slider_touch_event_cb, LV_EVENT_DELETE, ctx);
}

void ui_slider_touch_enable_padded(lv_obj_t *slider, lv_coord_t pad_along, lv_coord_t pad_cross)
{
    ui_slider_touch_attach(slider, pad_along, pad_cross, false);
}

void ui_slider_touch_enable(lv_obj_t *slider)
{
    if (slider == NULL) {
        return;
    }
    /* The cross padding follows the size because several sliders are measured by
     * the layout pass after this call. */
    ui_slider_touch_attach(slider, LV_DPX(8), ui_slider_touch_cross_pad(slider), true);
}
