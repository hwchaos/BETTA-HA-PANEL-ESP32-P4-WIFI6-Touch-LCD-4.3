#pragma once

/* Waste collection chips in the top bar: a bin icon per configured entity in
 * its own colour, with the days until the next pickup ("5T"); on the pickup
 * day the chip blinks. */

#include "lvgl.h"

#include "settings/runtime_settings.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Creates the (hidden) chips; `relayout` re-runs the top bar layout when a
 * chip appears or disappears. */
void ui_topbar_waste_create(lv_obj_t *topbar, void (*relayout)(void));

/* Forgets the chips (the top bar is being rebuilt). */
void ui_topbar_waste_reset(void);

/* Takes over entities and colours and subscribes the entities. */
void ui_topbar_waste_apply_settings(const runtime_settings_t *settings);

/* Places the visible chips from `left`; returns the next free x position. */
lv_coord_t ui_topbar_waste_layout(lv_coord_t left, lv_coord_t gap);

/* Total width of the visible chips including the gaps between them (0 = none). */
lv_coord_t ui_topbar_waste_width(lv_coord_t gap);

/* Hides the chips regardless of their data (element switched off in the arrangement). */
void ui_topbar_waste_set_suppressed(bool suppressed);

#ifdef __cplusplus
}
#endif
