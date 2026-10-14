/* SPDX-License-Identifier: LicenseRef-FNCL-1.1 */
#include "ha/ha_cards.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "ha/ha_client.h"

static const char *TAG = "ha_cards";

#define HA_CARDS_SUB_KEY "panel_cards"
#define HA_CARDS_LABEL_LEN 48

static SemaphoreHandle_t s_lock;
static char s_label[HA_CARDS_LABEL_LEN];
static bool s_enabled;
static bool s_dirty;            /* configuration changed, (re)subscribe */
static bool s_subscribed;
static char *s_result;          /* latest rendering, JSON text */
static char s_error[160];
static uint32_t s_seq;

/* Everything a card might need; the panel picks the card type itself.
 * Only the attributes the cards use are passed, as strings; points in time
 * (end-time sensors, kick-off, media position) as unix seconds. */
static const char *const TEMPLATE_FMT =
    "{%%- set keep = ['device_class', 'unit_of_measurement', 'media_title', 'media_artist',"
    " 'media_series_title', 'media_season', 'media_episode', 'media_duration', 'media_position',"
    " 'media_position_updated_at', 'app_name', 'entity_picture', 'supported_features', 'sport', 'league_name', 'team_abbr',"
    " 'team_name', 'team_score', 'team_logo', 'team_homeaway', 'opponent_abbr', 'opponent_name',"
    " 'opponent_score', 'opponent_logo', 'date', 'clock'] -%%}"
    "{%%- set ns = namespace(o=[]) -%%}"
    "{%%- for s in expand(label_entities(%s)) -%%}"
    "{%%- set d = device_id(s.entity_id) -%%}"
    "{%%- set na = namespace(a=[]) -%%}"
    "{%%- for k in keep if k in s.attributes -%%}"
    "{%%- set v = s.attributes[k] -%%}"
    "{%%- if k in ['date', 'media_position_updated_at'] -%%}{%%- set v = as_timestamp(v, 0)|int -%%}{%%- endif -%%}"
    "{%%- set na.a = na.a + [[k, (v|string)[:300]]] -%%}"
    "{%%- endfor -%%}"
    "{%%- set t = as_timestamp(s.state, 0)|int if s.attributes.device_class|default('') == 'timestamp' else 0 -%%}"
    "{%%- set ns.o = ns.o + [{'i': s.entity_id, 's': s.state, 'n': s.name, 'd': d or '',"
    " 'dn': ((device_attr(d, 'name_by_user') or device_attr(d, 'name')) if d else '') or '',"
    " 't': t, 'a': na.a}] -%%}"
    "{%%- endfor -%%}"
    "{{ ns.o|to_json }}";

static void ha_cards_lock(void)
{
    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutex();
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
}

static void ha_cards_unlock(void)
{
    xSemaphoreGive(s_lock);
}

static char *ha_cards_strdup_psram(const char *src)
{
    size_t len = strlen(src) + 1;
    char *out = heap_caps_malloc(len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (out == NULL) {
        out = malloc(len);
    }
    if (out != NULL) {
        memcpy(out, src, len);
    }
    return out;
}

/* WS receive task. */
static void ha_cards_event_cb(cJSON *event, void *user)
{
    (void)user;
    cJSON *result = cJSON_GetObjectItemCaseSensitive(event, "result");
    cJSON *error = cJSON_GetObjectItemCaseSensitive(event, "error");
    char *text = NULL;
    if (cJSON_IsString(result) && result->valuestring != NULL) {
        text = ha_cards_strdup_psram(result->valuestring);
    } else if (result != NULL) {
        /* HA parses JSON-like output into a native value. */
        char *printed = cJSON_PrintUnformatted(result);
        if (printed != NULL) {
            text = ha_cards_strdup_psram(printed);
            cJSON_free(printed);
        }
    }
    ha_cards_lock();
    if (text != NULL) {
        free(s_result);
        s_result = text;
        s_error[0] = '\0';
    } else if (cJSON_IsString(error) && error->valuestring != NULL) {
        snprintf(s_error, sizeof(s_error), "%s", error->valuestring);
        ESP_LOGW(TAG, "template error: %s", s_error);
    }
    s_seq++;
    ha_cards_unlock();
}

void ha_cards_configure(const char *label, bool enabled)
{
    ha_cards_lock();
    const char *want = (label != NULL) ? label : "";
    if (strcmp(s_label, want) != 0 || s_enabled != enabled) {
        snprintf(s_label, sizeof(s_label), "%s", want);
        s_enabled = enabled;
        s_dirty = true;
    }
    ha_cards_unlock();
}

static char *ha_cards_build_body(const char *label)
{
    cJSON *label_json = cJSON_CreateString(label);
    char *label_literal = label_json != NULL ? cJSON_PrintUnformatted(label_json) : NULL;
    cJSON_Delete(label_json);
    if (label_literal == NULL) {
        return NULL;
    }
    char template_text[2048];
    int n = snprintf(template_text, sizeof(template_text), TEMPLATE_FMT, label_literal);
    cJSON_free(label_literal);
    if (n <= 0 || (size_t)n >= sizeof(template_text)) {
        return NULL;
    }
    cJSON *body = cJSON_CreateObject();
    if (body == NULL) {
        return NULL;
    }
    cJSON_AddStringToObject(body, "template", template_text);
    cJSON_AddBoolToObject(body, "report_errors", true);
    char *out = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);
    return out;
}

