/* SPDX-License-Identifier: LicenseRef-FNCL-1.1 */
/* Screensaver cards ("Google Now" style), one row next to the clock.
 *
 * Built from the entities that carry the cards label in HA:
 *   - remaining time: sensors of one device, grouped. A timestamp sensor is
 *     the end time, a min/h/s sensor the remaining duration, a % sensor the
 *     progress. Shown while something is running.
 *   - sport: Team Tracker sensors (attributes "sport" + "team_abbr"), with
 *     both team logos. Live games, the next game from 24 h before kick-off,
 *     the result for a while after it.
 *   - now playing: media players that play or are paused, with cover and
 *     previous / play-pause / next. Touching a media card keeps the saver up.
 */
#include "ui/ui_saver_cards.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "app_config.h"
#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "ha/ha_cards.h"
#include "ha/ha_client.h"
#include "ha/ha_cover_fetcher.h"
#include "ui/fonts/app_text_fonts.h"
#include "ui/ui_i18n.h"
#include "ui/ui_mdi_icon.h"

static const char *TAG = "saver_cards";

#define CARDS_MAX 6
#define CARD_TEXT_LEN 96
#define CARD_URL_LEN 320

#define CARDS_BIG_SCREEN (APP_SCREEN_WIDTH >= 1000)
#define CARD_W (CARDS_BIG_SCREEN ? 360 : 236)
#define CARD_GAP (CARDS_BIG_SCREEN ? 20 : 12)
#define CARD_PAD (CARDS_BIG_SCREEN ? 16 : 10)
#define CARDS_MARGIN (CARDS_BIG_SCREEN ? 28 : 14)
#define CARDS_VISIBLE 3
/* A media card takes two card slots: cover + text + controls. */
#define MEDIA_CARD_W (2 * CARD_W + CARD_GAP)
#define COVER_PX (CARDS_BIG_SCREEN ? 168 : 112)
#define LOGO_PX (CARDS_BIG_SCREEN ? 60 : 40)
#define CTRL_ICON_PX (CARDS_BIG_SCREEN ? 44 : 30)
#define CTRL_PLAY_PX (CARDS_BIG_SCREEN ? 54 : 38)

/* Sport: show the next game from this long before kick-off, the result
 * this long after it. */
#define SPORT_PRE_WINDOW_S (24 * 3600)
#define SPORT_POST_WINDOW_S (14 * 3600)

/* media_player supported_features bits (HA MediaPlayerEntityFeature). */
#define MP_FEAT_PAUSE 1U
#define MP_FEAT_PREVIOUS 16U
#define MP_FEAT_NEXT 32U
#define MP_FEAT_PLAY 16384U

/* Decoded images (logos, covers), kept across card rebuilds. */
#define IMG_SLOTS 8
#define IMG_RETRY_S 300

typedef enum {
    CARD_REMAIN = 0,
    CARD_SPORT,
    CARD_MEDIA,
} card_kind_t;

typedef struct {
    card_kind_t kind;
    int order; /* lower first */
    char key[APP_MAX_ENTITY_ID_LEN];
    char header[CARD_TEXT_LEN];
    /* remaining time */
    int64_t end_ts;   /* 0 = none */
    int64_t remain_s; /* -1 = none */
    float progress;   /* -1 = none, 0..100 */
    /* sport */
    char line[CARD_TEXT_LEN];
    char sport_state[8];
    char clock[24];
    int64_t kickoff_ts;
    char logo_home[CARD_URL_LEN];
    char logo_away[CARD_URL_LEN];
    /* media */
    char title[CARD_TEXT_LEN];
    char sub[CARD_TEXT_LEN];
    char picture[CARD_URL_LEN];
    bool playing;
    uint32_t features; /* MP_FEAT_* */
    float duration;
    float position;
    int64_t position_ts;
} card_t;

typedef enum {
    CTRL_PREV = 0,
    CTRL_PLAY,
    CTRL_NEXT,
    CTRL_COUNT,
} card_ctrl_t;

typedef struct {
    card_kind_t kind;
    lv_obj_t *box;
    lv_obj_t *header;
    lv_obj_t *big;
    lv_obj_t *sub;
    lv_obj_t *bar;
    lv_obj_t *img[2]; /* sport: home, away logo; media: cover */
    lv_obj_t *ctrl[CTRL_COUNT];
    lv_obj_t *ctrl_icon[CTRL_COUNT];
    int play_icon; /* -1 unknown, 0 play, 1 pause */
} card_view_t;

typedef struct {
    char url[CARD_URL_LEN];
    ha_cover_result_t res;
    bool loaded;
    bool pending;
    bool used; /* wanted by a card in the current pass */
    int64_t failed_at;
    int64_t last_used;
} card_img_t;

/* The card tables live in PSRAM (about 40 KB): the internal RAM is what
 * Wi-Fi and TLS run short of. Allocated on first use. */
static lv_obj_t *s_row;
static card_t *s_cards;     /* [CARDS_MAX] */
static card_t *s_old_cards; /* [CARDS_MAX], previous pass */
static int s_card_count;
static card_view_t *s_views; /* [CARDS_MAX] */
static int s_view_count;
static uint32_t s_seen_seq = UINT32_MAX;
static bool s_was_visible;
static int32_t s_last_top = -1;
static int32_t s_last_bottom = -1;
static card_img_t *s_imgs; /* [IMG_SLOTS] */
static bool s_img_arrived;

