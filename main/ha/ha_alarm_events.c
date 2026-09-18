/* SPDX-License-Identifier: LicenseRef-FNCL-1.1
 * Copyright (c) 2026 Cpt_Kirk
 */
#include "ha/ha_alarm_events.h"

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"

const char *const ha_alarm_event_type_names[HA_ALARM_EVENT_TYPES] = {
    "alarmo_failed_to_arm",
    "alarmo_command_success",
    "alarmo_ready_to_arm_modes_updated",
};

_Static_assert(sizeof("alarmo_ready_to_arm_modes_updated") <= HA_ALARM_EVENT_TYPE_LEN,
               "HA_ALARM_EVENT_TYPE_LEN must fit the longest subscribed event type");

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static ha_alarm_event_t s_latest;
static bool s_have_event;

bool ha_alarm_events_matches(const char *event_type)
{
    if (event_type == NULL || event_type[0] == '\0') {
        return false;
    }
    for (size_t i = 0; i < HA_ALARM_EVENT_TYPES; i++) {
        if (strcmp(event_type, ha_alarm_event_type_names[i]) == 0) {
            return true;
        }
    }
    return false;
}

void ha_alarm_events_push(const char *event_type, const cJSON *data, int64_t unix_ms)
{
    if (!ha_alarm_events_matches(event_type)) {
        return;
    }

    ha_alarm_event_t next;
    memset(&next, 0, sizeof(next));
    snprintf(next.type, sizeof(next.type), "%s", event_type);
    next.unix_ms = unix_ms;

    if (cJSON_IsObject(data)) {
        /* cJSON_PrintPreallocated writes straight into the slot (no heap) and
         * returns false when the buffer is too small. */
        if (!cJSON_PrintPreallocated((cJSON *)data, next.payload, (int)sizeof(next.payload), 0)) {
            next.payload[0] = '\0';
        }
    }

    /* Single-slot store: a newer event replaces the old one, the reader only
     * compares seq numbers so it can never see a half-written payload. */
    portENTER_CRITICAL(&s_lock);
    next.seq = s_latest.seq + 1U;
    memcpy(&s_latest, &next, sizeof(s_latest));
    s_have_event = true;
    portEXIT_CRITICAL(&s_lock);
}

bool ha_alarm_events_take(uint32_t *last_seq, ha_alarm_event_t *out)
{
    if (last_seq == NULL || out == NULL) {
        return false;
    }

    bool fresh = false;
    portENTER_CRITICAL(&s_lock);
    if (s_have_event && s_latest.seq != *last_seq) {
        memcpy(out, &s_latest, sizeof(*out));
        *last_seq = s_latest.seq;
        fresh = true;
    }
    portEXIT_CRITICAL(&s_lock);
    return fresh;
}
