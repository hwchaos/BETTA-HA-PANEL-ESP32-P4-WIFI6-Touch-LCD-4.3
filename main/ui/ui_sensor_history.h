/* SPDX-License-Identifier: LicenseRef-FNCL-1.1 */
#pragma once

/* History popup for sensor tiles: shows the Home Assistant history of one
 * entity as a line chart with 6 h / 24 h / 7 d ranges and min / max / average.
 * Data is fetched when the popup opens (recorder statistics first, raw
 * history as fallback); only one popup exists at a time. */

#ifdef __cplusplus
extern "C" {
#endif

/* Opens the popup (LVGL task only). `value_text` is the value the tile shows
 * right now and is displayed while the history loads. */
void ui_sensor_history_open(const char *entity_id, const char *title, const char *value_text);

#ifdef __cplusplus
}
#endif