static bool cards_alloc(void)
{
    if (s_cards != NULL) {
        return true;
    }
    card_t *cards = heap_caps_calloc(CARDS_MAX, sizeof(card_t), MALLOC_CAP_SPIRAM);
    card_t *old = heap_caps_calloc(CARDS_MAX, sizeof(card_t), MALLOC_CAP_SPIRAM);
    card_view_t *views = heap_caps_calloc(CARDS_MAX, sizeof(card_view_t), MALLOC_CAP_SPIRAM);
    card_img_t *imgs = heap_caps_calloc(IMG_SLOTS, sizeof(card_img_t), MALLOC_CAP_SPIRAM);
    if (cards == NULL || old == NULL || views == NULL || imgs == NULL) {
        heap_caps_free(cards);
        heap_caps_free(old);
        heap_caps_free(views);
        heap_caps_free(imgs);
        return false;
    }
    s_old_cards = old;
    s_views = views;
    s_imgs = imgs;
    s_cards = cards;
    return true;
}

/* ------------------------------------------------------------------ data */

static const char *card_attr(const cJSON *entity, const char *key)
{
    const cJSON *attrs = cJSON_GetObjectItemCaseSensitive(entity, "a");
    const cJSON *pair = NULL;
    cJSON_ArrayForEach(pair, attrs)
    {
        const cJSON *k = cJSON_GetArrayItem(pair, 0);
        const cJSON *v = cJSON_GetArrayItem(pair, 1);
        if (cJSON_IsString(k) && cJSON_IsString(v) && strcmp(k->valuestring, key) == 0) {
            const char *s = v->valuestring;
            if (s[0] == '\0' || strcmp(s, "None") == 0) {
                return NULL;
            }
            return s;
        }
    }
    return NULL;
}

static const char *card_str(const cJSON *entity, const char *key)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(entity, key);
    return (cJSON_IsString(item) && item->valuestring != NULL) ? item->valuestring : "";
}

static bool card_number(const char *text, double *out)
{
    if (text == NULL || text[0] == '\0') {
        return false;
    }
    char *end = NULL;
    double v = strtod(text, &end);
    if (end == text || !isfinite(v)) {
        return false;
    }
    *out = v;
    return true;
}

static bool card_state_usable(const char *state)
{
    return state[0] != '\0' && strcmp(state, "unavailable") != 0 && strcmp(state, "unknown") != 0;
}

static card_t *card_find_or_add(card_kind_t kind, const char *key)
{
    for (int i = 0; i < s_card_count; i++) {
        if (s_cards[i].kind == kind && strcmp(s_cards[i].key, key) == 0) {
            return &s_cards[i];
        }
    }
    if (s_card_count >= CARDS_MAX) {
        return NULL;
    }
    card_t *c = &s_cards[s_card_count++];
    memset(c, 0, sizeof(*c));
    c->kind = kind;
    c->remain_s = -1;
    c->progress = -1.0f;
    snprintf(c->key, sizeof(c->key), "%s", key);
    return c;
}

static void card_add_sport(const cJSON *e, const char *state, int64_t now)
{
    double kickoff = 0;
    (void)card_number(card_attr(e, "date"), &kickoff);
    const bool live = strcmp(state, "IN") == 0;
    const bool pre = strcmp(state, "PRE") == 0 && kickoff > 0 && kickoff - (double)now < SPORT_PRE_WINDOW_S;
    const bool post = strcmp(state, "POST") == 0 && kickoff > 0 && (double)now - kickoff < SPORT_POST_WINDOW_S;
    if (!live && !pre && !post) {
        return;
    }
    card_t *c = card_find_or_add(CARD_SPORT, card_str(e, "i"));
    if (c == NULL) {
        return;
    }
    c->order = live ? 0 : (pre ? 4 : 5);
    c->kickoff_ts = (int64_t)kickoff;
    snprintf(c->sport_state, sizeof(c->sport_state), "%s", state);
    const char *league = card_attr(e, "league_name");
    snprintf(c->header, sizeof(c->header), "%s", league ? league : card_str(e, "n"));
    const char *clock = card_attr(e, "clock");
    snprintf(c->clock, sizeof(c->clock), "%s", clock ? clock : "");

    const char *team = card_attr(e, "team_abbr");
    const char *opp = card_attr(e, "opponent_abbr");
    const char *ts = card_attr(e, "team_score");
    const char *os = card_attr(e, "opponent_score");
    const char *tl = card_attr(e, "team_logo");
    const char *ol = card_attr(e, "opponent_logo");
    const char *ha = card_attr(e, "team_homeaway");
    const bool team_home = ha == NULL || strcmp(ha, "away") != 0;
    const char *home = team_home ? team : opp;
    const char *away = team_home ? opp : team;
    const char *home_score = team_home ? ts : os;
    const char *away_score = team_home ? os : ts;
    snprintf(c->logo_home, sizeof(c->logo_home), "%s", (team_home ? tl : ol) ? (team_home ? tl : ol) : "");
    snprintf(c->logo_away, sizeof(c->logo_away), "%s", (team_home ? ol : tl) ? (team_home ? ol : tl) : "");
    if (pre || home_score == NULL || away_score == NULL) {
        snprintf(c->line, sizeof(c->line), "%s – %s", home ? home : "?", away ? away : "?");
    } else {
        snprintf(c->line, sizeof(c->line), "%s %s:%s %s", home ? home : "?", home_score, away_score,
            away ? away : "?");
    }
}

