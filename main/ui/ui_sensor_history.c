/* SPDX-License-Identifier: LicenseRef-FNCL-1.1 */
#include "ui/ui_sensor_history.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "lvgl.h"

#include "app_config.h"
#include "ha/ha_client.h"
#include "ui/fonts/app_text_fonts.h"
#include "ui/theme/theme_default.h"
#include "ui/ui_i18n.h"

#define TAG_SENSOR_HISTORY "sensor_hist"

#define HIST_MAX_POINTS 300
#define HIST_POLL_MS 100
#define HIST_RETRY_MS 600
#define HIST_TIMEOUT_MS 15000

static const int k_ranges_h[3] = {6, 24, 168};

/* ---- Hand-over between the HA receive task and the LVGL task ---------------- */

typedef struct {
    SemaphoreHandle_t lock;
    uint32_t seq;      /* request the data belongs to */
    bool ready;
    bool ok;
    bool statistics;   /* came from recorder statistics */
    int count;
    float values[HIST_MAX_POINTS];
    int64_t times_s[HIST_MAX_POINTS]; /* epoch seconds, 0 = unknown */
} hist_stage_t;

static hist_stage_t *s_stage;

/* ---- Popup state (LVGL task) -------------------------------------------------- */

typedef struct {
    lv_obj_t *overlay;
    lv_obj_t *chart;
    lv_chart_series_t *series;
    lv_obj_t *status;
    lv_obj_t *stats;
    lv_obj_t *axis_top;
    lv_obj_t *axis_bottom;
    lv_obj_t *axis_left;
    lv_obj_t *axis_right;
    lv_obj_t *range_btns[3];
    lv_timer_t *timer;
    char entity_id[APP_MAX_ENTITY_ID_LEN];
    int range_idx;
    uint32_t seq;
    bool request_pending;   /* not sent yet (gate busy / clock) */
    bool use_statistics;
    int64_t sent_ms;
    int64_t retry_at_ms;
} hist_popup_t;

static hist_popup_t s_pop;

static const char *hist_i18n(const char *key, const char *fallback)
{
    const char *text = ui_i18n_get(key, NULL);
    return (text != NULL && text[0] != '\0') ? text : fallback;
}

static bool hist_stage_init(void)
{
    if (s_stage != NULL) {
        return true;
    }
    s_stage = heap_caps_calloc(1, sizeof(*s_stage), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_stage == NULL) {
        return false;
    }
    s_stage->lock = xSemaphoreCreateMutex();
    if (s_stage->lock == NULL) {
        heap_caps_free(s_stage);
        s_stage = NULL;
        return false;
    }
    return true;
}

static double hist_number(const cJSON *item)
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

/* Runs in the HA receive task: parse, downsample, stage. */
static void hist_response_cb(bool success, cJSON *result, void *user)
{
    const uint32_t seq = (uint32_t)(uintptr_t)user;
    if (s_stage == NULL) {
        return;
    }

    float *values = heap_caps_malloc(sizeof(float) * HIST_MAX_POINTS, MALLOC_CAP_SPIRAM);
    int64_t *times = heap_caps_malloc(sizeof(int64_t) * HIST_MAX_POINTS, MALLOC_CAP_SPIRAM);
    int count = 0;
    bool statistics = false;

    const cJSON *list = (success && cJSON_IsObject(result)) ? result->child : NULL;
    if (values != NULL && times != NULL && cJSON_IsArray(list)) {
        const int total = cJSON_GetArraySize(list);
        /* Raw history can be long: average it down into HIST_MAX_POINTS buckets. */
        const int per_bucket = (total + HIST_MAX_POINTS - 1) / HIST_MAX_POINTS;
        double acc = 0.0;
        int acc_n = 0;
        int64_t acc_t = 0;
        int seen = 0;
        const cJSON *item = NULL;
        cJSON_ArrayForEach(item, list)
        {
            double v = NAN;
            int64_t t = 0;
            const cJSON *mean = cJSON_GetObjectItemCaseSensitive(item, "mean");
            if (mean != NULL) {
                statistics = true;
                v = hist_number(mean);
                const double start = hist_number(cJSON_GetObjectItemCaseSensitive(item, "start"));
                if (!isnan(start)) {
                    t = (int64_t)(start > 1e11 ? start / 1000.0 : start); /* ms or s */
                }
            } else {
                const cJSON *state = cJSON_GetObjectItemCaseSensitive(item, "s");
                if (state == NULL) {
                    state = cJSON_GetObjectItemCaseSensitive(item, "state");
                }
                v = hist_number(state);
                double when = hist_number(cJSON_GetObjectItemCaseSensitive(item, "lu"));
                if (isnan(when)) {
                    when = hist_number(cJSON_GetObjectItemCaseSensitive(item, "lc"));
                }
                if (!isnan(when)) {
                    t = (int64_t)when;
                }
            }
            seen++;
            if (!isnan(v)) {
                acc += v;
                acc_n++;
                if (acc_t == 0) {
                    acc_t = t;
                }
            }
            if (seen % per_bucket == 0 || seen == total) {
                if (acc_n > 0 && count < HIST_MAX_POINTS) {
                    values[count] = (float)(acc / acc_n);
                    times[count] = acc_t;
                    count++;
                }
                acc = 0.0;
                acc_n = 0;
                acc_t = 0;
            }
        }
    }

    xSemaphoreTake(s_stage->lock, portMAX_DELAY);
    if (seq == s_stage->seq) {
        s_stage->ok = success;
        s_stage->statistics = statistics;
        s_stage->count = count;
        if (count > 0) {
            memcpy(s_stage->values, values, sizeof(float) * (size_t)count);
            memcpy(s_stage->times_s, times, sizeof(int64_t) * (size_t)count);
        }
        s_stage->ready = true;
    }
    xSemaphoreGive(s_stage->lock);
    heap_caps_free(values);
    heap_caps_free(times);
}

