/* SPDX-License-Identifier: LicenseRef-FNCL-1.1 */
/* Chart tile: up to four sensors as bars, columns, a ring (pie) or lines.
 *
 * Layout fields:
 *   "chart_type":   "bar" | "column" | "pie" | "line"
 *   "chart_hours":  history window of the line chart (1..168, default 24)
 *   "chart_series": [{"entity_id":"sensor.x","color":"#RRGGBB","name":"..."}, ...] (1..4)
 *
 * Bars, columns and the ring show the current values and follow every state
 * change. The line chart loads the Home Assistant history (long-term
 * statistics first, raw history as fallback) and reloads it every few minutes
 * while it is on screen. */
#include "ui/ui_widget_factory.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "app_config.h"
#include "ha/ha_client.h"
#include "ha/ha_model.h"
#include "ui/fonts/app_text_fonts.h"
#include "ui/theme/theme_default.h"
#include "ui/ui_i18n.h"
#include "ui/ui_memory.h"

static const char *TAG = "w_chart";

#define CHART_SERIES_MAX UI_CHART_SERIES_MAX
#define CHART_POINTS 60
#define CHART_REFRESH_US (5LL * 60 * 1000 * 1000)
#define CHART_RETRY_US (30LL * 1000 * 1000)
#define CHART_Y_SCALE 1000
#define CHART_STAGES 16

static const uint32_t k_default_colors[CHART_SERIES_MAX] = {0x03A9F4, 0xFF9800, 0x4CAF50, 0xE91E63};

typedef enum { CHART_BAR, CHART_COLUMN, CHART_PIE, CHART_LINE } chart_kind_t;

/* History responses arrive on the HA receive task and may outlive the tile
 * (page rebuilt meanwhile), so they land in a shared staging table keyed by a
 * token instead of in the tile itself. */
typedef struct {
    uint32_t token; /* 0 = free */
    bool ready;
    bool ok;
    bool statistics;
    int count;
    float values[CHART_POINTS];
} chart_stage_t;

static chart_stage_t *s_stages;
static SemaphoreHandle_t s_stage_lock;
static uint32_t s_token_seq;

typedef struct {
    lv_obj_t *card;
    lv_obj_t *title;
    lv_obj_t *body;
    chart_kind_t kind;
    int hours;
    int n;
    char entity[CHART_SERIES_MAX][APP_MAX_ENTITY_ID_LEN];
    char name[CHART_SERIES_MAX][APP_MAX_NAME_LEN];
    bool name_set[CHART_SERIES_MAX];
    lv_color_t color[CHART_SERIES_MAX];
    float value[CHART_SERIES_MAX];
    bool has_value[CHART_SERIES_MAX];
    char unit[CHART_SERIES_MAX][APP_MAX_UNIT_LEN];
    /* bar / column */
    lv_obj_t *track[CHART_SERIES_MAX];
    lv_obj_t *fill[CHART_SERIES_MAX];
    lv_obj_t *value_label[CHART_SERIES_MAX];
    lv_obj_t *name_label[CHART_SERIES_MAX];
    /* pie */
    lv_obj_t *arc[CHART_SERIES_MAX];
    lv_obj_t *center_label;
    /* line */
    lv_obj_t *chart;
    lv_chart_series_t *ser[CHART_SERIES_MAX];
    lv_obj_t *min_label;
    lv_obj_t *max_label;
    float hist[CHART_SERIES_MAX][CHART_POINTS];
    int hist_count[CHART_SERIES_MAX];
    int fetch_series;     /* series being fetched, -1 = idle */
    int stage;            /* staging slot of the running fetch */
    bool fetch_statistics;
    int64_t next_fetch_us;
    lv_timer_t *timer;
    int32_t body_w;
    int32_t body_h;
} w_chart_ctx_t;

/* ---- helpers ------------------------------------------------------------- */

static bool chart_parse_hex(const char *text, lv_color_t *out)
{
    if (text == NULL) {
        return false;
    }
    if (*text == '#') {
        text++;
    }
    if (strlen(text) != 6) {
        return false;
    }
    char *end = NULL;
    const unsigned long v = strtoul(text, &end, 16);
    if (end == NULL || *end != '\0') {
        return false;
    }
    *out = lv_color_hex((uint32_t)v);
    return true;
}

