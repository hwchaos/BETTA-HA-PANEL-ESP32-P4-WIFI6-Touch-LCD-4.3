#pragma once

/* On-screen side of the HA Assist voice control: the microphone chip in the
 * top bar (hold to talk, or tap and speak until the pause is detected) and the
 * speech bubble that shows what was understood and the answer. */

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Turns `chip` (a top bar label) into the push-to-talk button. */
void ui_assist_attach_chip(lv_obj_t *chip);

#ifdef __cplusplus
}
#endif