void ha_cards_tick(void)
{
    ha_cards_lock();
    if (!s_dirty) {
        ha_cards_unlock();
        return;
    }
    char label[HA_CARDS_LABEL_LEN];
    snprintf(label, sizeof(label), "%s", s_label);
    const bool enabled = s_enabled && label[0] != '\0';
    ha_cards_unlock();

    esp_err_t err = ESP_OK;
    if (enabled) {
        char *body = ha_cards_build_body(label);
        if (body == NULL) {
            return;
        }
        err = ha_client_ws_subscribe_ex(HA_CARDS_SUB_KEY, "render_template", body, ha_cards_event_cb, NULL);
        cJSON_free(body);
    } else {
        ha_client_ws_unsubscribe(HA_CARDS_SUB_KEY);
    }
    if (err != ESP_OK) {
        return; /* HA client not up yet: next tick */
    }
    ha_cards_lock();
    s_dirty = false;
    s_subscribed = enabled;
    if (!enabled) {
        free(s_result);
        s_result = NULL;
        s_seq++;
    }
    ha_cards_unlock();
    ESP_LOGI(TAG, "cards %s (label \"%s\")", enabled ? "subscribed" : "off", label);
}

void ha_cards_demo(bool on)
{
    if (!on) {
        /* A fresh subscription renders the real list right away. */
        ha_client_ws_unsubscribe(HA_CARDS_SUB_KEY);
        ha_cards_lock();
        s_dirty = true;
        ha_cards_unlock();
        return;
    }
    const long now = (long)time(NULL);
    char demo[1800];
    snprintf(demo, sizeof(demo),
        "["
        "{\"i\":\"sensor.demo_end\",\"s\":\"x\",\"n\":\"Demo end\",\"d\":\"demo1\",\"dn\":\"Waschmaschine\","
        "\"t\":%ld,\"a\":[[\"device_class\",\"timestamp\"]]},"
        "{\"i\":\"sensor.demo_progress\",\"s\":\"64\",\"n\":\"Demo progress\",\"d\":\"demo1\","
        "\"dn\":\"Waschmaschine\",\"t\":0,\"a\":[[\"unit_of_measurement\",\"%%\"]]},"
        "{\"i\":\"sensor.demo_team\",\"s\":\"IN\",\"n\":\"Demo team\",\"d\":\"\",\"dn\":\"\",\"t\":0,"
        "\"a\":[[\"sport\",\"soccer\"],[\"league_name\",\"Spanish LALIGA\"],[\"team_abbr\",\"BAR\"],"
        "[\"team_score\",\"2\"],[\"team_homeaway\",\"away\"],[\"opponent_abbr\",\"BET\"],"
        "[\"opponent_score\",\"1\"],[\"date\",\"%ld\"],[\"clock\",\"67'\"],"
        "[\"team_logo\",\"https://a.espncdn.com/i/teamlogos/soccer/500/83.png\"],"
        "[\"opponent_logo\",\"https://a.espncdn.com/i/teamlogos/soccer/500/244.png\"]]},"
        "{\"i\":\"media_player.demo\",\"s\":\"playing\",\"n\":\"Demo player\",\"d\":\"demo2\",\"dn\":\"Shield\","
        "\"t\":0,\"a\":[[\"app_name\",\"Plex\"],[\"media_series_title\",\"The Expanse\"],"
        "[\"media_title\",\"Leviathan Wakes\"],[\"media_season\",\"1\"],[\"media_episode\",\"10\"],"
        "[\"supported_features\",\"16433\"],[\"entity_picture\",\"https://a.espncdn.com/i/leaguelogos/soccer/500/15.png\"],"
        "[\"media_duration\",\"2640\"],[\"media_position\",\"1500\"],[\"media_position_updated_at\",\"%ld\"]]}"
        "]",
        now + 83 * 60, now - 67 * 60, now);
    char *text = ha_cards_strdup_psram(demo);
    if (text == NULL) {
        return;
    }
    ha_cards_lock();
    free(s_result);
    s_result = text;
    s_seq++;
    ha_cards_unlock();
}

uint32_t ha_cards_seq(void)
{
    ha_cards_lock();
    uint32_t seq = s_seq;
    ha_cards_unlock();
    return seq;
}

cJSON *ha_cards_entities(void)
{
    ha_cards_lock();
    cJSON *out = (s_result != NULL) ? cJSON_Parse(s_result) : NULL;
    ha_cards_unlock();
    if (out != NULL && !cJSON_IsArray(out)) {
        cJSON_Delete(out);
        out = NULL;
    }
    return out;
}

void ha_cards_status_json(cJSON *root)
{
    if (root == NULL) {
        return;
    }
    ha_cards_lock();
    cJSON_AddStringToObject(root, "label", s_label);
    cJSON_AddBoolToObject(root, "enabled", s_enabled);
    cJSON_AddBoolToObject(root, "subscribed", s_subscribed);
    cJSON_AddStringToObject(root, "error", s_error);
    cJSON_AddNumberToObject(root, "seq", s_seq);
    cJSON *entities = (s_result != NULL) ? cJSON_Parse(s_result) : NULL;
    ha_cards_unlock();
    if (entities != NULL) {
        cJSON_AddItemToObject(root, "entities", entities);
    } else {
        cJSON_AddNullToObject(root, "entities");
    }
}