/* ---- Rendering ----------------------------------------------------------------- */

static void hist_format_value(char *out, size_t len, float v)
{
    if (fabsf(v) >= 100.0f) {
        snprintf(out, len, "%.0f", (double)v);
    } else {
        snprintf(out, len, "%.1f", (double)v);
    }
}

static void hist_format_time(char *out, size_t len, int64_t epoch_s, bool long_range)
{
    if (epoch_s <= 0) {
        out[0] = '\0';
        return;
    }
    const time_t t = (time_t)epoch_s;
    struct tm tm_local;
    localtime_r(&t, &tm_local);
    if (long_range) {
        static const char *const keys[7] = {"weekday.su", "weekday.mo", "weekday.tu", "weekday.we",
                                            "weekday.th", "weekday.fr", "weekday.sa"};
        static const char *const fallback[7] = {"Su", "Mo", "Tu", "We", "Th", "Fr", "Sa"};
        snprintf(out, len, "%s %02d.%02d.", hist_i18n(keys[tm_local.tm_wday], fallback[tm_local.tm_wday]),
            tm_local.tm_mday, tm_local.tm_mon + 1);
    } else {
        snprintf(out, len, "%02d:%02d", tm_local.tm_hour, tm_local.tm_min);
    }
}

static void hist_set_status(const char *text)
{
    if (s_pop.status == NULL) {
        return;
    }
    if (text == NULL) {
        lv_obj_add_flag(s_pop.status, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_label_set_text(s_pop.status, text);
    lv_obj_clear_flag(s_pop.status, LV_OBJ_FLAG_HIDDEN);
}

static void hist_render(const hist_stage_t *data)
{
    if (s_pop.chart == NULL) {
        return;
    }
    if (data->count < 2) {
        lv_chart_set_point_count(s_pop.chart, 0);
        lv_label_set_text(s_pop.stats, "");
        lv_label_set_text(s_pop.axis_top, "");
        lv_label_set_text(s_pop.axis_bottom, "");
        lv_label_set_text(s_pop.axis_left, "");
        lv_label_set_text(s_pop.axis_right, "");
        hist_set_status(hist_i18n("sensor.history_none", "No history available"));
        return;
    }

    float vmin = data->values[0];
    float vmax = data->values[0];
    double sum = 0.0;
    for (int i = 0; i < data->count; i++) {
        vmin = fminf(vmin, data->values[i]);
        vmax = fmaxf(vmax, data->values[i]);
        sum += data->values[i];
    }
    const float avg = (float)(sum / data->count);
    float pad = (vmax - vmin) * 0.12f;
    if (pad < 0.5f) {
        pad = 0.5f;
    }
    /* lv_chart works in integers: scale by 10 for one decimal. */
    const int32_t lo = (int32_t)floorf((vmin - pad) * 10.0f);
    const int32_t hi = (int32_t)ceilf((vmax + pad) * 10.0f);
    lv_chart_set_point_count(s_pop.chart, (uint32_t)data->count);
    lv_chart_set_axis_range(s_pop.chart, LV_CHART_AXIS_PRIMARY_Y, lo, hi);
    for (int i = 0; i < data->count; i++) {
        lv_chart_set_value_by_id(s_pop.chart, s_pop.series, (uint32_t)i, (int32_t)lroundf(data->values[i] * 10.0f));
    }
    lv_chart_refresh(s_pop.chart);

    char a[48];
    char b[48];
    char c[48];
    char line[192];
    hist_format_value(a, sizeof(a), vmin);
    hist_format_value(b, sizeof(b), vmax);
    hist_format_value(c, sizeof(c), avg);
    snprintf(line, sizeof(line), "%s %s    %s %s    %s %s", hist_i18n("sensor.history_min", "Min"), a,
        hist_i18n("sensor.history_max", "Max"), b, hist_i18n("sensor.history_avg", "Avg"), c);
    lv_label_set_text(s_pop.stats, line);

    hist_format_value(a, sizeof(a), (float)hi / 10.0f);
    hist_format_value(b, sizeof(b), (float)lo / 10.0f);
    lv_label_set_text(s_pop.axis_top, a);
    lv_label_set_text(s_pop.axis_bottom, b);

    const bool long_range = k_ranges_h[s_pop.range_idx] > 24;
    hist_format_time(a, sizeof(a), data->times_s[0], long_range);
    lv_label_set_text(s_pop.axis_left, a);
    lv_label_set_text(s_pop.axis_right, hist_i18n("sensor.history_now", "now"));
    hist_set_status(NULL);
}

/* ---- Requests -------------------------------------------------------------------- */

static void hist_request(bool statistics)
{
    if (!hist_stage_init()) {
        hist_set_status(hist_i18n("sensor.history_none", "No history available"));
        return;
    }
    xSemaphoreTake(s_stage->lock, portMAX_DELAY);
    s_stage->seq++;
    s_stage->ready = false;
    s_pop.seq = s_stage->seq;
    xSemaphoreGive(s_stage->lock);

    s_pop.use_statistics = statistics;
    const int64_t now = (int64_t)lv_tick_get();
    esp_err_t err = ha_client_request_history(s_pop.entity_id, k_ranges_h[s_pop.range_idx], statistics,
        hist_response_cb, (void *)(uintptr_t)s_pop.seq);
    if (err == ESP_OK) {
        s_pop.request_pending = false;
        s_pop.sent_ms = now;
    } else {
        /* HA busy or clock not set yet: the poll timer retries. */
        s_pop.request_pending = true;
        s_pop.retry_at_ms = now + HIST_RETRY_MS;
        s_pop.sent_ms = now;
    }
}

static void hist_start_range(int idx)
{
    s_pop.range_idx = idx;
    for (int i = 0; i < 3; i++) {
        const bool active = (i == idx);
        lv_obj_set_style_bg_color(s_pop.range_btns[i],
            lv_color_hex(active ? APP_UI_COLOR_NAV_BTN_BG_ACTIVE : APP_UI_COLOR_NAV_BTN_BG_IDLE), LV_PART_MAIN);
        lv_obj_set_style_border_color(s_pop.range_btns[i],
            lv_color_hex(active ? APP_UI_COLOR_CARD_ICON_ON : APP_UI_COLOR_CARD_BORDER), LV_PART_MAIN);
    }
    lv_chart_set_point_count(s_pop.chart, 0);
    hist_set_status(hist_i18n("sensor.history_loading", "Loading..."));
    hist_request(true);
}

static void hist_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    if (s_pop.overlay == NULL || s_stage == NULL) {
        return;
    }
    const int64_t now = (int64_t)lv_tick_get();

    if (s_pop.request_pending) {
        if (now >= s_pop.retry_at_ms) {
            if (now - s_pop.sent_ms > HIST_TIMEOUT_MS) {
                s_pop.request_pending = false;
                hist_set_status(hist_i18n("sensor.history_none", "No history available"));
                return;
            }
            const int64_t first_try = s_pop.sent_ms;
            hist_request(s_pop.use_statistics);
            if (s_pop.request_pending) {
                s_pop.sent_ms = first_try; /* keep the overall timeout running */
            }
        }
        return;
    }

    bool ready = false;
    xSemaphoreTake(s_stage->lock, portMAX_DELAY);
    if (s_stage->ready && s_stage->seq == s_pop.seq) {
        ready = true;
        s_stage->ready = false;
    }
    xSemaphoreGive(s_stage->lock);

    if (!ready) {
        if (now - s_pop.sent_ms > HIST_TIMEOUT_MS) {
            hist_set_status(hist_i18n("sensor.history_none", "No history available"));
            s_pop.sent_ms = now + 1000000; /* show once */
        }
        return;
    }

    /* No long-term statistics for this entity: fall back to raw history. */
    if (s_pop.use_statistics && s_stage->count < 2) {
        hist_request(false);
        return;
    }
    hist_render(s_stage);
}

/* ---- Popup UI ---------------------------------------------------------------------- */

static void hist_overlay_delete_cb(lv_event_t *event)
{
    (void)event;
    if (s_pop.timer != NULL) {
        lv_timer_delete(s_pop.timer);
    }
    if (s_stage != NULL) {
        xSemaphoreTake(s_stage->lock, portMAX_DELAY);
        s_stage->seq++; /* drop late answers */
        xSemaphoreGive(s_stage->lock);
    }
    memset(&s_pop, 0, sizeof(s_pop));
}

static void hist_close_cb(lv_event_t *event)
{
    if (lv_event_get_target(event) == lv_event_get_current_target(event) && s_pop.overlay != NULL) {
        lv_obj_delete(s_pop.overlay);
    }
}

static void hist_range_cb(lv_event_t *event)
{
    const int idx = (int)(intptr_t)lv_event_get_user_data(event);
    if (idx != s_pop.range_idx) {
        hist_start_range(idx);
    }
}

static lv_obj_t *hist_label(lv_obj_t *parent, const lv_font_t *font, uint32_t color)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, "");
    lv_obj_set_style_text_font(label, font, LV_PART_MAIN);
    lv_obj_set_style_text_color(label, lv_color_hex(color), LV_PART_MAIN);
    return label;
}