static void card_add_media(const cJSON *e, const char *state)
{
    const bool playing = strcmp(state, "playing") == 0;
    if (!playing && strcmp(state, "paused") != 0) {
        return;
    }
    card_t *c = card_find_or_add(CARD_MEDIA, card_str(e, "i"));
    if (c == NULL) {
        return;
    }
    c->order = playing ? 1 : 3;
    c->playing = playing;
    const char *dn = card_str(e, "dn");
    const char *app = card_attr(e, "app_name");
    snprintf(c->header, sizeof(c->header), "%s", app ? app : (dn[0] ? dn : card_str(e, "n")));

    const char *title = card_attr(e, "media_title");
    const char *series = card_attr(e, "media_series_title");
    const char *artist = card_attr(e, "media_artist");
    const char *season = card_attr(e, "media_season");
    const char *episode = card_attr(e, "media_episode");
    if (series != NULL) {
        snprintf(c->title, sizeof(c->title), "%s", series);
        if (season != NULL && episode != NULL) {
            snprintf(c->sub, sizeof(c->sub), "S%s E%s · %s", season, episode, title ? title : "");
        } else {
            snprintf(c->sub, sizeof(c->sub), "%s", title ? title : "");
        }
    } else {
        snprintf(c->title, sizeof(c->title), "%s", title ? title : "");
        snprintf(c->sub, sizeof(c->sub), "%s", artist ? artist : "");
    }
    double feat = 0;
    c->features = card_number(card_attr(e, "supported_features"), &feat) ? (uint32_t)feat : 0;
    const char *picture = card_attr(e, "entity_picture");
    snprintf(c->picture, sizeof(c->picture), "%s", picture ? picture : "");
    double v = 0;
    c->duration = card_number(card_attr(e, "media_duration"), &v) ? (float)v : 0.0f;
    c->position = card_number(card_attr(e, "media_position"), &v) ? (float)v : 0.0f;
    c->position_ts = card_number(card_attr(e, "media_position_updated_at"), &v) ? (int64_t)v : 0;
}

/* One sensor of a remaining-time device. */
static void card_add_remain_part(const cJSON *e, const char *state)
{
    const char *dc = card_attr(e, "device_class");
    const char *unit = card_attr(e, "unit_of_measurement");
    const cJSON *t = cJSON_GetObjectItemCaseSensitive(e, "t");
    double v = 0;
    const bool numeric = card_number(state, &v);
    enum { PART_NONE, PART_END, PART_REMAIN, PART_PROGRESS } part = PART_NONE;
    if (dc != NULL && strcmp(dc, "timestamp") == 0) {
        part = PART_END;
    } else if (unit != NULL && strcmp(unit, "%") == 0 && numeric) {
        part = PART_PROGRESS;
    } else if (unit != NULL && numeric &&
               (strcmp(unit, "min") == 0 || strcmp(unit, "h") == 0 || strcmp(unit, "s") == 0)) {
        part = PART_REMAIN;
    }
    if (part == PART_NONE) {
        return;
    }
    const char *device = card_str(e, "d");
    card_t *c = card_find_or_add(CARD_REMAIN, device[0] ? device : card_str(e, "i"));
    if (c == NULL) {
        return;
    }
    c->order = 2;
    if (c->header[0] == '\0') {
        const char *dn = card_str(e, "dn");
        snprintf(c->header, sizeof(c->header), "%s", dn[0] ? dn : card_str(e, "n"));
    }
    if (part == PART_END) {
        c->end_ts = cJSON_IsNumber(t) ? (int64_t)t->valuedouble : 0;
    } else if (part == PART_PROGRESS) {
        c->progress = (float)v;
    } else {
        const double scale = strcmp(unit, "h") == 0 ? 3600.0 : (strcmp(unit, "min") == 0 ? 60.0 : 1.0);
        c->remain_s = (int64_t)(v * scale);
    }
}

static bool card_remain_running(const card_t *c, int64_t now)
{
    if (c->end_ts > now && c->end_ts - now < 3 * 24 * 3600) {
        return true;
    }
    if (c->remain_s > 0) {
        return true;
    }
    return c->progress > 0.0f && c->progress < 100.0f;
}

static int card_cmp(const void *a, const void *b)
{
    const card_t *ca = a;
    const card_t *cb = b;
    if (ca->order != cb->order) {
        return ca->order - cb->order;
    }
    if (ca->kind == CARD_SPORT && cb->kind == CARD_SPORT && ca->kickoff_ts != cb->kickoff_ts) {
        return ca->kickoff_ts < cb->kickoff_ts ? -1 : 1;
    }
    return strcmp(ca->key, cb->key);
}

static void cards_rebuild_data(int64_t now)
{
    s_card_count = 0;
    cJSON *list = ha_cards_entities();
    const cJSON *e = NULL;
    cJSON_ArrayForEach(e, list)
    {
        const char *id = card_str(e, "i");
        const char *state = card_str(e, "s");
        if (!card_state_usable(state)) {
            continue;
        }
        if (strncmp(id, "media_player.", 13) == 0) {
            card_add_media(e, state);
        } else if (card_attr(e, "sport") != NULL && card_attr(e, "team_abbr") != NULL) {
            card_add_sport(e, state, now);
        } else if (strncmp(id, "sensor.", 7) == 0) {
            card_add_remain_part(e, state);
        }
    }
    cJSON_Delete(list);

    /* Drop remaining-time devices that are idle. */
    int n = 0;
    for (int i = 0; i < s_card_count; i++) {
        if (s_cards[i].kind == CARD_REMAIN && !card_remain_running(&s_cards[i], now)) {
            continue;
        }
        if (n != i) {
            s_cards[n] = s_cards[i];
        }
        n++;
    }
    s_card_count = n;
    qsort(s_cards, (size_t)s_card_count, sizeof(card_t), card_cmp);
    if (s_card_count > CARDS_VISIBLE) {
        s_card_count = CARDS_VISIBLE;
    }
}

