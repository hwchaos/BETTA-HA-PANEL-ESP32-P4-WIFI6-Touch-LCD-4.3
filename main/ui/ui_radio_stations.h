/* SPDX-License-Identifier: LicenseRef-FNCL-1.1
 * Copyright (c) 2026 Cpt_Kirk
 *
 * Built-in internet-radio station table.
 *
 * The panel never decodes audio itself: a station is only a stream URL that is
 * handed to a Home Assistant media_player through media_player.play_media.
 * Music Assistant / HA can browse their own station library, but that needs a
 * player that supports browse_media and a configured radio provider.  This table
 * is the offline safety net: it makes the radio page usable on any setup,
 * including a panel whose HA has no radio library at all.
 */
#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    /* Shown on the station tile. */
    const char *name;
    /* Direct stream URL, passed as media_content_id with type "music". */
    const char *url;
} ui_radio_station_t;

/* Number of stations in the built-in table. */
size_t ui_radio_stations_count(void);

/* Station at `index`, or NULL when the index is out of range.  The returned
 * pointers live in flash (rodata) and stay valid for the whole runtime. */
const ui_radio_station_t *ui_radio_stations_get(size_t index);

#ifdef __cplusplus
}
#endif