void ui_sensor_history_open(const char *entity_id, const char *title, const char *value_text)
{
    if (entity_id == NULL || entity_id[0] == '\0' || s_pop.overlay != NULL) {
        return;
    }
    memset(&s_pop, 0, sizeof(s_pop));
    snprintf(s_pop.entity_id, sizeof(s_pop.entity_id), "%s", entity_id);

    lv_obj_t *overlay = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(overlay);
    lv_obj_set_size(overlay, LV_PCT(100), LV_PCT(100));
    lv_obj_add_flag(overlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(overlay, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(overlay, LV_OPA_60, LV_PART_MAIN);
    lv_obj_add_event_cb(overlay, hist_overlay_delete_cb, LV_EVENT_DELETE, NULL);
    lv_obj_add_event_cb(overlay, hist_close_cb, LV_EVENT_CLICKED, NULL);
    s_pop.overlay = overlay;

    lv_obj_update_layout(overlay);
    const lv_coord_t screen_w = lv_obj_get_width(overlay);
    const lv_coord_t screen_h = lv_obj_get_height(overlay);
    const lv_coord_t card_w = LV_MIN(screen_w - 40, 680);
    const lv_coord_t card_h = LV_MIN(screen_h - 40, 420);

    lv_obj_t *card = lv_obj_create(overlay);
    lv_obj_set_size(card, card_w, card_h);
    lv_obj_center(card);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_set_style_radius(card, 16, LV_PART_MAIN);
    lv_obj_set_style_bg_color(card, lv_color_hex(APP_UI_COLOR_CONTENT_BG), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(card, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(card, lv_color_hex(APP_UI_COLOR_CONTENT_BORDER), LV_PART_MAIN);
    lv_obj_set_style_pad_all(card, 16, LV_PART_MAIN);

    const lv_coord_t inner_w = card_w - 32;
    const lv_coord_t inner_h = card_h - 32;

    lv_obj_t *title_label = hist_label(card, APP_FONT_TEXT_20, APP_UI_COLOR_TEXT_MUTED);
    lv_label_set_text(title_label, (title != NULL && title[0] != '\0') ? title : entity_id);
    lv_label_set_long_mode(title_label, LV_LABEL_LONG_DOT);
    lv_obj_set_width(title_label, inner_w - 300);
    lv_obj_align(title_label, LV_ALIGN_TOP_LEFT, 0, 0);

    lv_obj_t *value_label = hist_label(card, APP_FONT_TEXT_28, APP_UI_COLOR_TEXT_PRIMARY);
    lv_label_set_text(value_label, value_text != NULL ? value_text : "");
    lv_obj_align(value_label, LV_ALIGN_TOP_LEFT, 0, 28);

    /* Range buttons + close, top right. */
    static const char *const range_keys[3] = {"sensor.history_6h", "sensor.history_24h", "sensor.history_7d"};
    static const char *const range_fallback[3] = {"6 h", "24 h", "7 d"};
    lv_obj_t *close = lv_obj_create(card);
    lv_obj_remove_style_all(close);
    lv_obj_set_size(close, 38, 38);
    lv_obj_add_flag(close, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_radius(close, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(close, lv_color_hex(APP_UI_COLOR_NAV_BTN_BG_IDLE), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(close, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_align(close, LV_ALIGN_TOP_RIGHT, 0, 0);
    lv_obj_t *close_label = hist_label(close, APP_FONT_TEXT_16, APP_UI_COLOR_TEXT_PRIMARY);
    lv_label_set_text(close_label, "X");
    lv_obj_center(close_label);
    lv_obj_add_event_cb(close, hist_close_cb, LV_EVENT_CLICKED, NULL);

    for (int i = 0; i < 3; i++) {
        lv_obj_t *btn = lv_obj_create(card);
        lv_obj_remove_style_all(btn);
        lv_obj_set_size(btn, 70, 38);
        lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_radius(btn, 12, LV_PART_MAIN);
        lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_border_width(btn, 1, LV_PART_MAIN);
        lv_obj_align(btn, LV_ALIGN_TOP_RIGHT, -(48 + (2 - i) * 78), 0);
        lv_obj_t *label = hist_label(btn, APP_FONT_TEXT_16, APP_UI_COLOR_TEXT_PRIMARY);
        lv_label_set_text(label, hist_i18n(range_keys[i], range_fallback[i]));
        lv_obj_center(label);
        lv_obj_add_event_cb(btn, hist_range_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        s_pop.range_btns[i] = btn;
    }

    /* Chart area with value labels on the left and time labels below. */
    const lv_coord_t chart_y = 84;
    const lv_coord_t axis_w = 52;
    const lv_coord_t chart_h = inner_h - chart_y - 52;
    lv_obj_t *chart = lv_chart_create(card);
    lv_obj_set_size(chart, inner_w - axis_w, chart_h);
    lv_obj_align(chart, LV_ALIGN_TOP_LEFT, axis_w, chart_y);
    lv_chart_set_type(chart, LV_CHART_TYPE_LINE);
    lv_chart_set_div_line_count(chart, 4, 0);
    lv_chart_set_update_mode(chart, LV_CHART_UPDATE_MODE_SHIFT);
    lv_chart_set_point_count(chart, 0);
    lv_obj_set_style_bg_opa(chart, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(chart, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(chart, 4, LV_PART_MAIN);
    lv_obj_set_style_line_color(chart, lv_color_hex(APP_UI_COLOR_CARD_BORDER), LV_PART_MAIN);
    lv_obj_set_style_line_opa(chart, LV_OPA_50, LV_PART_MAIN);
    lv_obj_set_style_line_width(chart, 3, LV_PART_ITEMS);
    lv_obj_set_style_width(chart, 0, LV_PART_INDICATOR); /* no point markers */
    lv_obj_set_style_height(chart, 0, LV_PART_INDICATOR);
    s_pop.series = lv_chart_add_series(chart, lv_color_hex(APP_UI_COLOR_CARD_ICON_ON), LV_CHART_AXIS_PRIMARY_Y);
    s_pop.chart = chart;

    s_pop.axis_top = hist_label(card, APP_FONT_TEXT_14, APP_UI_COLOR_TEXT_MUTED);
    lv_obj_align(s_pop.axis_top, LV_ALIGN_TOP_LEFT, 0, chart_y);
    s_pop.axis_bottom = hist_label(card, APP_FONT_TEXT_14, APP_UI_COLOR_TEXT_MUTED);
    lv_obj_align(s_pop.axis_bottom, LV_ALIGN_TOP_LEFT, 0, chart_y + chart_h - 20);
    s_pop.axis_left = hist_label(card, APP_FONT_TEXT_14, APP_UI_COLOR_TEXT_MUTED);
    lv_obj_align(s_pop.axis_left, LV_ALIGN_TOP_LEFT, axis_w, chart_y + chart_h + 4);
    s_pop.axis_right = hist_label(card, APP_FONT_TEXT_14, APP_UI_COLOR_TEXT_MUTED);
    lv_obj_align(s_pop.axis_right, LV_ALIGN_TOP_RIGHT, 0, chart_y + chart_h + 4);

    s_pop.stats = hist_label(card, APP_FONT_TEXT_16, APP_UI_COLOR_TEXT_SOFT);
    lv_obj_align(s_pop.stats, LV_ALIGN_BOTTOM_MID, 0, 0);

    s_pop.status = hist_label(card, APP_FONT_TEXT_18, APP_UI_COLOR_TEXT_MUTED);
    lv_obj_align(s_pop.status, LV_ALIGN_TOP_MID, axis_w / 2, chart_y + chart_h / 2 - 12);

    s_pop.timer = lv_timer_create(hist_timer_cb, HIST_POLL_MS, NULL);
    hist_start_range(1); /* 24 h */
}
