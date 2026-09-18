/* SPDX-License-Identifier: LicenseRef-FNCL-1.1
 * Copyright (c) 2026 Cpt_Kirk
 *
 * Small store for the latest Alarmo event received over the HA websocket.
 *
 * Alarmo reports failures ("alarmo_failed_to_arm") that never show up in the
 * entity attributes, so the alarm tile has no other way to explain why arming
 * was refused. Events are tiny (a few dozen bytes), the store is one static
 * slot - no heap, no queues - so pulling them costs nothing when unused.
 */
#ifndef HA_ALARM_EVENTS_H
#define HA_ALARM_EVENTS_H

#include <stdbool.h>
#include <stdint.h>

#include "cJSON.h"

#define HA_ALARM_EVENT_TYPES 3
/* Must hold the longest subscribed name - "alarmo_ready_to_arm_modes_updated" is 33
 * characters - plus the terminator. A shorter buffer truncates the stored type and the
 * strcmp() done by the alarm tile then never matches. */
#define HA_ALARM_EVENT_TYPE_LEN 48
#define HA_ALARM_EVENT_PAYLOAD_LEN 512

/* Event types the client subscribes to (used both for the subscription and for
 * the fast "is this ours?" filter in the message handler). */
extern const char *const ha_alarm_event_type_names[HA_ALARM_EVENT_TYPES];

typedef struct {
    char type[HA_ALARM_EVENT_TYPE_LEN];
    /* Raw "event.data" object, already serialized ("" when the event has no data). */
    char payload[HA_ALARM_EVENT_PAYLOAD_LEN];
    int64_t unix_ms;
    uint32_t seq;
} ha_alarm_event_t;

/* True when the event type is one of the Alarmo ones this store keeps. */
bool ha_alarm_events_matches(const char *event_type);

/* Store the newest event. Called from the HA websocket task; never blocks. */
void ha_alarm_events_push(const char *event_type, const cJSON *data, int64_t unix_ms);

/* Copy the newest event into *out when its seq differs from *last_seq and
 * update *last_seq - the "any new event?" test polled by the widgets' timers. */
bool ha_alarm_events_take(uint32_t *last_seq, ha_alarm_event_t *out);

#endif /* HA_ALARM_EVENTS_H */
