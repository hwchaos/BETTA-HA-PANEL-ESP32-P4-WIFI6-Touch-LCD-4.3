/* SPDX-License-Identifier: LicenseRef-FNCL-1.1 */
#pragma once

/* Any Material Design Icon ("mdi:desk") at any size: the full MDI webfont is
 * embedded and rendered at runtime through LVGL's Tiny TTF engine, so tiles can
 * show the same icons as Home Assistant. Without LV_USE_TINY_TTF every lookup
 * fails and callers keep their built-in glyphs. */

#include <stdbool.h>
#include <stdint.h>

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Codepoint for "mdi:name" or "name"; 0 when unknown. */
uint32_t ui_mdi_icon_codepoint(const char *name);

/* MDI font at `px` pixels (cached per size); NULL when unavailable. */
const lv_font_t *ui_mdi_icon_font(int32_t px);

/* Pixel size of `font` when it is one of the MDI fonts from
 * ui_mdi_icon_font(), else 0. */
int32_t ui_mdi_icon_font_px(const lv_font_t *font);

/* Puts the icon on `label` at `px`. Returns false (label untouched) when the
 * icon or the renderer is not available. */
bool ui_mdi_icon_apply(lv_obj_t *label, const char *name, int32_t px);

/* The icon to show for an entity, in order of preference: the tile's own icon
 * (`tile_icon`), the icon Home Assistant reports for the entity, then a
 * default derived from domain and device class like the HA frontend does.
 * `is_on` selects on/off variants. Never NULL; "" when nothing fits. */
const char *ui_mdi_icon_for_entity(const char *tile_icon, const char *entity_id, bool is_on);

/* Prefix search over the icon names for the web editor's picker. Writes up to
 * `max_out` names into `out` and returns how many were written. */
size_t ui_mdi_icon_search(const char *query, const char **out, size_t max_out);

#ifdef __cplusplus
}
#endif