/* ------------------------------------------------------------------ images */

/* LVGL task (lv_async_call from the fetcher). */
static void card_img_cb(void *user, const ha_cover_result_t *result)
{
    card_img_t *slot = user;
    slot->pending = false;
    if (result == NULL) {
        slot->failed_at = (int64_t)time(NULL);
        return;
    }
    ha_cover_result_t copy = *result;
    if (!copy.valid || copy.image.data == NULL) {
        ha_cover_result_release(&copy);
        slot->failed_at = (int64_t)time(NULL);
        ESP_LOGW(TAG, "image failed: %.80s", slot->url);
        return;
    }
    slot->res = copy;
    slot->loaded = true;
    s_img_arrived = true;
}

static void card_img_free(card_img_t *slot)
{
    if (slot->pending) {
        ha_cover_fetcher_cancel(slot);
    }
    if (slot->loaded) {
        ha_cover_result_release(&slot->res);
    }
    memset(slot, 0, sizeof(*slot));
}

/* ESPN serves its logos at 500 px; its resizer hands out small ones. */
static void card_img_source_url(const char *url, int px, char *out, size_t out_len)
{
    static const char ESPN[] = "https://a.espncdn.com/i/";
    if (strncmp(url, ESPN, sizeof(ESPN) - 1) == 0 && strchr(url, '?') == NULL) {
        snprintf(out, out_len, "https://a.espncdn.com/combiner/i?img=%s&w=%d&h=%d", url + strlen("https://a.espncdn.com"),
            px, px);
    } else {
        snprintf(out, out_len, "%s", url);
    }
}

/* The decoded image for `url`, or NULL while it loads (the request is sent). */
static const lv_image_dsc_t *card_img_get(const char *url, int px)
{
    if (url == NULL || url[0] == '\0') {
        return NULL;
    }
    const int64_t now = (int64_t)time(NULL);
    for (int i = 0; i < IMG_SLOTS; i++) {
        card_img_t *slot = &s_imgs[i];
        if (strcmp(slot->url, url) != 0) {
            continue;
        }
        slot->used = true;
        slot->last_used = now;
        if (slot->loaded) {
            return &slot->res.image;
        }
        if (slot->pending || (slot->failed_at != 0 && now - slot->failed_at < IMG_RETRY_S)) {
            return NULL;
        }
        break; /* retry below in the same slot */
    }

    card_img_t *slot = NULL;
    for (int i = 0; i < IMG_SLOTS && slot == NULL; i++) {
        if (strcmp(s_imgs[i].url, url) == 0) {
            slot = &s_imgs[i];
        }
    }
    for (int i = 0; i < IMG_SLOTS && slot == NULL; i++) {
        if (s_imgs[i].url[0] == '\0') {
            slot = &s_imgs[i];
        }
    }
    if (slot == NULL) {
        /* Least recently used one that no card wants right now. */
        for (int i = 0; i < IMG_SLOTS; i++) {
            if (!s_imgs[i].used && (slot == NULL || s_imgs[i].last_used < slot->last_used)) {
                slot = &s_imgs[i];
            }
        }
    }
    if (slot == NULL) {
        return NULL;
    }
    card_img_free(slot);
    snprintf(slot->url, sizeof(slot->url), "%s", url);
    slot->used = true;
    slot->last_used = now;

    char source[CARD_URL_LEN + 64];
    card_img_source_url(url, px, source, sizeof(source));
    esp_err_t err;
    if (url[0] == '/') {
        err = ha_cover_fetcher_request(source, px, px, card_img_cb, slot); /* HA proxy */
    } else {
        err = ha_cover_fetcher_request_auth(source, NULL, NULL, px, px, card_img_cb, slot);
    }
    if (err == ESP_OK) {
        slot->pending = true;
    } else {
        slot->failed_at = now;
    }
    return NULL;
}

