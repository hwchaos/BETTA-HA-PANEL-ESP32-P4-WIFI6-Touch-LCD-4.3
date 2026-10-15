/* SPDX-License-Identifier: LicenseRef-FNCL-1.1 */
#pragma once

/* Small SVG rasteriser for map images (robot mower / vacuum maps served as
 * SVG by Home Assistant camera entities).
 *
 * LVGL's SVG decoder draws through ThorVG on the LVGL task; a mower map took
 * 12 s there on the 10.1" panel and froze touch input for that long. This
 * renderer runs in the image fetcher task instead and hands LVGL a finished
 * bitmap. It covers what such maps use: path (all commands, curves
 * flattened, arcs as lines), polygon, polyline, line, rect, circle, ellipse,
 * nested groups with transform, fill / stroke / stroke-width / opacity
 * (attributes or style="..."), #rgb / #rrggbb / #rrggbbaa / rgb() / a few
 * colour names. Text is skipped, as are rects sized in percent (the usual
 * page background), so the map lies on a transparent background. No
 * anti-aliasing. */

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool zoom_main_shape;   /* fit the largest connected shape instead of the whole drawing */
    uint8_t min_value;      /* brighten colours darker than this HSV value (0 = keep) */
} svg_map_opts_t;

/* Renders `svg` (NUL-terminated) into a new w x h ARGB8888 buffer (straight
 * alpha, LVGL byte order B,G,R,A) in PSRAM. Returns NULL when the text is
 * not usable SVG or memory is short. Free with heap_caps_free(). */
uint8_t *svg_map_render(const char *svg, int w, int h, const svg_map_opts_t *opts);

#ifdef __cplusplus
}
#endif
