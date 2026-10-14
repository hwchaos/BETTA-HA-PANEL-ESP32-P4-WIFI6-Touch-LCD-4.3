/* SPDX-License-Identifier: LicenseRef-FNCL-1.1 */
/* Lawn mower tile for a lawn_mower.* entity.
 *
 *   ┌──────────────────────────────┐
 *   │ Title                 ▮ 87 % │
 *   │ ┌──────────────────────────┐ │
 *   │ │   map (camera.*) or icon │ │
 *   │ └──────────────────────────┘ │
 *   │ Mowing · Zone 1              │
 *   │ ████████░░░░░░░░░░░░   45 %  │
 *   │ [ ⏸ Pause ]   [ ⌂ Dock ]     │
 *   └──────────────────────────────┘
 *
 * Layout fields:
 *   "entity_id":                lawn_mower.*
 *   "secondary_entity_id":      camera.* with the map (optional)
 *   "mower_battery_entity_id":  battery sensor in % (optional; else the
 *                               mower's battery_level attribute)
 *   "mower_progress_entity_id": mowing progress sensor in % (optional)
 *   "mower_task_entity_id":     current task sensor (optional)
 *
 * Buttons call lawn_mower.start_mowing / pause / dock. The map is fetched
 * through HA's camera proxy, every 10 s while mowing and every 2 min
 * otherwise, and only while the tile is on screen.
 */
#include "ui/ui_widget_factory.h"

#include <ctype.h>
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "image/lv_image_decoder_private.h"
#include "misc/cache/instance/lv_image_cache.h"

#include "ha/ha_client.h"
#include "ha/ha_cover_fetcher.h"
#include "ha/ha_model.h"
#include "ui/fonts/app_text_fonts.h"
#include "ui/theme/theme_default.h"
#include "ui/ui_i18n.h"
#include "ui/ui_mdi_icon.h"
#include "ui/ui_memory.h"
#include "ui/widgets/w_widget_util.h"

static const char *TAG = "w_mower";

#define MOWER_MAP_ACTIVE_MS 10000
#define MOWER_MAP_IDLE_MS 120000
#define MOWER_MAP_RETRY_MS 30000
/* vector maps: zoom on the lawn up to this factor, keep this margin */
#define MOWER_MAP_MAX_ZOOM 8
#define MOWER_MAP_MARGIN 4
#define MOWER_MAP_CELL 4
/* darkest colour value (HSV) a vector map keeps on the dark tile */
#define MOWER_MAP_MIN_VALUE 0xB0
#define MOWER_COLOR_ACTIVE 0x4CD964
#define MOWER_COLOR_ERROR 0xFF5A52
#define MOWER_COLOR_BAR 0x4FC3F7

typedef enum {
    MOWER_ST_UNKNOWN = 0,
    MOWER_ST_MOWING,
    MOWER_ST_DOCKED,
    MOWER_ST_PAUSED,
    MOWER_ST_RETURNING,
    MOWER_ST_ERROR,
} mower_state_t;

typedef struct {
    char entity_id[APP_MAX_ENTITY_ID_LEN];
    char map_entity[APP_MAX_ENTITY_ID_LEN];
    char battery_entity[APP_MAX_ENTITY_ID_LEN];
    char progress_entity[APP_MAX_ENTITY_ID_LEN];
    char task_entity[APP_MAX_ENTITY_ID_LEN];

    lv_obj_t *card;
    lv_obj_t *battery_icon;
    lv_obj_t *battery_label;
    lv_obj_t *map_box;
    lv_obj_t *map_img;
    lv_obj_t *map_icon;
    lv_obj_t *status_label;
    lv_obj_t *bar;
    lv_obj_t *bar_label;
    lv_obj_t *btn_main;
    lv_obj_t *btn_main_icon;
    lv_obj_t *btn_main_text;
    lv_obj_t *btn_dock;
    lv_timer_t *timer;
    int32_t map_w;
    int32_t map_h;
    int32_t icon_px;

    mower_state_t state;
    bool unavailable;
    bool visible;
    /* last shown values, so unchanged data does not repaint the tile */
    char shown_status[128];
    char shown_battery[16];
    int shown_battery_icon; /* -1 none, 0 battery, 1 charging */
    int shown_progress;     /* -2 never, -1 hidden, 0..100 */
    int shown_main;         /* -1 never, 0 start, 1 pause, 2 resume */

    /* map: raster image, or a vector map rendered once into a canvas */
    lv_image_dsc_t map_dsc;
    void *map_buf;
    lv_obj_t *map_canvas;
    lv_draw_buf_t *map_canvas_buf;
    bool map_fetching;
    int64_t map_next_ms;
} w_mower_ctx_t;

/* ------------------------------------------------------------------ data */

static int64_t mower_now_ms(void)
{
    return esp_timer_get_time() / 1000;
}

static mower_state_t mower_parse_state(const char *s)
{
    if (s == NULL) {
        return MOWER_ST_UNKNOWN;
    }
    if (strcmp(s, "mowing") == 0) {
        return MOWER_ST_MOWING;
    }
    if (strcmp(s, "docked") == 0) {
        return MOWER_ST_DOCKED;
    }
    if (strcmp(s, "paused") == 0) {
        return MOWER_ST_PAUSED;
    }
    if (strcmp(s, "returning") == 0) {
        return MOWER_ST_RETURNING;
    }
    if (strcmp(s, "error") == 0) {
        return MOWER_ST_ERROR;
    }
    return MOWER_ST_UNKNOWN;
}