static void card_set_img(lv_obj_t *img, const lv_image_dsc_t *dsc, bool hide_empty)
{
    if (img == NULL) {
        return;
    }
    if (lv_image_get_src(img) != (const void *)dsc) {
        lv_image_set_src(img, dsc);
    }
    if (hide_empty && dsc == NULL) {
        lv_obj_add_flag(img, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_clear_flag(img, LV_OBJ_FLAG_HIDDEN);
    }
}

/* ------------------------------------------------------------------ texts */

static void card_format_clock(char *out, size_t len, int64_t ts)
{
    time_t t = (time_t)ts;
    struct tm tm_ts;
    localtime_r(&t, &tm_ts);
    strftime(out, len, "%H:%M", &tm_ts);
}

static void card_format_duration(char *out, size_t len, int64_t secs)
{
    if (secs < 0) {
        secs = 0;
    }
    const int64_t mins = (secs + 59) / 60;
    if (mins >= 60) {
        snprintf(out, len, "%d:%02d h", (int)(mins / 60), (int)(mins % 60));
    } else {
        snprintf(out, len, "%d min", (int)mins);
    }
}

static void card_set_text(lv_obj_t *label, const char *text)
{
    if (label == NULL) {
        return;
    }
    const char *cur = lv_label_get_text(label);
    if (cur == NULL || strcmp(cur, text) != 0) {
        lv_label_set_text(label, text);
    }
    if (text[0] == '\0') {
        lv_obj_add_flag(label, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_clear_flag(label, LV_OBJ_FLAG_HIDDEN);
    }
}

static void card_set_bar(lv_obj_t *bar, float percent)
{
    if (bar == NULL) {
        return;
    }
    if (percent < 0.0f) {
        lv_obj_add_flag(bar, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    if (percent > 100.0f) {
        percent = 100.0f;
    }
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_HIDDEN);
    const int32_t v = (int32_t)lroundf(percent * 10.0f);
    if (lv_bar_get_value(bar) != v) {
        lv_bar_set_value(bar, v, LV_ANIM_OFF);
    }
}

static void card_render(const card_t *c, card_view_t *v, int64_t now)
{
    char big[CARD_TEXT_LEN + 32] = {0};
    char sub[2 * CARD_TEXT_LEN] = {0};
    float bar = -1.0f;

    if (c->kind == CARD_REMAIN) {
        int64_t remain = -1;
        if (c->end_ts > 0) {
            remain = c->end_ts - now;
        } else if (c->remain_s >= 0) {
            remain = c->remain_s;
        }
        if (remain >= 0) {
            card_format_duration(big, sizeof(big), remain);
            char at[8];
            card_format_clock(at, sizeof(at), c->end_ts > 0 ? c->end_ts : now + remain);
            snprintf(sub, sizeof(sub), ui_i18n_get("cards.done_at", "done at %s"), at);
            if (c->progress >= 0.0f) {
                const size_t used = strlen(sub);
                snprintf(sub + used, sizeof(sub) - used, " · %d %%", (int)lroundf(c->progress));
            }
        } else if (c->progress >= 0.0f) {
            snprintf(big, sizeof(big), "%d %%", (int)lroundf(c->progress));
        }
        bar = c->progress;
    } else if (c->kind == CARD_SPORT) {
        snprintf(big, sizeof(big), "%s", c->line);
        if (strcmp(c->sport_state, "IN") == 0) {
            snprintf(sub, sizeof(sub), "%s%s%s", ui_i18n_get("cards.live", "LIVE"), c->clock[0] ? " · " : "",
                c->clock);
        } else if (strcmp(c->sport_state, "POST") == 0) {
            snprintf(sub, sizeof(sub), "%s", ui_i18n_get("cards.final", "Final"));
        } else if (c->kickoff_ts > 0) {
            time_t t = (time_t)c->kickoff_ts;
            time_t tn = (time_t)now;
            struct tm tk;
            struct tm tt;
            localtime_r(&t, &tk);
            localtime_r(&tn, &tt);
            char hm[8];
            card_format_clock(hm, sizeof(hm), c->kickoff_ts);
            if (tk.tm_yday == tt.tm_yday && tk.tm_year == tt.tm_year) {
                snprintf(sub, sizeof(sub), "%s %s", ui_i18n_get("cards.today", "today"), hm);
            } else {
                snprintf(sub, sizeof(sub), "%s %s", ui_i18n_get("cards.tomorrow", "tomorrow"), hm);
            }
        }
        card_set_img(v->img[0], card_img_get(c->logo_home, LOGO_PX), false);
        card_set_img(v->img[1], card_img_get(c->logo_away, LOGO_PX), false);
    } else {
        snprintf(big, sizeof(big), "%s", c->title);
        if (c->playing) {
            snprintf(sub, sizeof(sub), "%s", c->sub);
        } else {
            snprintf(sub, sizeof(sub), "%s%s%s", ui_i18n_get("common.paused", "paused"), c->sub[0] ? " · " : "",
                c->sub);
        }
        if (c->duration > 0.0f) {
            float pos = c->position;
            if (c->playing && c->position_ts > 0 && now > c->position_ts) {
                pos += (float)(now - c->position_ts);
            }
            bar = pos * 100.0f / c->duration;
        }
        card_set_img(v->img[0], card_img_get(c->picture, COVER_PX), false);
        /* Only the commands this player takes. */
        const bool show[CTRL_COUNT] = {
            (c->features & MP_FEAT_PREVIOUS) != 0,
            (c->features & (c->playing ? MP_FEAT_PAUSE : MP_FEAT_PLAY)) != 0,
            (c->features & MP_FEAT_NEXT) != 0,
        };
        for (int k = 0; k < CTRL_COUNT; k++) {
            if (v->ctrl[k] == NULL) {
                continue;
            }
            if (show[k]) {
                lv_obj_clear_flag(v->ctrl[k], LV_OBJ_FLAG_HIDDEN);
            } else {
                lv_obj_add_flag(v->ctrl[k], LV_OBJ_FLAG_HIDDEN);
            }
        }
        const int want = c->playing ? 1 : 0;
        if (v->ctrl_icon[CTRL_PLAY] != NULL && v->play_icon != want) {
            v->play_icon = want;
            if (!ui_mdi_icon_apply(v->ctrl_icon[CTRL_PLAY], want ? "mdi:pause" : "mdi:play", CTRL_PLAY_PX)) {
                lv_label_set_text(v->ctrl_icon[CTRL_PLAY], want ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);
            }
        }
    }
    card_set_text(v->header, c->header);
    card_set_text(v->big, big);
    card_set_text(v->sub, sub);
    card_set_bar(v->bar, bar);
    if (v->sub != NULL) {
        const bool live = c->kind == CARD_SPORT && strcmp(c->sport_state, "IN") == 0;
        lv_obj_set_style_text_color(v->sub, lv_color_hex(live ? 0xFF5A52 : 0xD0D6DC), LV_PART_MAIN);
    }
}

/* ------------------------------------------------------------------ views */

static lv_obj_t *card_label(lv_obj_t *parent, const lv_font_t *font, uint32_t color)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_width(l, lv_pct(100));
    lv_obj_set_style_text_font(l, font, LV_PART_MAIN);
    lv_obj_set_style_text_color(l, lv_color_hex(color), LV_PART_MAIN);
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_DOTS);
    lv_label_set_text(l, "");
    return l;
}

static lv_obj_t *card_plain(lv_obj_t *parent)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    return o;
}

