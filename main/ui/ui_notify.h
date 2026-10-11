/* SPDX-License-Identifier: LicenseRef-FNCL-1.1 */
#pragma once

/* Full-screen message pushed from Home Assistant (MQTT or HTTP), e.g. the
 * doorbell: large text, white on black alternating with black on white,
 * above everything else (screensaver, popups, "display off"). A tap ends it. */

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#define UI_NOTIFY_TEXT_MAX 128

typedef struct {
    char text[UI_NOTIFY_TEXT_MAX];
    uint32_t duration_ms; /* how long it stays (ignored when flashes > 0) */
    uint32_t interval_ms; /* time per colour phase; 0 = no blinking */
    uint32_t flashes;     /* number of inverted phases, 0 = use duration_ms */
    uint32_t fg1, bg1;    /* first phase: text / background (0xRRGGBB) */
    uint32_t fg2, bg2;    /* second (blink) phase; default: the first one swapped */
} ui_notify_req_t;

/* Parses a command payload: plain text ("Haustür") or JSON
 * {"text":"Haustür","duration":10,"interval":500,"count":6,
 *  "color":"#FFFFFF","background":"black","color2":..,"background2":..}
 * or {"stop":true}. Colours: #RRGGBB / RRGGBB or a few names.
 * *stop is set for a stop request (or empty text). */
esp_err_t ui_notify_parse(const char *payload, ui_notify_req_t *out, bool *stop);

/* Shows (or replaces) the message. Safe to call from any task. */
esp_err_t ui_notify_show(const ui_notify_req_t *req);

/* Ends a running message. Safe to call from any task. */
void ui_notify_stop(void);
