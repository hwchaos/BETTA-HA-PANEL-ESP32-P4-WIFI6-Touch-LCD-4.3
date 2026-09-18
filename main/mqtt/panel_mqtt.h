/* SPDX-License-Identifier: LicenseRef-FNCL-1.1
 * Copyright (c) 2026 Cpt_Kirk
 */
#pragma once

#include <stdbool.h>

#include "settings/runtime_settings.h"

void panel_mqtt_init(void);
void panel_mqtt_apply_settings(const runtime_settings_t *settings);
void panel_mqtt_notify_settings_changed(void);
bool panel_mqtt_is_connected(void);

/* Broker URI last handed to the MQTT client, e.g. "mqtts://homeassistant.local:8883".
 * Empty when MQTT is disabled or no host could be derived. */
const char *panel_mqtt_broker_uri(void);