static lv_obj_t *card_image(lv_obj_t *parent, int32_t px, bool placeholder)
{
    lv_obj_t *img = lv_image_create(parent);
    lv_obj_set_size(img, px, px);
    lv_image_set_inner_align(img, LV_IMAGE_ALIGN_CONTAIN);
    lv_obj_clear_flag(img, LV_OBJ_FLAG_CLICKABLE);
    if (placeholder) {
        lv_obj_set_style_bg_color(img, lv_color_white(), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(img, LV_OPA_10, LV_PART_MAIN);
        lv_obj_set_style_radius(img, CARDS_BIG_SCREEN ? 12 : 8, LV_PART_MAIN);
        lv_obj_set_style_clip_corner(img, true, LV_PART_MAIN);
    }
    return img;
}

static lv_obj_t *card_bar(lv_obj_t *parent)
{
    lv_obj_t *bar = lv_bar_create(parent);
    lv_obj_set_size(bar, lv_pct(100), CARDS_BIG_SCREEN ? 8 : 6);
    lv_bar_set_range(bar, 0, 1000);
    lv_obj_set_style_margin_top(bar, CARDS_BIG_SCREEN ? 8 : 6, LV_PART_MAIN);
    lv_obj_set_style_bg_color(bar, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(bar, LV_OPA_20, LV_PART_MAIN);
    lv_obj_set_style_radius(bar, 4, LV_PART_MAIN);
    lv_obj_set_style_bg_color(bar, lv_color_hex(0x4FC3F7), LV_PART_INDICATOR);
    lv_obj_set_style_radius(bar, 4, LV_PART_INDICATOR);
    lv_obj_add_flag(bar, LV_OBJ_FLAG_HIDDEN);
    return bar;
}

static void card_ctrl_clicked(lv_event_t *e)
{
    card_view_t *v = lv_event_get_user_data(e);
    lv_obj_t *target = lv_event_get_current_target(e);
    const int idx = (int)(v - s_views);
    if (idx < 0 || idx >= s_card_count || s_cards[idx].kind != CARD_MEDIA) {
        return;
    }
    card_t *c = &s_cards[idx];
    const char *service = NULL;
    if (target == v->ctrl[CTRL_PREV]) {
        service = "media_previous_track";
    } else if (target == v->ctrl[CTRL_NEXT]) {
        service = "media_next_track";
    } else {
        service = c->playing ? "media_pause" : "media_play";
        /* Answer the finger at once; HA's state follows. */
        if (c->playing && c->position_ts > 0) {
            c->position += (float)((int64_t)time(NULL) - c->position_ts);
        }
        c->position_ts = (int64_t)time(NULL);
        c->playing = !c->playing;
    }
    char data[APP_MAX_ENTITY_ID_LEN + 24];
    snprintf(data, sizeof(data), "{\"entity_id\":\"%s\"}", c->key);
    const esp_err_t err = ha_client_call_service("media_player", service, data);
    ESP_LOGI(TAG, "%s %s -> %s", service, c->key, esp_err_to_name(err));
    card_render(c, v, (int64_t)time(NULL));
}

static lv_obj_t *card_ctrl(card_view_t *v, lv_obj_t *parent, card_ctrl_t which)
{
    static const char *const icons[CTRL_COUNT] = {"mdi:skip-previous", "mdi:play", "mdi:skip-next"};
    static const char *const fallback[CTRL_COUNT] = {LV_SYMBOL_PREV, LV_SYMBOL_PLAY, LV_SYMBOL_NEXT};
    const int32_t px = which == CTRL_PLAY ? CTRL_PLAY_PX : CTRL_ICON_PX;
    const int32_t size = px + (CARDS_BIG_SCREEN ? 20 : 14);
    lv_obj_t *btn = lv_obj_create(parent);
    lv_obj_remove_style_all(btn);
    lv_obj_set_size(btn, size, size);
    lv_obj_set_style_radius(btn, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(btn, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(btn, which == CTRL_PLAY ? LV_OPA_20 : LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(btn, LV_OPA_40, LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_ext_click_area(btn, CARDS_BIG_SCREEN ? 10 : 6);
    lv_obj_add_event_cb(btn, card_ctrl_clicked, LV_EVENT_CLICKED, v);

    lv_obj_t *icon = lv_label_create(btn);
    lv_obj_set_style_text_color(icon, lv_color_white(), LV_PART_MAIN);
    if (!ui_mdi_icon_apply(icon, icons[which], px)) {
        lv_obj_set_style_text_font(icon, APP_FONT_TEXT_24, LV_PART_MAIN);
        lv_label_set_text(icon, fallback[which]);
    }
    lv_obj_center(icon);
    v->ctrl[which] = btn;
    v->ctrl_icon[which] = icon;
    return btn;
}

static void card_build_box(card_view_t *v, int32_t width)
{
    v->box = lv_obj_create(s_row);
    lv_obj_remove_style_all(v->box);
    lv_obj_set_size(v->box, width, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(v->box, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(v->box, LV_OPA_60, LV_PART_MAIN);
    lv_obj_set_style_radius(v->box, CARDS_BIG_SCREEN ? 18 : 12, LV_PART_MAIN);
    lv_obj_set_style_border_color(v->box, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_border_opa(v->box, LV_OPA_20, LV_PART_MAIN);
    lv_obj_set_style_border_width(v->box, 1, LV_PART_MAIN);
    lv_obj_set_style_pad_all(v->box, CARD_PAD, LV_PART_MAIN);
    lv_obj_set_style_pad_row(v->box, CARDS_BIG_SCREEN ? 4 : 2, LV_PART_MAIN);
    lv_obj_set_style_pad_column(v->box, CARD_PAD, LV_PART_MAIN);
    lv_obj_clear_flag(v->box, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
}

static void cards_views_clear(void)
{
    if (s_row != NULL) {
        lv_obj_clean(s_row);
    }
    memset(s_views, 0, CARDS_MAX * sizeof(*s_views));
    s_view_count = 0;
}

static void cards_views_build(void)
{
    cards_views_clear();
    if (s_row == NULL) {
        return;
    }
    const lv_font_t *f_head = CARDS_BIG_SCREEN ? APP_FONT_TEXT_20 : APP_FONT_TEXT_14;
    const lv_font_t *f_big = CARDS_BIG_SCREEN ? APP_FONT_TEXT_34 : APP_FONT_TEXT_24;
    const lv_font_t *f_sub = CARDS_BIG_SCREEN ? APP_FONT_TEXT_22 : APP_FONT_TEXT_16;
    for (int i = 0; i < s_card_count; i++) {
        card_view_t *v = &s_views[i];
        v->kind = s_cards[i].kind;
        v->play_icon = -1;
        if (v->kind == CARD_MEDIA) {
            card_build_box(v, MEDIA_CARD_W);
            lv_obj_set_flex_flow(v->box, LV_FLEX_FLOW_ROW);
            lv_obj_set_flex_align(v->box, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
            v->img[0] = card_image(v->box, COVER_PX, true);
            lv_obj_t *col = card_plain(v->box);
            lv_obj_set_height(col, LV_SIZE_CONTENT);
            lv_obj_set_flex_grow(col, 1);
            lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
            lv_obj_set_style_pad_row(col, CARDS_BIG_SCREEN ? 4 : 2, LV_PART_MAIN);
            v->header = card_label(col, f_head, 0xA8B4C0);
            v->big = card_label(col, f_big, 0xFFFFFF);
            v->sub = card_label(col, f_sub, 0xD0D6DC);
            v->bar = card_bar(col);
            lv_obj_t *ctrls = card_plain(col);
            lv_obj_set_size(ctrls, lv_pct(100), LV_SIZE_CONTENT);
            lv_obj_set_style_margin_top(ctrls, CARDS_BIG_SCREEN ? 8 : 4, LV_PART_MAIN);
            lv_obj_set_flex_flow(ctrls, LV_FLEX_FLOW_ROW);
            lv_obj_set_flex_align(ctrls, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
            lv_obj_set_style_pad_column(ctrls, CARDS_BIG_SCREEN ? 28 : 18, LV_PART_MAIN);
            for (int k = 0; k < CTRL_COUNT; k++) {
                card_ctrl(v, ctrls, (card_ctrl_t)k);
            }
        } else if (v->kind == CARD_SPORT) {
            card_build_box(v, CARD_W);
            lv_obj_set_flex_flow(v->box, LV_FLEX_FLOW_COLUMN);
            v->header = card_label(v->box, f_head, 0xA8B4C0);
            lv_obj_t *row = card_plain(v->box);
            lv_obj_set_size(row, lv_pct(100), LV_SIZE_CONTENT);
            lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
            lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
            lv_obj_set_style_pad_column(row, CARDS_BIG_SCREEN ? 8 : 4, LV_PART_MAIN);
            lv_obj_set_style_pad_ver(row, CARDS_BIG_SCREEN ? 4 : 2, LV_PART_MAIN);
            v->img[0] = card_image(row, LOGO_PX, false);
            /* One line between the logos: the flex share sets the width. */
            v->big = card_label(row, CARDS_BIG_SCREEN ? APP_FONT_TEXT_28 : APP_FONT_TEXT_22, 0xFFFFFF);
            lv_obj_set_width(v->big, 1);
            lv_obj_set_flex_grow(v->big, 1);
            lv_obj_set_style_text_align(v->big, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
            v->img[1] = card_image(row, LOGO_PX, false);
            v->sub = card_label(v->box, f_sub, 0xD0D6DC);
            lv_obj_set_style_text_align(v->sub, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
        } else {
            card_build_box(v, CARD_W);
            lv_obj_set_flex_flow(v->box, LV_FLEX_FLOW_COLUMN);
            v->header = card_label(v->box, f_head, 0xA8B4C0);
            v->big = card_label(v->box, f_big, 0xFFFFFF);
            v->sub = card_label(v->box, f_sub, 0xD0D6DC);
            v->bar = card_bar(v->box);
        }
    }
    s_view_count = s_card_count;
}

static void cards_place(int32_t clock_top, int32_t clock_bottom)
{
    if (s_row == NULL) {
        return;
    }
    if (s_view_count == 0) {
        lv_obj_add_flag(s_row, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_obj_clear_flag(s_row, LV_OBJ_FLAG_HIDDEN);
    /* The small cards as tall as the tallest one. */
    for (int i = 0; i < s_view_count; i++) {
        lv_obj_set_height(s_views[i].box, LV_SIZE_CONTENT);
    }
    lv_obj_update_layout(s_row);
    int32_t max_h = 0;
    for (int i = 0; i < s_view_count; i++) {
        if (s_views[i].kind != CARD_MEDIA) {
            max_h = LV_MAX(max_h, lv_obj_get_height(s_views[i].box));
        }
    }
    for (int i = 0; i < s_view_count; i++) {
        if (s_views[i].kind != CARD_MEDIA) {
            lv_obj_set_height(s_views[i].box, max_h);
        }
    }
    lv_obj_update_layout(s_row);
    const int32_t h = lv_obj_get_height(s_row);
    const int32_t space_above = clock_top;
    const int32_t space_below = APP_SCREEN_HEIGHT - clock_bottom;
    int32_t y;
    if (space_above >= space_below) {
        /* centred in the band above the clock, but not glued to it */
        y = LV_MAX(CARDS_MARGIN, (clock_top - h) / 2);
    } else {
        y = clock_bottom + LV_MAX(CARDS_MARGIN, (space_below - h) / 2);
        if (y + h > APP_SCREEN_HEIGHT - CARDS_MARGIN) {
            y = APP_SCREEN_HEIGHT - CARDS_MARGIN - h;
        }
    }
    lv_obj_set_y(s_row, y);
}

/* ------------------------------------------------------------------ API */

void ui_saver_cards_attach(lv_obj_t *overlay)
{
    if (!cards_alloc()) {
        return; /* no cards without their tables */
    }
    s_row = lv_obj_create(overlay);
    lv_obj_remove_style_all(s_row);
    lv_obj_set_size(s_row, APP_SCREEN_WIDTH - 2 * CARDS_MARGIN, LV_SIZE_CONTENT);
    lv_obj_set_x(s_row, CARDS_MARGIN);
    lv_obj_set_flex_flow(s_row, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(s_row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_column(s_row, CARD_GAP, LV_PART_MAIN);
    lv_obj_set_style_pad_row(s_row, CARD_GAP, LV_PART_MAIN);
    lv_obj_clear_flag(s_row, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(s_row, LV_OBJ_FLAG_HIDDEN);
    memset(s_views, 0, CARDS_MAX * sizeof(*s_views));
    s_view_count = 0;
    s_seen_seq = UINT32_MAX; /* rebuild on the next tick */
    s_last_top = -1;
    s_last_bottom = -1;
}

void ui_saver_cards_detach(void)
{
    s_row = NULL;
    if (s_views != NULL) {
        memset(s_views, 0, CARDS_MAX * sizeof(*s_views));
    }
    s_view_count = 0;
    s_seen_seq = UINT32_MAX;
}

bool ui_saver_cards_hit(int32_t x, int32_t y)
{
    if (s_row == NULL || lv_obj_has_flag(s_row, LV_OBJ_FLAG_HIDDEN)) {
        return false;
    }
    for (int i = 0; i < s_view_count; i++) {
        if (s_views[i].kind != CARD_MEDIA || s_views[i].box == NULL) {
            continue;
        }
        /* A player without controls is just a picture: let the touch wake. */
        bool any_ctrl = false;
        for (int k = 0; k < CTRL_COUNT; k++) {
            any_ctrl = any_ctrl ||
                       (s_views[i].ctrl[k] != NULL && !lv_obj_has_flag(s_views[i].ctrl[k], LV_OBJ_FLAG_HIDDEN));
        }
        if (!any_ctrl) {
            continue;
        }
        lv_area_t a;
        lv_obj_get_coords(s_views[i].box, &a);
        if (x >= a.x1 && x <= a.x2 && y >= a.y1 && y <= a.y2) {
            return true;
        }
    }
    return false;
}

static bool cards_same_set(const card_t *old_cards, int old_count)
{
    if (old_count != s_card_count) {
        return false;
    }
    for (int i = 0; i < s_card_count; i++) {
        if (old_cards[i].kind != s_cards[i].kind || strcmp(old_cards[i].key, s_cards[i].key) != 0) {
            return false;
        }
    }
    return true;
}

void ui_saver_cards_tick(bool visible, int32_t clock_top, int32_t clock_bottom)
{
    if (s_cards == NULL) {
        return;
    }
    if (s_row == NULL) {
        return;
    }
    if (!visible) {
        s_was_visible = false;
        return;
    }
    const int64_t now = (int64_t)time(NULL);
    if (now < 1700000000) {
        return; /* clock not set yet */
    }
    /* Re-evaluate on new data, on every reveal and once a minute (time
     * windows, finished programs). */
    const uint32_t seq = ha_cards_seq();
    const bool refresh = seq != s_seen_seq || !s_was_visible || (now % 60) == 0;
    s_was_visible = true;
    bool relayout = clock_top != s_last_top || clock_bottom != s_last_bottom || s_img_arrived;
    s_img_arrived = false;
    if (refresh) {
        card_t *old_cards = s_old_cards;
        const int old_count = s_card_count;
        memcpy(old_cards, s_cards, CARDS_MAX * sizeof(*s_cards));
        s_seen_seq = seq;
        cards_rebuild_data(now);
        if (!cards_same_set(old_cards, old_count) || s_view_count != s_card_count) {
            cards_views_build();
            relayout = true;
        }
    }
    for (int i = 0; i < IMG_SLOTS; i++) {
        s_imgs[i].used = false;
    }
    for (int i = 0; i < s_view_count && i < s_card_count; i++) {
        card_render(&s_cards[i], &s_views[i], now);
    }
    if (relayout || refresh) {
        s_last_top = clock_top;
        s_last_bottom = clock_bottom;
        cards_place(clock_top, clock_bottom);
    }
}
