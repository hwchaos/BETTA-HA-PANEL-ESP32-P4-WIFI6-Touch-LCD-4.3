/* SPDX-License-Identifier: LicenseRef-FNCL-1.1
 * Copyright (c) 2026 Cpt_Kirk
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "lvgl.h"
#include "settings/runtime_settings.h"

/* Page switch animation. Only the incoming page is animated: the outgoing page
 * is hidden immediately, so no handle to a disappearing page has to be kept
 * alive across the animation. */
void ui_page_transition_init(void);
/* Refresh the cached settings; called whenever the settings change at runtime. */
void ui_page_transition_apply_settings(const runtime_settings_t *settings);
/* Animate the incoming page in. `from_index < 0` (first show) and equal indexes
 * show the page instantly. */
void ui_page_transition_run(lv_obj_t *incoming, int from_index, int to_index);
/* Cancel a running transition and restore the container's start geometry. */
void ui_page_transition_reset(lv_obj_t *page);

const char *ui_page_transition_mode(void);
uint16_t ui_page_transition_duration_ms(void);