static const char *mower_state_text(mower_state_t st, const char *raw)
{
    switch (st) {
    case MOWER_ST_MOWING:
        return ui_i18n_get("mower.mowing", "Mowing");
    case MOWER_ST_DOCKED:
        return ui_i18n_get("mower.docked", "Docked");
    case MOWER_ST_PAUSED:
        return ui_i18n_get("mower.paused", "Paused");
    case MOWER_ST_RETURNING:
        return ui_i18n_get("mower.returning", "Returning");
    case MOWER_ST_ERROR:
        return ui_i18n_get("mower.error", "Error");
    default:
        return (raw != NULL && raw[0] != '\0') ? raw : "--";
    }
}

/* Numeric state of a sensor, or NAN. */
static float mower_sensor_value(const char *entity_id)
{
    if (entity_id == NULL || entity_id[0] == '\0') {
        return NAN;
    }
    static ha_state_t st; /* LVGL task only */
    if (!ha_model_get_state(entity_id, &st) || w_state_is_unavailable(st.state)) {
        return NAN;
    }
    char *end = NULL;
    float v = strtof(st.state, &end);
    return (end != st.state && isfinite(v)) ? v : NAN;
}

/* Text state of a sensor ("" when unusable). */
static void mower_sensor_text(const char *entity_id, char *out, size_t len)
{
    out[0] = '\0';
    if (entity_id == NULL || entity_id[0] == '\0') {
        return;
    }
    static ha_state_t st; /* LVGL task only */
    if (ha_model_get_state(entity_id, &st) && !w_state_is_unavailable(st.state)) {
        snprintf(out, len, "%s", st.state);
    }
}

/* battery_level attribute of the mower itself, or NAN. */
static float mower_attr_battery(const ha_state_t *st)
{
    cJSON *attrs = cJSON_Parse(st->attributes_json);
    float v = NAN;
    const cJSON *b = cJSON_GetObjectItemCaseSensitive(attrs, "battery_level");
    if (cJSON_IsNumber(b)) {
        v = (float)b->valuedouble;
    }
    cJSON_Delete(attrs);
    return v;
}

/* ------------------------------------------------------------------ map */

#if LV_USE_SVG
/* Numeric attribute of the <svg> tag (plain user units only). */
static bool mower_svg_attr(const char *tag, const char *tag_end, const char *name, float *out, int count)
{
    const size_t name_len = strlen(name);
    for (const char *p = tag; p != NULL && p < tag_end; p++) {
        p = strstr(p, name);
        if (p == NULL || p >= tag_end) {
            return false;
        }
        if ((p > tag && p[-1] != ' ' && p[-1] != '\t' && p[-1] != '\n' && p[-1] != '\r') ||
            strncmp(p + name_len, "=\"", 2) != 0) {
            continue;
        }
        const char *v = p + name_len + 2;
        for (int i = 0; i < count; i++) {
            char *end = NULL;
            out[i] = strtof(v, &end);
            if (end == v || end > tag_end) {
                return false;
            }
            v = end;
            while (*v == ' ' || *v == ',') {
                v++;
            }
        }
        /* a percentage says nothing about the drawing's size */
        return *v != '%';
    }
    return false;
}

/* The drawing's own size: width/height, else the viewBox. */
static bool mower_svg_size(const char *svg, int32_t *w, int32_t *h)
{
    const char *tag = strstr(svg, "<svg");
    const char *tag_end = tag != NULL ? strchr(tag, '>') : NULL;
    if (tag_end == NULL) {
        return false;
    }
    float wh[2];
    float vb[4];
    if (mower_svg_attr(tag, tag_end, "width", &wh[0], 1) && mower_svg_attr(tag, tag_end, "height", &wh[1], 1) &&
        wh[0] >= 1.0f && wh[1] >= 1.0f) {
        *w = (int32_t)lroundf(wh[0]);
        *h = (int32_t)lroundf(wh[1]);
        return true;
    }
    if (mower_svg_attr(tag, tag_end, "viewBox", vb, 4) && vb[2] >= 1.0f && vb[3] >= 1.0f) {
        *w = (int32_t)lroundf(vb[2]);
        *h = (int32_t)lroundf(vb[3]);
        return true;
    }
    return false;
}

/* The map lies on the tile's dark background (the drawing's own full-size
 * background rect has a percentage size, which LVGL's renderer skips), so
 * colours too dark to see there are brightened in place: same hue and
 * saturation, value raised to MOWER_MAP_MIN_VALUE. */
static void mower_svg_lift_colors(char *svg)
{
    static const char hex[] = "0123456789abcdef";
    for (char *p = strstr(svg, "\"#"); p != NULL; p = strstr(p + 2, "\"#")) {
        char *h = p + 2;
        uint8_t c[3];
        bool ok = true;
        for (int i = 0; i < 6 && ok; i++) {
            ok = isxdigit((unsigned char)h[i]);
        }
        /* #rrggbb or #rrggbbaa only */
        if (!ok || (h[6] != '"' && !(isxdigit((unsigned char)h[6]) && isxdigit((unsigned char)h[7]) && h[8] == '"'))) {
            continue;
        }
        for (int i = 0; i < 3; i++) {
            char pair[3] = {h[i * 2], h[i * 2 + 1], 0};
            c[i] = (uint8_t)strtoul(pair, NULL, 16);
        }
        const int v = LV_MAX(c[0], LV_MAX(c[1], c[2]));
        if (v >= MOWER_MAP_MIN_VALUE) {
            continue;
        }
        for (int i = 0; i < 3; i++) {
            const int n = v == 0 ? MOWER_MAP_MIN_VALUE : (c[i] * MOWER_MAP_MIN_VALUE) / v;
            h[i * 2] = hex[(n >> 4) & 0xF];
            h[i * 2 + 1] = hex[n & 0xF];
        }
    }
}