static void chart_format(char *out, size_t len, float v, const char *unit)
{
    const float a = fabsf(v);
    const char *fmt = a >= 100.0f ? "%.0f" : (a >= 10.0f ? "%.1f" : "%.2f");
    char num[24];
    snprintf(num, sizeof(num), fmt, (double)v);
    /* "12.50" -> "12.5", "3.00" -> "3" */
    if (strchr(num, '.') != NULL) {
        size_t n = strlen(num);
        while (n > 0 && num[n - 1] == '0') {
            num[--n] = '\0';
        }
        if (n > 0 && num[n - 1] == '.') {
            num[--n] = '\0';
        }
    }
    if (unit != NULL && unit[0] != '\0') {
        snprintf(out, len, "%s %s", num, unit);
    } else {
        snprintf(out, len, "%s", num);
    }
}

static const char *chart_series_name(const w_chart_ctx_t *ctx, int i)
{
    return ctx->name[i][0] != '\0' ? ctx->name[i] : ctx->entity[i];
}

static lv_obj_t *chart_label(lv_obj_t *parent, const lv_font_t *font, lv_color_t color)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, LV_PART_MAIN);
    lv_obj_set_style_text_color(l, color, LV_PART_MAIN);
    lv_label_set_text(l, "");
    return l;
}

static lv_obj_t *chart_box(lv_obj_t *parent)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    return o;
}

/* ---- current values (bar / column / pie) ---------------------------------- */

static void chart_read_values(w_chart_ctx_t *ctx)
{
    ha_state_t *st = ui_calloc_prefer_psram(1, sizeof(*st));
    ha_entity_info_t *info = ui_calloc_prefer_psram(1, sizeof(*info));
    if (st == NULL || info == NULL) {
        free(st);
        free(info);
        return;
    }
    for (int i = 0; i < ctx->n; i++) {
        ctx->has_value[i] = false;
        if (ha_model_get_state(ctx->entity[i], st)) {
            char *end = NULL;
            const float v = strtof(st->state, &end);
            if (end != st->state && isfinite(v)) {
                ctx->value[i] = v;
                ctx->has_value[i] = true;
            }
        }
        if (ha_model_get_entity(ctx->entity[i], info)) {
            snprintf(ctx->unit[i], sizeof(ctx->unit[i]), "%s", info->unit);
            if (!ctx->name_set[i] && info->name[0] != '\0') {
                snprintf(ctx->name[i], sizeof(ctx->name[i]), "%s", info->name);
            }
        }
    }
    free(st);
    free(info);
}

static float chart_max_abs(const w_chart_ctx_t *ctx)
{
    float m = 0.0f;
    for (int i = 0; i < ctx->n; i++) {
        if (ctx->has_value[i] && fabsf(ctx->value[i]) > m) {
            m = fabsf(ctx->value[i]);
        }
    }
    return m;
}

static void chart_update_bars(w_chart_ctx_t *ctx)
{
    const float max = chart_max_abs(ctx);
    for (int i = 0; i < ctx->n; i++) {
        char text[40];
        if (ctx->has_value[i]) {
            chart_format(text, sizeof(text), ctx->value[i], ctx->unit[i]);
        } else {
            snprintf(text, sizeof(text), "--");
        }
        lv_label_set_text(ctx->value_label[i], text);
        lv_label_set_text(ctx->name_label[i], chart_series_name(ctx, i));
        const float frac = (ctx->has_value[i] && max > 0.0f) ? fabsf(ctx->value[i]) / max : 0.0f;
        if (ctx->kind == CHART_BAR) {
            lv_obj_set_width(ctx->fill[i], LV_PCT((int32_t)(frac * 100.0f + 0.5f)));
        } else {
            lv_obj_set_height(ctx->fill[i], LV_PCT((int32_t)(frac * 100.0f + 0.5f)));
        }
    }
}

