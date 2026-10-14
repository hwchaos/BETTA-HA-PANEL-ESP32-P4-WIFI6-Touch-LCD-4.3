/* SPDX-License-Identifier: LicenseRef-FNCL-1.1 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cJSON.h"

/* Cards ("Google Now" style) are fed by the entities carrying one HA label.
 * A render_template subscription lists them with their device, state and
 * attributes; HA re-renders it whenever one of them changes. */

/* Desired label / on-off. Safe from any task; the subscription itself is
 * (re)sent by ha_cards_tick(). */
void ha_cards_configure(const char *label, bool enabled);

/* Call periodically (UI timer): subscribes once the HA client is up. */
void ha_cards_tick(void);

/* Increases on every new rendering (or error). */
uint32_t ha_cards_seq(void);

/* Parsed copy of the latest entity list (JSON array of
 * {"i","s","n","d","dn","t","a":[[key,value],...]}), or NULL. Caller frees with
 * cJSON_Delete(). */
cJSON *ha_cards_entities(void);

/* Demo data (one card of every kind) until the next real rendering; off
 * fetches the real list again. For previewing the cards. */
void ha_cards_demo(bool on);

/* Status for the diagnostics endpoint: label, subscribed, error, seq, the raw
 * entity list. */
void ha_cards_status_json(cJSON *root);