/* Draws the SVG at `scale` (1/256) with its origin at ox/oy of the canvas. */
static void mower_svg_draw(w_mower_ctx_t *ctx, lv_image_decoder_dsc_t *dec, const lv_image_dsc_t *src,
    int32_t scale, int32_t ox, int32_t oy)
{
    lv_draw_buf_clear(ctx->map_canvas_buf, NULL);
    lv_layer_t layer;
    lv_canvas_init_layer(ctx->map_canvas, &layer);
    lv_draw_image_dsc_t dsc;
    lv_draw_image_dsc_init(&dsc);
    dsc.base.obj = ctx->map_canvas;
    dsc.src = src;
    dsc.header = dec->header;
    dsc.scale_x = scale;
    dsc.scale_y = scale;
    const lv_area_t coords = {ox, oy, ox + ctx->map_w - 1, oy + ctx->map_h - 1};
    const lv_area_t clip = {0, 0, ctx->map_w - 1, ctx->map_h - 1};
    dec->decoder->custom_draw_cb(&layer, dec, &coords, &dsc, &clip);
    lv_canvas_finish_layer(ctx->map_canvas, &layer);
}

/* Bounding box of the largest connected shape on the canvas, found on a
 * grid of MOWER_MAP_CELL px cells so that nearby strokes join up. */
