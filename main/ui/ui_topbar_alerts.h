#pragma once

/* Red alert triangle in the top bar: shown while important notifications are
 * active (HA persistent notifications, configured warning entities such as
 * weather warnings). It blinks while something is unread; tapping it opens a
 * list of the entries, and after "Gelesen" it stays lit without blinking. */

#include <stdbool.h>

#include "lvgl.h"

#include "settings/runtime_settings.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Creates the (hidden) chip in the top bar; `relayout` re-runs the top bar
 * layout when the chip appears or disappears. */
lv_obj_t *ui_topbar_alerts_create(lv_obj_t *topbar, void (*relayout)(void));

/* Forgets the chip (the top bar is being rebuilt). */
void ui_topbar_alerts_reset(void);

void ui_topbar_alerts_apply_settings(const runtime_settings_t *settings);

bool ui_topbar_alerts_visible(void);

lv_obj_t *ui_topbar_alerts_chip(void);

/* Keeps the chip hidden (element switched off in the arrangement). */
void ui_topbar_alerts_set_suppressed(bool suppressed);

#ifdef __cplusplus
}
#endif