static void chart_update_pie(w_chart_ctx_t *ctx)
{
    float total = 0.0f;
    for (int i = 0; i < ctx->n; i++) {
        if (ctx->has_value[i] && ctx->value[i] > 0.0f) {
            total += ctx->value[i];
        }
    }
    float start = 0.0f;
    for (int i = 0; i < ctx->n; i++) {
        const float share = (total > 0.0f && ctx->has_value[i] && ctx->value[i] > 0.0f) ? ctx->value[i] / total : 0.0f;
        const float end = start + share * 360.0f;
        if (share <= 0.0f) {
            lv_obj_add_flag(ctx->arc[i], LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_clear_flag(ctx->arc[i], LV_OBJ_FLAG_HIDDEN);
            lv_arc_set_bg_angles(ctx->arc[i], (lv_value_precise_t)start, (lv_value_precise_t)end);
        }
        start = end;

        char text[48];
        if (ctx->has_value[i]) {
            char v[32];
            chart_format(v, sizeof(v), ctx->value[i], ctx->unit[i]);
            snprintf(text, sizeof(text), "%s: %s", chart_series_name(ctx, i), v);
        } else {
            snprintf(text, sizeof(text), "%s: --", chart_series_name(ctx, i));
        }
        lv_label_set_text(ctx->value_label[i], text);
    }
    /* Sum in the middle when all series share a unit that adds up (W, kWh,
     * ...); a sum of percentages or temperatures means nothing. */
    bool same_unit = ctx->n > 0;
    for (int i = 1; i < ctx->n; i++) {
        same_unit = same_unit && strcmp(ctx->unit[i], ctx->unit[0]) == 0;
    }
    if (same_unit) {
        static const char *const not_additive[] = {"%", "Â°" "C", "Â°" "F", "K", "lx", "ppm", "hPa", "dB"};
        for (size_t u = 0; u < sizeof(not_additive) / sizeof(not_additive[0]); u++) {
            if (strcmp(ctx->unit[0], not_additive[u]) == 0) {
                same_unit = false;
            }
        }
    }
    char text[40] = "";
    if (total > 0.0f && same_unit) {
        chart_format(text, sizeof(text), total, ctx->unit[0]);
    }
    lv_label_set_text(ctx->center_label, text);
}

static void chart_update_line_legend(w_chart_ctx_t *ctx)
{
    for (int i = 0; i < ctx->n; i++) {
        char text[48];
        if (ctx->has_value[i]) {
            char v[32];
            chart_format(v, sizeof(v), ctx->value[i], ctx->unit[i]);
            snprintf(text, sizeof(text), "%s %s", chart_series_name(ctx, i), v);
        } else {
            snprintf(text, sizeof(text), "%s", chart_series_name(ctx, i));
        }
        lv_label_set_text(ctx->value_label[i], text);
    }
}

/* ---- line chart: history ------------------------------------------------- */

static double chart_number(const cJSON *item)
{
    if (cJSON_IsNumber(item)) {
        return item->valuedouble;
    }
    if (cJSON_IsString(item) && item->valuestring != NULL) {
        char *end = NULL;
        const double v = strtod(item->valuestring, &end);
        if (end != item->valuestring) {
            return v;
        }
    }
    return NAN;
}

static chart_stage_t *chart_stage_find(uint32_t token)
{
    for (int i = 0; i < CHART_STAGES; i++) {
        if (s_stages[i].token == token) {
            return &s_stages[i];
        }
    }
    return NULL;
}

/* HA receive task: put the samples into CHART_POINTS time buckets. */
static void chart_history_cb(bool success, cJSON *result, void *user)
{
    const uint32_t token = (uint32_t)(uintptr_t)user;
    float sum[CHART_POINTS];
    int cnt[CHART_POINTS];
    memset(sum, 0, sizeof(sum));
    memset(cnt, 0, sizeof(cnt));
    bool statistics = false;
    int samples = 0;

    const int hours = (int)(token & 0xFFFF);
    const time_t now = time(NULL);
    const double t0 = (double)now - (double)hours * 3600.0;
    const double span = (double)hours * 3600.0;
    double last_before = NAN;

    const cJSON *list = (success && cJSON_IsObject(result)) ? result->child : NULL;
    const cJSON *item = NULL;
    if (cJSON_IsArray(list)) {
        cJSON_ArrayForEach(item, list)
        {
            double v;
            double t;
            const cJSON *mean = cJSON_GetObjectItemCaseSensitive(item, "mean");
            if (mean != NULL) {
                statistics = true;
                v = chart_number(mean);
                t = chart_number(cJSON_GetObjectItemCaseSensitive(item, "start"));
                if (!isnan(t) && t > 1e11) {
                    t /= 1000.0; /* ms */
                }
            } else {
                const cJSON *state = cJSON_GetObjectItemCaseSensitive(item, "s");
                v = chart_number(state != NULL ? state : cJSON_GetObjectItemCaseSensitive(item, "state"));
                t = chart_number(cJSON_GetObjectItemCaseSensitive(item, "lu"));
                if (isnan(t)) {
                    t = chart_number(cJSON_GetObjectItemCaseSensitive(item, "lc"));
                }
            }
            if (isnan(v) || isnan(t)) {
                continue;
            }
            samples++;
            if (t < t0) {
                last_before = v;
                continue;
            }
            int b = (int)((t - t0) / span * CHART_POINTS);
            if (b < 0) {
                b = 0;
            }
            if (b >= CHART_POINTS) {
                b = CHART_POINTS - 1;
            }
            sum[b] += (float)v;
            cnt[b]++;
        }
    }

    float values[CHART_POINTS];
    /* Empty buckets carry the last value forward: raw history only records
     * changes, a flat stretch has no samples. */
    float carry = isnan(last_before) ? NAN : (float)last_before;
    for (int b = 0; b < CHART_POINTS; b++) {
        if (cnt[b] > 0) {
            carry = sum[b] / (float)cnt[b];
        }
        values[b] = carry;
    }

    if (s_stage_lock == NULL) {
        return;
    }
    xSemaphoreTake(s_stage_lock, portMAX_DELAY);
    chart_stage_t *stage = chart_stage_find(token);
    if (stage != NULL) {
        stage->ok = success;
        stage->statistics = statistics;
        stage->count = samples;
        memcpy(stage->values, values, sizeof(values));
        stage->ready = true;
    }
    xSemaphoreGive(s_stage_lock);
}

static void chart_release_stage(w_chart_ctx_t *ctx)
{
    if (ctx->stage >= 0 && s_stage_lock != NULL) {
        xSemaphoreTake(s_stage_lock, portMAX_DELAY);
        s_stages[ctx->stage].token = 0;
        s_stages[ctx->stage].ready = false;
        xSemaphoreGive(s_stage_lock);
    }
    ctx->stage = -1;
}

static bool chart_start_fetch(w_chart_ctx_t *ctx, int series, bool statistics)
{
    if (s_stage_lock == NULL) {
        return false;
    }
    xSemaphoreTake(s_stage_lock, portMAX_DELAY);
    int slot = -1;
    for (int i = 0; i < CHART_STAGES; i++) {
        if (s_stages[i].token == 0) {
            slot = i;
            break;
        }
    }
    uint32_t token = 0;
    if (slot >= 0) {
        /* Low 16 bits: the window in hours, needed by the callback. */
        s_token_seq = (s_token_seq + 1) & 0x7FFF;
        if (s_token_seq == 0) {
            s_token_seq = 1;
        }
        token = (s_token_seq << 16) | (uint32_t)(ctx->hours & 0xFFFF);
        s_stages[slot].token = token;
        s_stages[slot].ready = false;
    }
    xSemaphoreGive(s_stage_lock);
    if (slot < 0) {
        return false;
    }
    if (ha_client_request_history(ctx->entity[series], ctx->hours, statistics, chart_history_cb,
            (void *)(uintptr_t)token) != ESP_OK) {
        ctx->stage = slot;
        chart_release_stage(ctx);
        return false;
    }
    ctx->stage = slot;
    ctx->fetch_series = series;
    ctx->fetch_statistics = statistics;
    return true;
}

static void chart_render_line(w_chart_ctx_t *ctx)
{
    float lo = INFINITY;
    float hi = -INFINITY;
    for (int i = 0; i < ctx->n; i++) {
        for (int b = 0; b < ctx->hist_count[i]; b++) {
            const float v = ctx->hist[i][b];
            if (!isnan(v)) {
                lo = fminf(lo, v);
                hi = fmaxf(hi, v);
            }
        }
    }
    if (!isfinite(lo) || !isfinite(hi)) {
        lv_label_set_text(ctx->min_label, "");
        lv_label_set_text(ctx->max_label, ui_i18n_get("sensor.history_none", "No history available"));
        return;
    }
    if (hi - lo < 1e-6f) {
        hi = lo + 1.0f;
    }
    const float pad = (hi - lo) * 0.08f;
    lo -= pad;
    hi += pad;
    for (int i = 0; i < ctx->n; i++) {
        for (int b = 0; b < CHART_POINTS; b++) {
            const float v = b < ctx->hist_count[i] ? ctx->hist[i][b] : NAN;
            const int32_t y = isnan(v) ? LV_CHART_POINT_NONE : (int32_t)((v - lo) / (hi - lo) * CHART_Y_SCALE);
            lv_chart_set_value_by_id(ctx->chart, ctx->ser[i], (uint32_t)b, y);
        }
    }
    lv_chart_refresh(ctx->chart);
    char text[32];
    chart_format(text, sizeof(text), hi, ctx->n > 0 ? ctx->unit[0] : "");
    lv_label_set_text(ctx->max_label, text);
    chart_format(text, sizeof(text), lo, ctx->n > 0 ? ctx->unit[0] : "");
    lv_label_set_text(ctx->min_label, text);
}

static void chart_timer_cb(lv_timer_t *timer)
{
    w_chart_ctx_t *ctx = (w_chart_ctx_t *)lv_timer_get_user_data(timer);
    if (ctx == NULL || ctx->kind != CHART_LINE) {
        return;
    }
    const int64_t now = esp_timer_get_time();

    /* A response arrived? */
    if (ctx->fetch_series >= 0 && ctx->stage >= 0) {
        bool ready = false;
        bool ok = false;
        int count = 0;
        float values[CHART_POINTS];
        xSemaphoreTake(s_stage_lock, portMAX_DELAY);
        chart_stage_t *stage = &s_stages[ctx->stage];
        if (stage->ready) {
            ready = true;
            ok = stage->ok;
            count = stage->count;
            memcpy(values, stage->values, sizeof(values));
        }
        xSemaphoreGive(s_stage_lock);
        if (!ready) {
            return;
        }
        chart_release_stage(ctx);
        const int i = ctx->fetch_series;
        if (ok && count < 2 && ctx->fetch_statistics) {
            /* No long-term statistics (no state_class): try raw history. */
            if (!chart_start_fetch(ctx, i, false)) {
                ctx->fetch_series = -1;
                ctx->next_fetch_us = now + CHART_RETRY_US;
            }
            return;
        }
        memcpy(ctx->hist[i], values, sizeof(values));
        ctx->hist_count[i] = ok ? CHART_POINTS : 0;
        ctx->fetch_series = -1;
        chart_render_line(ctx);
        /* Next series right away; after the last one wait for the refresh. */
        if (i + 1 < ctx->n) {
            if (!chart_start_fetch(ctx, i + 1, true)) {
                ctx->next_fetch_us = now + CHART_RETRY_US;
            }
        } else {
            ctx->next_fetch_us = now + CHART_REFRESH_US;
        }
        return;
    }

    /* Only fetch for a chart that is on screen. */
    if (ctx->fetch_series < 0 && now >= ctx->next_fetch_us && !lv_obj_has_flag(ctx->card, LV_OBJ_FLAG_HIDDEN)) {
        if (!chart_start_fetch(ctx, 0, true)) {
            ctx->next_fetch_us = now + CHART_RETRY_US;
        }
    }
}

/* ---- construction -------------------------------------------------------- */

static void chart_build_bars(w_chart_ctx_t *ctx, const lv_font_t *font)
{
    lv_obj_set_flex_flow(ctx->body, ctx->kind == CHART_BAR ? LV_FLEX_FLOW_COLUMN : LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(ctx->body, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(ctx->body, 6, LV_PART_MAIN);
    const lv_color_t track_color = lv_color_mix(theme_default_color_text_muted(), lv_color_black(), 60);

    for (int i = 0; i < ctx->n; i++) {
        lv_obj_t *cell = chart_box(ctx->body);
        if (ctx->kind == CHART_BAR) {
            lv_obj_set_size(cell, LV_PCT(100), LV_SIZE_CONTENT);
            lv_obj_set_flex_grow(cell, 1);
            lv_obj_set_flex_flow(cell, LV_FLEX_FLOW_COLUMN);
            lv_obj_set_style_pad_row(cell, 2, LV_PART_MAIN);
            lv_obj_t *head = chart_box(cell);
            lv_obj_set_size(head, LV_PCT(100), LV_SIZE_CONTENT);
            ctx->name_label[i] = chart_label(head, font, theme_default_color_text_muted());
            lv_obj_align(ctx->name_label[i], LV_ALIGN_LEFT_MID, 0, 0);
            lv_label_set_long_mode(ctx->name_label[i], LV_LABEL_LONG_DOT);
            lv_obj_set_width(ctx->name_label[i], LV_PCT(60));
            ctx->value_label[i] = chart_label(head, font, theme_default_color_text_primary());
            lv_obj_align(ctx->value_label[i], LV_ALIGN_RIGHT_MID, 0, 0);
            ctx->track[i] = chart_box(cell);
            lv_obj_set_size(ctx->track[i], LV_PCT(100), 10);
            lv_obj_set_flex_grow(ctx->track[i], 0);
            ctx->fill[i] = chart_box(ctx->track[i]);
            lv_obj_set_size(ctx->fill[i], 0, LV_PCT(100));
        } else {
            lv_obj_set_size(cell, LV_SIZE_CONTENT, LV_PCT(100));
            lv_obj_set_flex_grow(cell, 1);
            lv_obj_set_flex_flow(cell, LV_FLEX_FLOW_COLUMN);
            lv_obj_set_flex_align(cell, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
            lv_obj_set_style_pad_row(cell, 2, LV_PART_MAIN);
            ctx->value_label[i] = chart_label(cell, font, theme_default_color_text_primary());
            ctx->track[i] = chart_box(cell);
            lv_obj_set_width(ctx->track[i], LV_PCT(70));
            lv_obj_set_flex_grow(ctx->track[i], 1);
            ctx->fill[i] = chart_box(ctx->track[i]);
            lv_obj_set_size(ctx->fill[i], LV_PCT(100), 0);
            lv_obj_align(ctx->fill[i], LV_ALIGN_BOTTOM_MID, 0, 0);
            ctx->name_label[i] = chart_label(cell, font, theme_default_color_text_muted());
            lv_label_set_long_mode(ctx->name_label[i], LV_LABEL_LONG_DOT);
            lv_obj_set_width(ctx->name_label[i], LV_PCT(100));
            lv_obj_set_style_text_align(ctx->name_label[i], LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
        }
        lv_obj_set_style_bg_color(ctx->track[i], track_color, LV_PART_MAIN);
        lv_obj_set_style_bg_opa(ctx->track[i], LV_OPA_40, LV_PART_MAIN);
        lv_obj_set_style_radius(ctx->track[i], 4, LV_PART_MAIN);
        lv_obj_set_style_bg_color(ctx->fill[i], ctx->color[i], LV_PART_MAIN);
        lv_obj_set_style_bg_opa(ctx->fill[i], LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_radius(ctx->fill[i], 4, LV_PART_MAIN);
    }
}

static void chart_build_pie(w_chart_ctx_t *ctx, const lv_font_t *font)
{
    /* Ring on the left (or on top of a tall tile), legend next to it. */
    const bool side = ctx->body_w >= ctx->body_h;
    int32_t ring = side ? LV_MIN(ctx->body_h, ctx->body_w * 55 / 100) : LV_MIN(ctx->body_w, ctx->body_h * 60 / 100);
    ring = LV_MAX(ring, 40);
    const int32_t thickness = LV_MAX(ring / 5, 8);

    lv_obj_t *holder = chart_box(ctx->body);
    lv_obj_set_size(holder, ring, ring);
    lv_obj_align(holder, side ? LV_ALIGN_LEFT_MID : LV_ALIGN_TOP_MID, 0, 0);
    for (int i = 0; i < ctx->n; i++) {
        lv_obj_t *arc = lv_arc_create(holder);
        lv_obj_set_size(arc, ring, ring);
        lv_obj_center(arc);
        lv_obj_remove_flag(arc, LV_OBJ_FLAG_CLICKABLE);
        lv_arc_set_rotation(arc, 270);
        lv_obj_set_style_arc_width(arc, thickness, LV_PART_MAIN);
        lv_obj_set_style_arc_color(arc, ctx->color[i], LV_PART_MAIN);
        lv_obj_set_style_arc_rounded(arc, false, LV_PART_MAIN);
        lv_obj_set_style_arc_opa(arc, LV_OPA_TRANSP, LV_PART_INDICATOR);
        lv_obj_set_style_bg_opa(arc, LV_OPA_TRANSP, LV_PART_KNOB);
        lv_obj_set_style_pad_all(arc, 0, LV_PART_KNOB);
        lv_obj_add_flag(arc, LV_OBJ_FLAG_HIDDEN);
        ctx->arc[i] = arc;
    }
    ctx->center_label = chart_label(holder, font, theme_default_color_text_primary());
    lv_obj_center(ctx->center_label);

    lv_obj_t *legend = chart_box(ctx->body);
    if (side) {
        lv_obj_set_size(legend, ctx->body_w - ring - 10, LV_SIZE_CONTENT);
        lv_obj_align(legend, LV_ALIGN_RIGHT_MID, 0, 0);
    } else {
        lv_obj_set_size(legend, LV_PCT(100), LV_SIZE_CONTENT);
        lv_obj_align(legend, LV_ALIGN_BOTTOM_MID, 0, 0);
    }
    lv_obj_set_flex_flow(legend, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(legend, 4, LV_PART_MAIN);
    for (int i = 0; i < ctx->n; i++) {
        ctx->value_label[i] = chart_label(legend, font, ctx->color[i]);
        lv_obj_set_width(ctx->value_label[i], LV_PCT(100));
        lv_label_set_long_mode(ctx->value_label[i], LV_LABEL_LONG_DOT);
    }
}

static void chart_build_line(w_chart_ctx_t *ctx, const lv_font_t *font)
{
    /* Legend row at the bottom, axis labels at the left top / bottom. */
    lv_obj_t *legend = chart_box(ctx->body);
    lv_obj_set_size(legend, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_align(legend, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    lv_obj_set_flex_flow(legend, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_column(legend, 10, LV_PART_MAIN);
    for (int i = 0; i < ctx->n; i++) {
        ctx->value_label[i] = chart_label(legend, font, ctx->color[i]);
    }
    lv_obj_update_layout(legend);
    const int32_t legend_h = lv_obj_get_height(legend);

    ctx->chart = lv_chart_create(ctx->body);
    lv_obj_set_size(ctx->chart, LV_PCT(100), LV_MAX(ctx->body_h - legend_h - 4, 30));
    lv_obj_align(ctx->chart, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_remove_flag(ctx->chart, LV_OBJ_FLAG_CLICKABLE);
    lv_chart_set_type(ctx->chart, LV_CHART_TYPE_LINE);
    lv_chart_set_point_count(ctx->chart, CHART_POINTS);
    lv_chart_set_axis_range(ctx->chart, LV_CHART_AXIS_PRIMARY_Y, 0, CHART_Y_SCALE);
    lv_chart_set_div_line_count(ctx->chart, 3, 0);
    lv_obj_set_style_bg_opa(ctx->chart, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(ctx->chart, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(ctx->chart, 2, LV_PART_MAIN);
    lv_obj_set_style_line_color(ctx->chart, theme_default_color_text_muted(), LV_PART_MAIN);
    lv_obj_set_style_line_opa(ctx->chart, LV_OPA_30, LV_PART_MAIN);
    lv_obj_set_style_line_width(ctx->chart, 2, LV_PART_ITEMS);
    lv_obj_set_style_width(ctx->chart, 0, LV_PART_INDICATOR);
    lv_obj_set_style_height(ctx->chart, 0, LV_PART_INDICATOR);
    for (int i = 0; i < ctx->n; i++) {
        ctx->ser[i] = lv_chart_add_series(ctx->chart, ctx->color[i], LV_CHART_AXIS_PRIMARY_Y);
        lv_chart_set_all_values(ctx->chart, ctx->ser[i], LV_CHART_POINT_NONE);
    }
    ctx->max_label = chart_label(ctx->body, APP_FONT_TEXT_14, theme_default_color_text_muted());
    lv_obj_align(ctx->max_label, LV_ALIGN_TOP_LEFT, 4, 0);
    lv_label_set_text(ctx->max_label, ui_i18n_get("sensor.history_loading", "Loading history..."));
    ctx->min_label = chart_label(ctx->body, APP_FONT_TEXT_14, theme_default_color_text_muted());
    lv_obj_align_to(ctx->min_label, ctx->chart, LV_ALIGN_BOTTOM_LEFT, 4, 0);
}

static void w_chart_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_DELETE) {
        return;
    }
    w_chart_ctx_t *ctx = (w_chart_ctx_t *)lv_event_get_user_data(e);
    if (ctx == NULL) {
        return;
    }
    if (ctx->timer != NULL) {
        lv_timer_delete(ctx->timer);
    }
    chart_release_stage(ctx);
    free(ctx);
}

static chart_kind_t chart_kind_from(const char *type)
{
    if (type != NULL) {
        if (strcmp(type, "column") == 0) {
            return CHART_COLUMN;
        }
        if (strcmp(type, "pie") == 0) {
            return CHART_PIE;
        }
        if (strcmp(type, "line") == 0) {
            return CHART_LINE;
        }
    }
    return CHART_BAR;
}

esp_err_t w_chart_create(const ui_widget_def_t *def, lv_obj_t *parent, ui_widget_instance_t *out_instance)
{
    if (def == NULL || parent == NULL || out_instance == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_stage_lock == NULL) {
        s_stages = heap_caps_calloc(CHART_STAGES, sizeof(chart_stage_t), MALLOC_CAP_SPIRAM);
        s_stage_lock = xSemaphoreCreateMutex();
        if (s_stages == NULL || s_stage_lock == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }
    w_chart_ctx_t *ctx = ui_calloc_prefer_psram(1, sizeof(w_chart_ctx_t));
    if (ctx == NULL) {
        return ESP_ERR_NO_MEM;
    }
    ctx->fetch_series = -1;
    ctx->stage = -1;
    ctx->hours = 24;

    const cJSON *json = def->json;
    const cJSON *type = cJSON_GetObjectItemCaseSensitive(json, "chart_type");
    ctx->kind = chart_kind_from(cJSON_IsString(type) ? type->valuestring : NULL);
    const cJSON *hours = cJSON_GetObjectItemCaseSensitive(json, "chart_hours");
    if (cJSON_IsNumber(hours) && hours->valueint >= 1 && hours->valueint <= 168) {
        ctx->hours = hours->valueint;
    }
    const cJSON *series = cJSON_GetObjectItemCaseSensitive(json, "chart_series");
    const cJSON *s = NULL;
    cJSON_ArrayForEach(s, series)
    {
        if (ctx->n >= CHART_SERIES_MAX) {
            break;
        }
        const cJSON *eid = cJSON_GetObjectItemCaseSensitive(s, "entity_id");
        if (!cJSON_IsString(eid) || eid->valuestring == NULL || eid->valuestring[0] == '\0') {
            continue;
        }
        const int i = ctx->n++;
        snprintf(ctx->entity[i], sizeof(ctx->entity[i]), "%s", eid->valuestring);
        const cJSON *color = cJSON_GetObjectItemCaseSensitive(s, "color");
        if (!cJSON_IsString(color) || !chart_parse_hex(color->valuestring, &ctx->color[i])) {
            ctx->color[i] = lv_color_hex(k_default_colors[i]);
        }
        const cJSON *name = cJSON_GetObjectItemCaseSensitive(s, "name");
        if (cJSON_IsString(name) && name->valuestring != NULL && name->valuestring[0] != '\0') {
            snprintf(ctx->name[i], sizeof(ctx->name[i]), "%s", name->valuestring);
            ctx->name_set[i] = true;
        }
    }

    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_set_pos(card, def->x, def->y);
    lv_obj_set_size(card, def->w, def->h);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    theme_default_style_card(card);
    lv_obj_set_style_pad_all(card, 10, LV_PART_MAIN);
    ctx->card = card;

    const lv_font_t *font = APP_LAYOUT_COMPACT ? APP_FONT_TEXT_14 : APP_FONT_TEXT_16;
    int32_t top = 0;
    if (def->title[0] != '\0') {
        ctx->title = lv_label_create(card);
        lv_obj_add_flag(ctx->title, LV_OBJ_FLAG_USER_1);
        lv_label_set_text(ctx->title, def->title);
        lv_obj_set_style_text_color(ctx->title, theme_default_color_text_muted(), LV_PART_MAIN);
        lv_obj_set_style_text_font(ctx->title, APP_LAYOUT_COMPACT ? APP_FONT_TEXT_16 : APP_FONT_TEXT_20, LV_PART_MAIN);
        lv_obj_update_layout(ctx->title);
        top = lv_obj_get_height(ctx->title) + 4;
    }
    ctx->body_w = def->w - 20;
    ctx->body_h = def->h - 20 - top;
    ctx->body = chart_box(card);
    lv_obj_set_size(ctx->body, ctx->body_w, ctx->body_h);
    lv_obj_set_pos(ctx->body, 0, top);

    if (ctx->n == 0) {
        lv_obj_t *hint = chart_label(ctx->body, font, theme_default_color_text_muted());
        lv_label_set_text(hint, ui_i18n_get("chart.no_series", "No sensors selected"));
        lv_obj_center(hint);
    } else if (ctx->kind == CHART_PIE) {
        chart_build_pie(ctx, font);
    } else if (ctx->kind == CHART_LINE) {
        chart_build_line(ctx, font);
        ctx->timer = lv_timer_create(chart_timer_cb, 500, ctx);
    } else {
        chart_build_bars(ctx, font);
    }

    lv_obj_add_event_cb(card, w_chart_event_cb, LV_EVENT_DELETE, ctx);
    ESP_LOGI(TAG, "%s: %s chart, %d series", def->id,
        ctx->kind == CHART_LINE ? "line" : ctx->kind == CHART_PIE ? "pie" : ctx->kind == CHART_COLUMN ? "column" : "bar",
        ctx->n);

    /* The runtime routes state changes of every series here. */
    for (int i = 0; i < ctx->n; i++) {
        snprintf(out_instance->chart_entities[i], sizeof(out_instance->chart_entities[i]), "%s", ctx->entity[i]);
    }
    if (ctx->n > 0 && out_instance->entity_id[0] == '\0') {
        snprintf(out_instance->entity_id, sizeof(out_instance->entity_id), "%s", ctx->entity[0]);
    }
    out_instance->obj = card;
    out_instance->ctx = ctx;
    return ESP_OK;
}

void w_chart_apply_state(ui_widget_instance_t *instance, const ha_state_t *state)
{
    LV_UNUSED(state);
    if (instance == NULL || instance->ctx == NULL) {
        return;
    }
    w_chart_ctx_t *ctx = (w_chart_ctx_t *)instance->ctx;
    if (ctx->n == 0) {
        return;
    }
    chart_read_values(ctx);
    if (ctx->kind == CHART_PIE) {
        chart_update_pie(ctx);
    } else if (ctx->kind == CHART_LINE) {
        chart_update_line_legend(ctx);
    } else {
        chart_update_bars(ctx);
    }
}

void w_chart_mark_unavailable(ui_widget_instance_t *instance)
{
    /* Each series shows "--" on its own; nothing tile-wide to do. */
    w_chart_apply_state(instance, NULL);
}
