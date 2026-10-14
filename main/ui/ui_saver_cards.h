/* SPDX-License-Identifier: LicenseRef-FNCL-1.1 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "lvgl.h"

/* Cards on the screensaver (remaining time, sport, now playing), built from
 * the entities with the cards label (see ha/ha_cards.h). LVGL task only. */

/* Creates the card row inside the saver overlay. */
void ui_saver_cards_attach(lv_obj_t *overlay);

/* The overlay was deleted: forget the handles (objects are gone already). */
void ui_saver_cards_detach(void);

/* Once a second from the saver tick. `visible`: overlay on the glass.
 * `clock_top`/`clock_bottom`: the clock block, the cards take the larger free
 * band above or below it. */
void ui_saver_cards_tick(bool visible, int32_t clock_top, int32_t clock_bottom);

/* True when (x, y) is on a card that takes touches (media controls): such a
 * touch must not dismiss the screensaver. */
bool ui_saver_cards_hit(int32_t x, int32_t y);
