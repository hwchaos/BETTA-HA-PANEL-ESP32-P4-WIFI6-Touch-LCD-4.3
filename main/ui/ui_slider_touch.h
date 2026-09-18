/* SPDX-License-Identifier: LicenseRef-FNCL-1.1
 * Copyright (c) 2026 Cpt_Kirk
 */
#pragma once

#include "lvgl.h"

/* A bare lv_slider claims a touch only when the finger lands on the knob, which
 * is nearly impossible to hit on a wall panel. These helpers widen the touch
 * band around the track: pressing anywhere in the band grabs the slider and the
 * value follows the finger, so a short tap anywhere on the track jumps straight
 * to that value. Scrolling a page by dragging over the band keeps working. */

/* Widened touch band with explicit padding, measured along and across the
 * track (along = x on a horizontal slider, y on a vertical one). */
void ui_slider_touch_enable_padded(lv_obj_t *slider, lv_coord_t pad_along, lv_coord_t pad_cross);

/* Convenience wrapper: keeps a finger friendly band (roughly 44 px across the
 * track) and follows the slider size, so sliders measured by a later layout pass
 * are covered too. */
void ui_slider_touch_enable(lv_obj_t *slider);