static bool mower_map_main_shape(const lv_draw_buf_t *buf, lv_area_t *out)
{
    const int32_t w = (int32_t)buf->header.w;
    const int32_t h = (int32_t)buf->header.h;
    const int32_t gw = (w + MOWER_MAP_CELL - 1) / MOWER_MAP_CELL;
    const int32_t gh = (h + MOWER_MAP_CELL - 1) / MOWER_MAP_CELL;
    const int32_t cells = gw * gh;
    /* per cell: number of drawn pixels, and whether a search reached it */
    uint16_t *count = heap_caps_calloc((size_t)cells, sizeof(uint16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    uint8_t *seen = heap_caps_malloc((size_t)cells, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    int32_t *stack = heap_caps_malloc((size_t)cells * sizeof(int32_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    bool found = false;
    if (count == NULL || seen == NULL || stack == NULL) {
        goto done;
    }
    for (int32_t y = 0; y < h; y++) {
        const uint8_t *row = buf->data + (size_t)y * buf->header.stride;
        for (int32_t x = 0; x < w; x++) {
            if (row[x * 4 + 3] > 48) {
                count[(y / MOWER_MAP_CELL) * gw + x / MOWER_MAP_CELL]++;
            }
        }
    }
    memset(seen, 0, (size_t)cells);
    uint32_t best_px = 0;
    for (int32_t seed = 0; seed < cells; seed++) {
        if (count[seed] == 0 || seen[seed]) {
            continue;
        }
        int32_t top = 0;
        uint32_t px = 0;
        lv_area_t a = {seed % gw, seed / gw, seed % gw, seed / gw};
        stack[top++] = seed;
        seen[seed] = 1;
        while (top > 0) {
            const int32_t c = stack[--top];
            const int32_t cx = c % gw;
            const int32_t cy = c / gw;
            px += count[c];
            a.x1 = LV_MIN(a.x1, cx);
            a.x2 = LV_MAX(a.x2, cx);
            a.y1 = LV_MIN(a.y1, cy);
            a.y2 = LV_MAX(a.y2, cy);
            for (int32_t dy = -1; dy <= 1; dy++) {
                for (int32_t dx = -1; dx <= 1; dx++) {
                    const int32_t nx = cx + dx;
                    const int32_t ny = cy + dy;
                    if (nx < 0 || ny < 0 || nx >= gw || ny >= gh) {
                        continue;
                    }
                    const int32_t n = ny * gw + nx;
                    if (count[n] > 0 && !seen[n]) {
                        seen[n] = 1;
                        stack[top++] = n;
                    }
                }
            }
        }
        if (px > best_px) {
            best_px = px;
            out->x1 = a.x1 * MOWER_MAP_CELL;
            out->y1 = a.y1 * MOWER_MAP_CELL;
            out->x2 = LV_MIN((a.x2 + 1) * MOWER_MAP_CELL, w) - 1;
            out->y2 = LV_MIN((a.y2 + 1) * MOWER_MAP_CELL, h) - 1;
            found = true;
        }
    }
done:
    heap_caps_free(count);
    heap_caps_free(seen);
    heap_caps_free(stack);
    return found;
}
#endif

/* A vector map (SVG) is rendered once per download into a canvas of the
 * box size; drawing the page then only blits that canvas. */
static bool mower_map_render_svg(w_mower_ctx_t *ctx, const ha_cover_result_t *svg)
{
#if LV_USE_SVG
    if (ctx->map_canvas_buf == NULL) {
        ctx->map_canvas_buf = lv_draw_buf_create((uint32_t)ctx->map_w, (uint32_t)ctx->map_h,
            LV_COLOR_FORMAT_ARGB8888, LV_STRIDE_AUTO);
        if (ctx->map_canvas_buf == NULL) {
            return false;
        }
    }
    if (ctx->map_canvas == NULL) {
        ctx->map_canvas = lv_canvas_create(ctx->map_box);
        lv_canvas_set_draw_buf(ctx->map_canvas, ctx->map_canvas_buf);
        lv_obj_set_pos(ctx->map_canvas, 0, 0);
    }
    /* lv_draw_image() clips a custom-drawn image against its widget's
     * screen coordinates, which does not work for an off-screen canvas
     * layer; so the decoder's draw callback is called directly, scaled to
     * fit the box (the SVG's own size comes from its width/height/viewBox). */
    /* the text is ours (NUL-terminated copy of the download) */
    mower_svg_lift_colors((char *)svg->image.data);
    lv_image_decoder_dsc_t dec;
    if (lv_image_decoder_open(&dec, &svg->image, NULL) != LV_RESULT_OK) {
        return false;
    }
    if (dec.decoder == NULL || dec.decoder->custom_draw_cb == NULL || dec.header.w == 0 || dec.header.h == 0) {
        lv_image_decoder_close(&dec);
        return false;
    }
    /* the decoder reports the size set by the fetcher (the box), not the
     * drawing's own one */
    int32_t src_w = 0;
    int32_t src_h = 0;
    if (!mower_svg_size((const char *)svg->image.data, &src_w, &src_h)) {
        src_w = (int32_t)dec.header.w;
        src_h = (int32_t)dec.header.h;
    }
    /* fit: scale = min(box/src) in 1/256 */
    int32_t scale = (int32_t)(((int64_t)ctx->map_w * 256) / src_w);
    const int32_t scale_h = (int32_t)(((int64_t)ctx->map_h * 256) / src_h);
    if (scale_h < scale) {
        scale = scale_h;
    }
    int32_t ox = (ctx->map_w - (src_w * scale) / 256) / 2;
    int32_t oy = (ctx->map_h - (src_h * scale) / 256) / 2;
    mower_svg_draw(ctx, &dec, &svg->image, scale, ox, oy);

    /* The lawn usually covers a small part of the drawing (legend, captions
     * and margins around it): zoom in on the largest connected shape. */
    lv_area_t box;
    if (mower_map_main_shape(ctx->map_canvas_buf, &box)) {
        const int32_t bw = lv_area_get_width(&box);
        const int32_t bh = lv_area_get_height(&box);
        const int32_t avail_w = ctx->map_w - 2 * MOWER_MAP_MARGIN;
        const int32_t avail_h = ctx->map_h - 2 * MOWER_MAP_MARGIN;
        int32_t zoom = (avail_w * 256) / bw;
        if ((avail_h * 256) / bh < zoom) {
            zoom = (avail_h * 256) / bh;
        }
        zoom = LV_MIN(zoom, MOWER_MAP_MAX_ZOOM * 256);
        if (zoom > 256 + 256 / 16) {
            /* drawing coordinates of the shape's centre, times 256 */
            const int64_t cx = ((int64_t)(box.x1 + box.x2 + 1) * 128 - (int64_t)ox * 256) * 256 / scale;
            const int64_t cy = ((int64_t)(box.y1 + box.y2 + 1) * 128 - (int64_t)oy * 256) * 256 / scale;
            scale = (int32_t)(((int64_t)scale * zoom) / 256);
            ox = ctx->map_w / 2 - (int32_t)((cx * scale) / (256 * 256));
            oy = ctx->map_h / 2 - (int32_t)((cy * scale) / (256 * 256));
            mower_svg_draw(ctx, &dec, &svg->image, scale, ox, oy);
        }
    }
    lv_image_decoder_close(&dec);

    /* the next download may reuse this buffer address */
    lv_image_cache_drop(&svg->image);
    lv_obj_set_hidden(ctx->map_canvas, false);
    lv_obj_set_hidden(ctx->map_img, true);
    lv_obj_set_hidden(ctx->map_icon, true);
    return true;
#else
    LV_UNUSED(ctx);
    LV_UNUSED(svg);
    return false;
#endif
}

static void mower_map_cb(void *user, const ha_cover_result_t *result)
{
    w_mower_ctx_t *ctx = user;
    ctx->map_fetching = false;
    if (result == NULL) {
        ctx->map_next_ms = mower_now_ms() + MOWER_MAP_RETRY_MS;
        return;
    }
    ha_cover_result_t copy = *result;
    if (copy.valid && copy.is_svg && ctx->map_box != NULL) {
        if (!mower_map_render_svg(ctx, &copy)) {
            ctx->map_next_ms = mower_now_ms() + MOWER_MAP_RETRY_MS;
        }
        ha_cover_result_release(&copy); /* the text is drawn now */
        return;
    }
    if (!copy.valid || copy.image.data == NULL || ctx->map_img == NULL) {
        ha_cover_result_release(&copy);
        ctx->map_next_ms = mower_now_ms() + MOWER_MAP_RETRY_MS;
        return;
    }
    void *old = ctx->map_buf;
    ctx->map_dsc = copy.image;
    ctx->map_buf = (void *)copy.image.data;
    lv_image_set_src(ctx->map_img, &ctx->map_dsc);
    /* A map that fits is drawn 1:1; only a larger one is scaled down. */
    const bool fits = (int32_t)ctx->map_dsc.header.w <= ctx->map_w && (int32_t)ctx->map_dsc.header.h <= ctx->map_h;
    lv_image_set_inner_align(ctx->map_img, fits ? LV_IMAGE_ALIGN_CENTER : LV_IMAGE_ALIGN_CONTAIN);
    lv_obj_set_hidden(ctx->map_img, false);
    if (ctx->map_icon != NULL) {
        lv_obj_set_hidden(ctx->map_icon, true);
    }
    if (ctx->map_canvas != NULL) {
        lv_obj_set_hidden(ctx->map_canvas, true);
    }
    if (old != NULL) {
        heap_caps_free(old);
    }
}

static void mower_map_fetch(w_mower_ctx_t *ctx)
{
    if (ctx->map_entity[0] == '\0' || ctx->map_fetching || !ctx->visible || !ha_client_is_connected()) {
        return;
    }
    char url[APP_MAX_ENTITY_ID_LEN + 32];
    snprintf(url, sizeof(url), "/api/camera_proxy/%s", ctx->map_entity);
    if (ha_cover_fetcher_request(url, (int)ctx->map_w, (int)ctx->map_h, mower_map_cb, ctx) == ESP_OK) {
        ctx->map_fetching = true;
    } else {
        ctx->map_next_ms = mower_now_ms() + MOWER_MAP_RETRY_MS;
    }
}

/* ------------------------------------------------------------------ view */

static void mower_set_text(lv_obj_t *label, char *shown, size_t shown_len, const char *text)
{
    if (label == NULL || strcmp(shown, text) == 0) {
        return;
    }
    snprintf(shown, shown_len, "%s", text);
    lv_label_set_text(label, text);
}

static void mower_refresh(w_mower_ctx_t *ctx)
{
    static ha_state_t st; /* LVGL task only */
    const bool have = ha_model_get_state(ctx->entity_id, &st);
    ctx->unavailable = !have || w_state_is_unavailable(st.state);
    ctx->state = ctx->unavailable ? MOWER_ST_UNKNOWN : mower_parse_state(st.state);

    /* status line: state · task */
    char task[64];
    mower_sensor_text(ctx->task_entity, task, sizeof(task));
    /* Placeholders of the task sensor say nothing next to the state. */
    static const char *const no_task[] = {"idle", "none", "standby", "-", "no task", "keine"};
    for (size_t i = 0; i < sizeof(no_task) / sizeof(no_task[0]); i++) {
        if (strcasecmp(task, no_task[i]) == 0) {
            task[0] = '\0';
        }
    }
    /* A task that is a mower state ("paused" = paused job) is translated
     * like one, and left out when it only repeats the state. */
    const mower_state_t task_st = task[0] != '\0' ? mower_parse_state(task) : MOWER_ST_UNKNOWN;
    if (task_st == ctx->state) {
        task[0] = '\0';
    } else if (task_st != MOWER_ST_UNKNOWN) {
        snprintf(task, sizeof(task), "%s", mower_state_text(task_st, task));
    }
    char status[128];
    if (ctx->unavailable) {
        snprintf(status, sizeof(status), "%s", ui_i18n_get("common.unavailable", "unavailable"));
    } else if (task[0] != '\0') {
        snprintf(status, sizeof(status), "%s · %s", mower_state_text(ctx->state, st.state), task);
    } else {
        snprintf(status, sizeof(status), "%s", mower_state_text(ctx->state, st.state));
    }
    if (strcmp(ctx->shown_status, status) != 0) {
        mower_set_text(ctx->status_label, ctx->shown_status, sizeof(ctx->shown_status), status);
        const uint32_t color = ctx->state == MOWER_ST_ERROR ? MOWER_COLOR_ERROR
                               : ctx->state == MOWER_ST_MOWING ? MOWER_COLOR_ACTIVE
                                                               : 0;
        lv_obj_set_style_text_color(ctx->status_label,
            color != 0 ? lv_color_hex(color) : theme_default_color_text_primary(), LV_PART_MAIN);
    }

    /* battery */
    float battery = mower_sensor_value(ctx->battery_entity);
    if (isnan(battery) && have) {
        battery = mower_attr_battery(&st);
    }
    char batt_text[16] = "";
    if (!isnan(battery)) {
        snprintf(batt_text, sizeof(batt_text), "%d %%", (int)lroundf(battery));
    }
    mower_set_text(ctx->battery_label, ctx->shown_battery, sizeof(ctx->shown_battery), batt_text);
    const int batt_icon = isnan(battery) ? -1 : (ctx->state == MOWER_ST_DOCKED && battery < 99.5f) ? 1 : 0;
    if (batt_icon != ctx->shown_battery_icon && ctx->battery_icon != NULL) {
        ctx->shown_battery_icon = batt_icon;
        if (batt_icon < 0) {
            lv_obj_set_hidden(ctx->battery_icon, true);
        } else {
            lv_obj_set_hidden(ctx->battery_icon, false);
            (void)ui_mdi_icon_apply(ctx->battery_icon, batt_icon ? "mdi:battery-charging" : "mdi:battery",
                APP_LAYOUT_COMPACT ? 18 : 22);
        }
    }

    /* progress: while a job runs */
    const float progress = mower_sensor_value(ctx->progress_entity);
    const bool job = ctx->state == MOWER_ST_MOWING || ctx->state == MOWER_ST_PAUSED ||
                     ctx->state == MOWER_ST_RETURNING;
    const int prog = (job && !isnan(progress)) ? (int)lroundf(fminf(fmaxf(progress, 0.0f), 100.0f)) : -1;
    if (prog != ctx->shown_progress) {
        ctx->shown_progress = prog;
        if (prog < 0) {
            lv_obj_set_hidden(ctx->bar, true);
            lv_obj_set_hidden(ctx->bar_label, true);
        } else {
            lv_obj_set_hidden(ctx->bar, false);
            lv_obj_set_hidden(ctx->bar_label, false);
            lv_bar_set_value(ctx->bar, prog, LV_ANIM_OFF);
            lv_label_set_text_fmt(ctx->bar_label, "%d %%", prog);
        }
    }

    /* main button: pause while mowing, resume when paused, start otherwise */
    const int main_mode = ctx->state == MOWER_ST_MOWING ? 1 : ctx->state == MOWER_ST_PAUSED ? 2 : 0;
    if (main_mode != ctx->shown_main) {
        ctx->shown_main = main_mode;
        (void)ui_mdi_icon_apply(ctx->btn_main_icon, main_mode == 1 ? "mdi:pause" : "mdi:play",
            APP_LAYOUT_COMPACT ? 22 : 26);
        lv_label_set_text(ctx->btn_main_text, main_mode == 1   ? ui_i18n_get("mower.pause", "Pause")
                                              : main_mode == 2 ? ui_i18n_get("mower.resume", "Resume")
                                                               : ui_i18n_get("mower.start", "Start"));
    }
    const bool can_act = !ctx->unavailable;
    if (can_act) {
        lv_obj_clear_state(ctx->btn_main, LV_STATE_DISABLED);
    } else {
        lv_obj_add_state(ctx->btn_main, LV_STATE_DISABLED);
    }
    if (can_act && ctx->state != MOWER_ST_DOCKED && ctx->state != MOWER_ST_RETURNING) {
        lv_obj_clear_state(ctx->btn_dock, LV_STATE_DISABLED);
    } else {
        lv_obj_add_state(ctx->btn_dock, LV_STATE_DISABLED);
    }
}

static void mower_call(w_mower_ctx_t *ctx, const char *service)
{
    char data[APP_MAX_ENTITY_ID_LEN + 24];
    snprintf(data, sizeof(data), "{\"entity_id\":\"%s\"}", ctx->entity_id);
    const esp_err_t err = ha_client_call_service("lawn_mower", service, data);
    ESP_LOGI(TAG, "%s %s -> %s", service, ctx->entity_id, esp_err_to_name(err));
    /* A fresh map soon after a command. */
    ctx->map_next_ms = mower_now_ms() + 3000;
}

static void mower_main_cb(lv_event_t *e)
{
    w_mower_ctx_t *ctx = lv_event_get_user_data(e);
    if (ctx == NULL || ctx->unavailable) {
        return;
    }
    mower_call(ctx, ctx->state == MOWER_ST_MOWING ? "pause" : "start_mowing");
}

static void mower_dock_cb(lv_event_t *e)
{
    w_mower_ctx_t *ctx = lv_event_get_user_data(e);
    if (ctx == NULL || ctx->unavailable) {
        return;
    }
    mower_call(ctx, "dock");
}

static void mower_timer_cb(lv_timer_t *t)
{
    w_mower_ctx_t *ctx = lv_timer_get_user_data(t);
    if (ctx == NULL || !ctx->visible || ctx->map_entity[0] == '\0') {
        return;
    }
    const int64_t now = mower_now_ms();
    if (now >= ctx->map_next_ms && !ctx->map_fetching) {
        ctx->map_next_ms = now + (ctx->state == MOWER_ST_MOWING || ctx->state == MOWER_ST_RETURNING
                                         ? MOWER_MAP_ACTIVE_MS
                                         : MOWER_MAP_IDLE_MS);
        mower_map_fetch(ctx);
    }
}

static void mower_delete_cb(lv_event_t *e)
{
    w_mower_ctx_t *ctx = lv_event_get_user_data(e);
    if (ctx == NULL) {
        return;
    }
    ha_cover_fetcher_cancel(ctx);
    if (ctx->timer != NULL) {
        lv_timer_delete(ctx->timer);
    }
    if (ctx->map_buf != NULL) {
        heap_caps_free(ctx->map_buf);
    }
    /* The canvas object goes with the card; its buffer is ours. */
    if (ctx->map_canvas_buf != NULL) {
        lv_draw_buf_destroy(ctx->map_canvas_buf);
    }
    free(ctx);
}

static lv_obj_t *mower_label(lv_obj_t *parent, const lv_font_t *font, lv_color_t color)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, LV_PART_MAIN);
    lv_obj_set_style_text_color(l, color, LV_PART_MAIN);
    lv_label_set_text(l, "");
    return l;
}

static lv_obj_t *mower_button(lv_obj_t *parent, int32_t w, int32_t h, lv_obj_t **icon, lv_obj_t **text,
    const char *mdi, const char *caption, lv_event_cb_t cb, w_mower_ctx_t *ctx)
{
    lv_obj_t *btn = lv_obj_create(parent);
    /* The theme helper starts with lv_obj_remove_style_all(): size after it. */
    theme_default_style_button(btn, false);
    lv_obj_set_size(btn, w, h);
    lv_obj_set_scrollable(btn, false);
    lv_obj_set_style_radius(btn, h / 2, LV_PART_MAIN);
    lv_obj_set_style_pad_all(btn, 0, LV_PART_MAIN);
    lv_obj_set_style_opa(btn, LV_OPA_40, LV_PART_MAIN | LV_STATE_DISABLED);
    lv_obj_set_style_bg_opa(btn, LV_OPA_80, LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_set_flex_flow(btn, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(btn, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(btn, 6, LV_PART_MAIN);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, ctx);

    lv_obj_t *ic = mower_label(btn, APP_FONT_TEXT_18, theme_default_color_text_primary());
    (void)ui_mdi_icon_apply(ic, mdi, APP_LAYOUT_COMPACT ? 22 : 26);
    lv_obj_t *tx = mower_label(btn, APP_LAYOUT_COMPACT ? APP_FONT_TEXT_16 : APP_FONT_TEXT_18,
        theme_default_color_text_primary());
    lv_label_set_text(tx, caption);
    if (icon != NULL) {
        *icon = ic;
    }
    if (text != NULL) {
        *text = tx;
    }
    return btn;
}

/* ------------------------------------------------------------------ API */

static void mower_json_entity(const cJSON *json, const char *key, char *out, size_t len)
{
    out[0] = '\0';
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(json, key);
    if (cJSON_IsString(v) && v->valuestring != NULL) {
        snprintf(out, len, "%s", v->valuestring);
    }
}

esp_err_t w_mower_create(const ui_widget_def_t *def, lv_obj_t *parent, ui_widget_instance_t *out_instance)
{
    if (def == NULL || parent == NULL || out_instance == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    w_mower_ctx_t *ctx = ui_calloc_prefer_psram(1, sizeof(w_mower_ctx_t));
    if (ctx == NULL) {
        return ESP_ERR_NO_MEM;
    }
    snprintf(ctx->entity_id, sizeof(ctx->entity_id), "%s", def->entity_id);
    snprintf(ctx->map_entity, sizeof(ctx->map_entity), "%s", def->secondary_entity_id);
    mower_json_entity(def->json, "mower_battery_entity_id", ctx->battery_entity, sizeof(ctx->battery_entity));
    mower_json_entity(def->json, "mower_progress_entity_id", ctx->progress_entity, sizeof(ctx->progress_entity));
    mower_json_entity(def->json, "mower_task_entity_id", ctx->task_entity, sizeof(ctx->task_entity));
    ctx->shown_battery_icon = -2;
    ctx->shown_progress = -2;
    ctx->shown_main = -1;
    ctx->visible = true;

    const bool compact = APP_LAYOUT_COMPACT;
    const int32_t pad = compact ? 10 : 14;
    const int32_t gap = compact ? 6 : 8;
    const int32_t btn_h = compact ? 40 : 48;
    const int32_t inner_w = def->w - 2 * pad;

    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_set_pos(card, def->x, def->y);
    lv_obj_set_size(card, def->w, def->h);
    lv_obj_set_scrollable(card, false);
    theme_default_style_card(card);
    lv_obj_set_style_pad_all(card, pad, LV_PART_MAIN);
    ctx->card = card;

    /* top row: title, battery */
    const lv_font_t *f_title = compact ? APP_FONT_TEXT_16 : APP_FONT_TEXT_18;
    lv_obj_t *title = mower_label(card, f_title, theme_default_color_text_muted());
    lv_label_set_text(title, def->title[0] ? def->title : def->id);
    lv_label_set_long_mode(title, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_width(title, inner_w - (compact ? 80 : 100));
    lv_obj_set_pos(title, 0, 0);
    ctx->battery_label = mower_label(card, f_title, theme_default_color_text_primary());
    lv_obj_align(ctx->battery_label, LV_ALIGN_TOP_RIGHT, 0, 0);
    ctx->battery_icon = mower_label(card, f_title, theme_default_color_text_primary());
    lv_obj_align_to(ctx->battery_icon, ctx->battery_label, LV_ALIGN_OUT_LEFT_MID, -2, 0);
    lv_obj_set_hidden(ctx->battery_icon, true);
    lv_obj_update_layout(title);
    const int32_t top_h = LV_MAX(lv_obj_get_height(title), compact ? 22 : 26);

    /* bottom up: buttons, progress, status */
    const int32_t btn_y = def->h - 2 * pad - btn_h;
    const int32_t btn_w = (inner_w - gap) / 2;
    ctx->btn_main = mower_button(card, btn_w, btn_h, &ctx->btn_main_icon, &ctx->btn_main_text, "mdi:play",
        ui_i18n_get("mower.start", "Start"), mower_main_cb, ctx);
    lv_obj_set_pos(ctx->btn_main, 0, btn_y);
    ctx->btn_dock = mower_button(card, btn_w, btn_h, NULL, NULL, "mdi:home-import-outline",
        ui_i18n_get("mower.dock", "Dock"), mower_dock_cb, ctx);
    lv_obj_set_pos(ctx->btn_dock, btn_w + gap, btn_y);

    const int32_t bar_h = compact ? 6 : 8;
    const int32_t bar_row = compact ? 18 : 22;
    const int32_t bar_y = btn_y - gap - bar_row;
    ctx->bar_label = mower_label(card, compact ? APP_FONT_TEXT_14 : APP_FONT_TEXT_16, theme_default_color_text_muted());
    lv_obj_set_width(ctx->bar_label, compact ? 44 : 56);
    lv_obj_set_style_text_align(ctx->bar_label, LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN);
    lv_obj_set_pos(ctx->bar_label, inner_w - (compact ? 44 : 56), bar_y);
    ctx->bar = lv_bar_create(card);
    lv_obj_set_size(ctx->bar, inner_w - (compact ? 52 : 66), bar_h);
    lv_obj_set_pos(ctx->bar, 0, bar_y + (bar_row - bar_h) / 2);
    lv_bar_set_range(ctx->bar, 0, 100);
    lv_obj_set_style_bg_color(ctx->bar, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(ctx->bar, LV_OPA_20, LV_PART_MAIN);
    lv_obj_set_style_radius(ctx->bar, bar_h / 2, LV_PART_MAIN);
    lv_obj_set_style_bg_color(ctx->bar, lv_color_hex(MOWER_COLOR_BAR), LV_PART_INDICATOR);
    lv_obj_set_style_radius(ctx->bar, bar_h / 2, LV_PART_INDICATOR);
    lv_obj_set_hidden(ctx->bar, true);
    lv_obj_set_hidden(ctx->bar_label, true);

    const lv_font_t *f_status = compact ? APP_FONT_TEXT_18 : APP_FONT_TEXT_22;
    ctx->status_label = mower_label(card, f_status, theme_default_color_text_primary());
    lv_label_set_long_mode(ctx->status_label, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_width(ctx->status_label, inner_w);
    lv_label_set_text(ctx->status_label, "--");
    lv_obj_update_layout(ctx->status_label);
    const int32_t status_h = lv_obj_get_height(ctx->status_label);
    const int32_t status_y = bar_y - gap / 2 - status_h;
    lv_obj_set_pos(ctx->status_label, 0, status_y);

    /* map (or icon) fills what is left */
    const int32_t map_y = top_h + gap;
    ctx->map_w = inner_w;
    ctx->map_h = LV_MAX(status_y - gap - map_y, 24);
    ctx->map_box = lv_obj_create(card);
    lv_obj_remove_style_all(ctx->map_box);
    lv_obj_set_pos(ctx->map_box, 0, map_y);
    lv_obj_set_size(ctx->map_box, ctx->map_w, ctx->map_h);
    lv_obj_set_style_bg_color(ctx->map_box, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(ctx->map_box, LV_OPA_20, LV_PART_MAIN);
    lv_obj_set_style_radius(ctx->map_box, compact ? 8 : 10, LV_PART_MAIN);
    lv_obj_set_scrollable(ctx->map_box, false);
    lv_obj_set_clickable(ctx->map_box, false);
    ctx->map_img = lv_image_create(ctx->map_box);
    lv_obj_set_size(ctx->map_img, ctx->map_w, ctx->map_h);
    lv_obj_set_hidden(ctx->map_img, true);
    ctx->icon_px = LV_MIN(ctx->map_h - 8, compact ? 56 : 72);
    ctx->map_icon = mower_label(ctx->map_box, f_title, lv_color_hex(MOWER_COLOR_ACTIVE));
    if (!ui_mdi_icon_apply(ctx->map_icon, "mdi:robot-mower", ctx->icon_px)) {
        lv_label_set_text(ctx->map_icon, "");
    }
    lv_obj_center(ctx->map_icon);

    lv_obj_add_event_cb(card, mower_delete_cb, LV_EVENT_DELETE, ctx);
    if (ctx->map_entity[0] != '\0') {
        ctx->timer = lv_timer_create(mower_timer_cb, 1000, ctx);
        ctx->map_next_ms = 0; /* first fetch on the next tick */
    }

    /* The runtime routes state changes of these sensors here as well. */
    const char *extras[] = {ctx->battery_entity, ctx->progress_entity, ctx->task_entity};
    for (size_t i = 0; i < sizeof(extras) / sizeof(extras[0]) && i < UI_EXTRA_ENTITIES_MAX; i++) {
        snprintf(out_instance->extra_entities[i], sizeof(out_instance->extra_entities[i]), "%s", extras[i]);
    }
    mower_refresh(ctx);
    ESP_LOGI(TAG, "%s: %s map=%s battery=%s progress=%s task=%s", def->id, ctx->entity_id,
        ctx->map_entity[0] ? ctx->map_entity : "-", ctx->battery_entity[0] ? ctx->battery_entity : "-",
        ctx->progress_entity[0] ? ctx->progress_entity : "-", ctx->task_entity[0] ? ctx->task_entity : "-");

    out_instance->obj = card;
    out_instance->ctx = ctx;
    return ESP_OK;
}

void w_mower_apply_state(ui_widget_instance_t *instance, const ha_state_t *state)
{
    if (instance == NULL || instance->ctx == NULL) {
        return;
    }
    w_mower_ctx_t *ctx = instance->ctx;
    const mower_state_t before = ctx->state;
    mower_refresh(ctx);
    /* Started or stopped mowing: refresh the map at the new rate. */
    if (state != NULL && strcmp(state->entity_id, ctx->entity_id) == 0 && before != ctx->state) {
        ctx->map_next_ms = mower_now_ms() + 2000;
    }
}

void w_mower_mark_unavailable(ui_widget_instance_t *instance)
{
    if (instance == NULL || instance->ctx == NULL) {
        return;
    }
    mower_refresh(instance->ctx);
}

void w_mower_set_visible(ui_widget_instance_t *instance, bool visible)
{
    if (instance == NULL || instance->ctx == NULL) {
        return;
    }
    w_mower_ctx_t *ctx = instance->ctx;
    ctx->visible = visible;
    if (!visible) {
        if (ctx->map_fetching) {
            ha_cover_fetcher_cancel(ctx);
            ctx->map_fetching = false;
        }
        return;
    }
    /* Shown again: a map older than the idle period is fetched at once. */
    if (ctx->map_buf == NULL && ctx->map_canvas == NULL) {
        ctx->map_next_ms = 0;
    }
}
