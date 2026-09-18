/* SPDX-License-Identifier: LicenseRef-FNCL-1.1
 * Copyright (c) 2026 Cpt_Kirk
 *
 * On-panel internet radio.
 *
 * The radio page can either hand a stream URL to a Home Assistant
 * media_player or play it on the panel itself. This module is the second
 * option: it streams an internet radio URL over HTTP, decodes it and writes
 * the PCM to the built-in ES8311 speaker, so the panel is a radio on its own.
 *
 * Two tasks keep a stream alive. The network task owns the HTTP connection,
 * detects the codec (MP3 or AAC/HE-AAC, with or without ADTS headers) and
 * fills a PSRAM jitter buffer with mono PCM; the playback task drains that
 * buffer into the speaker and writes silence when it runs dry. A network
 * hiccup or a slow screen repaint therefore costs buffering time instead of
 * an audible gap, and the speaker itself paces the stream at real time.
 *
 * Redirects are followed by hand: esp_http_client_open() sends a single
 * request and returns, the 3xx chasing lives in esp_http_client_perform(),
 * and half of the built-in stations answer 302 to their public URL.
 *
 * The speaker is borrowed from the voice path through xz_audio_acquire_output()
 * / xz_audio_release_output(), and the engine refuses to start while the
 * microphone is open (a voice session owns the I2S clock at that point). The
 * speaker runs mono, so the decoders' stereo output is averaged down before it
 * reaches the jitter buffer.
 *
 * The API is thread safe and non-blocking: play/stop post a command and the
 * caller polls panel_radio_get_state() (the radio page does that from an LVGL
 * timer).
 */
#include "panel_radio.h"

#include <stdio.h>
#include <string.h>

#include "esp_aac_dec.h"
#include "esp_audio_dec.h"
#include "esp_codec_dev.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_mp3_dec.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "xiaozhi_audio.h"

#if defined(CONFIG_MBEDTLS_CERTIFICATE_BUNDLE)
#include "esp_crt_bundle.h"
#endif

#define PR_TAG "PANEL_RADIO"

/* The decoder and the HTTP client allocate their working memory on the heap;
 * the task stack only holds the call frames - but the TLS handshake and the
 * reconnect path below it are deep enough to need more than the 7 KB that was
 * enough for a plain socket. */
#define PR_TASK_STACK 12288
#define PR_TASK_PRIORITY 5
#define PR_TASK_CORE 1

/* The playback task sits above LVGL (2), httpd (3) and the UI (4) so a repaint
 * cannot starve the speaker, and below the HA task (8) so cloud traffic still
 * gets out in time. */
#define PR_PLAY_STACK 4096
#define PR_PLAY_PRIORITY 6
#define PR_PLAY_CORE 1

/* Big enough for a full second of a 256 kbit/s stream; the PCM buffer holds a
 * whole MP3 frame (1152 samples, 2 ch, 16 bit) or a 2048 sample AAC frame with
 * room to spare. */
#define PR_IN_BUF_SIZE (16 * 1024)
#define PR_PCM_BUF_SIZE (8 * 1024)
#define PR_PCM_BUF_MAX (64 * 1024)
/* An ADTS header codes its frame length in 13 bits, so no audio frame can be
 * longer than 8191 bytes and anything bigger is a header that misread. */
#define PR_MAX_FRAME_BYTES 8192
/* The decoders work on stereo frames, the codec runs mono, so the downmix
 * destination never needs more than half of the largest PCM buffer. */
#define PR_MONO_BUF_SIZE (PR_PCM_BUF_MAX / 2)

/* Jitter buffer: ~1.5 s of 44.1 kHz mono PCM per 128 KB block, so a station
 * that stutters every few seconds is covered by the fill level. */
#define PR_RING_BYTES (256 * 1024)
#define PR_RING_LOW_WATER (4 * 1024)
/* Playback starts once half a second is buffered, and 20 ms blocks keep the
 * I2S DMA fed without holding the lock for long. */
#define PR_PREBUFFER_BYTES (48 * 1024)
#define PR_BLOCK_BYTES 1920

/* Connect (and DNS) timeout is generous, the streaming read timeout is short
 * because it is also the worst-case stop latency. */
#define PR_CONNECT_TIMEOUT_MS 5000
#define PR_READ_TIMEOUT_MS 500
#define PR_RETRY_DELAY_MS 2000
#define PR_MAX_CONNECTS 4
/* Silence in the socket while the jitter buffer is empty: the station is gone
 * and a fresh connection is cheaper than waiting. */
#define PR_STREAM_IDLE_MS 4000
/* Data keeps arriving but no frame decodes for this long: the codec changed on
 * the wire or the stream is corrupt, so a reconnect is more useful than
 * waiting. */
#define PR_AUDIO_STALL_MS 12000
/* Bytes skipped while hunting for a frame header before the stream is declared
 * unplayable. */
#define PR_RESYNC_MAX (32 * 1024)
/* Bytes needed before the codec can be named with confidence. */
#define PR_SNIFF_BYTES 1024

/* Redirect chasing and URL handling. */
#define PR_MAX_REDIRECTS 5
/* Radio directories and Icecast hosts reject small or unknown agents with 403. */
#define PR_USER_AGENT "VLC/3.0.20 LibVLC/3.0.20"

/* Starting rate; the real one is taken from the first decoded frame. */
#define PR_DEFAULT_RATE 44100
#define PR_MAX_RATE 48000

enum { PR_CMD_NONE = 0, PR_CMD_PLAY, PR_CMD_STOP };

typedef enum {
    PR_FMT_AUTO = 0, /* nothing conclusive in the first bytes */
    PR_FMT_MP3,
    PR_FMT_AAC,
    PR_FMT_HLS,   /* m3u8 playlist: not a stream we can decode */
    PR_FMT_OGG,   /* Ogg/Vorbis or Ogg/Opus */
    PR_FMT_OTHER, /* FLAC, or something that is not audio at all */
} pr_fmt_t;

static SemaphoreHandle_t s_lock;
static SemaphoreHandle_t s_wake;
static TaskHandle_t s_task;
static TaskHandle_t s_play_task;
static bool s_task_started;

/* Written by any task and read by the stream task without the lock, so a stop
 * is seen even while the task sits in a read. */
static volatile bool s_stop;
static volatile uint32_t s_generation;

/* Guarded by s_lock. */
static int s_cmd;
static panel_radio_state_t s_state;
static char s_url[PANEL_RADIO_URL_MAX];
static char s_title[PANEL_RADIO_TITLE_MAX];
static char s_error[PANEL_RADIO_TEXT_MAX];
static uint32_t s_rate;

/* TEMPORARY diagnostics for the /api/radio debug route: the log ring is turned
 * over several times a second by the decoder's rejection flood, so the counters
 * have to be readable without the console. Written by the stream task only. */
static panel_radio_stats_t s_stats;

/* Jitter buffer: one producer (network task), one consumer (playback task).
 * Both counters only grow, so their difference is the fill level and 32 bit
 * wraparound is correct by construction. Both tasks run on PR_TASK_CORE. */
static uint8_t *s_ring;
static volatile uint32_t s_ring_wr;
static volatile uint32_t s_ring_rd;

/* Playback task state. */
static SemaphoreHandle_t s_play_go;
static SemaphoreHandle_t s_play_idle;
static volatile bool s_play_run;
static volatile bool s_play_failed;
static volatile uint32_t s_play_generation;
static volatile uint32_t s_pending_rate;
/* True once the playback rate came from the frame header instead of a guess. */
static volatile bool s_rate_exact;
static uint32_t s_play_underruns;

static void pr_task(void *arg);

typedef struct {
    uint8_t *in;      /* raw bytes from the socket */
    size_t in_size;   /* valid bytes in `in` */
    uint8_t *pcm;     /* decoder output, still stereo */
    size_t pcm_size;
    int16_t *mono;    /* downmix destination */
    uint32_t generation;
    uint32_t samples; /* mono frames handed to the jitter buffer */
    uint32_t resynced;
    unsigned channels;
    unsigned hdr_rate;  /* rate named by the frame header, 0 when unknown */
    uint32_t best_per_ch; /* most samples per channel seen in one decoded frame */
    uint32_t rate_pub;  /* playback rate already handed to the speaker */
    uint32_t frames;    /* decoded frames of the current connection */
    uint32_t decerr;    /* frames the decoder rejected */
    uint32_t calls;     /* decode calls of the current connection */
    uint32_t ok;        /* decode calls that produced PCM */
    uint32_t err_log;   /* rejected frames already written to the log */
    uint32_t pcm_bytes; /* PCM the decoder produced */
    uint32_t bytes_in;  /* stream bytes read from the socket */
    uint32_t stat_samples;
    uint32_t stat_decerr;
    uint32_t stat_calls;
    uint32_t stat_ok;
    uint32_t stat_pcm;
    uint32_t stat_bytes;
    uint32_t stat_resync;
    int64_t stat_us;
    pr_fmt_t fmt;
    bool rate_known;
    bool rate_capped;
} pr_stream_t;

static void pr_lock(void)
{
    if (s_lock != NULL) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
    }
}

static void pr_unlock(void)
{
    if (s_lock != NULL) {
        xSemaphoreGive(s_lock);
    }
}

/* Publish a state that belongs to `generation` only: an older stream must not
 * overwrite the state of a newer command. */
static void pr_publish(uint32_t generation, panel_radio_state_t state, const char *error)
{
    pr_lock();
    if (s_generation == generation) {
        s_state = state;
        if (state == PANEL_RADIO_ERROR && error != NULL) {
            snprintf(s_error, sizeof(s_error), "%s", error);
            ESP_LOGW(PR_TAG, "error: %s", error);
        } else if (state != PANEL_RADIO_ERROR) {
            s_error[0] = '\0';
        }
        if (state == PANEL_RADIO_IDLE) {
            s_rate = 0;
        }
    }
    pr_unlock();
}

static bool pr_should_exit(uint32_t generation)
{
    if (s_stop) {
        return true;
    }
    return s_generation != generation;
}

/* ------------------------------------------------------------ http helper */

/* Copy the origin (scheme://host[:port]) of `base` in front of an absolute
 * path. Radio redirects use either a full URL or "/path". */
static bool pr_resolve_url(const char *base, const char *loc, char *out, size_t out_len)
{
    if (strstr(loc, "://") != NULL) {
        const size_t n = strlen(loc);
        if (out_len == 0 || n + 1 > out_len) {
            return false;
        }
        memcpy(out, loc, n + 1);
        return true;
    }
    if (loc[0] != '/') {
        return false;
    }
    const char *scheme = strstr(base, "://");
    if (scheme == NULL) {
        return false;
    }
    const char *path = strchr(scheme + 3, '/');
    const size_t origin = path != NULL ? (size_t)(path - base) : strlen(base);
    const size_t tail = strlen(loc);
    if (out_len == 0 || origin + tail + 1 > out_len) {
        return false;
    }
    memcpy(out, base, origin);
    memcpy(out + origin, loc, tail + 1);
    return true;
}

/* Case-insensitive header name compare. */
static bool pr_ieq(const char *a, const char *b)
{
    while (*a != '\0' && *b != '\0') {
        char ca = *a;
        char cb = *b;
        if (ca >= 'A' && ca <= 'Z') {
            ca = (char)(ca - 'A' + 'a');
        }
        if (cb >= 'A' && cb <= 'Z') {
            cb = (char)(cb - 'A' + 'a');
        }
        if (ca != cb) {
            return false;
        }
        a++;
        b++;
    }
    return *a == *b;
}

/* Response headers are only reachable through the event handler: the public
 * esp_http_client_get_header() reads the request headers. */
typedef struct {
    char location[PANEL_RADIO_URL_MAX];
    char content_type[64];
} pr_hop_t;

static esp_err_t pr_http_event(esp_http_client_event_t *evt)
{
    pr_hop_t *hop = (pr_hop_t *)evt->user_data;
    if (hop != NULL && evt->event_id == HTTP_EVENT_ON_HEADER && evt->header_key != NULL &&
        evt->header_value != NULL) {
        if (pr_ieq(evt->header_key, "Location")) {
            snprintf(hop->location, sizeof(hop->location), "%s", evt->header_value);
        } else if (pr_ieq(evt->header_key, "Content-Type")) {
            snprintf(hop->content_type, sizeof(hop->content_type), "%s", evt->header_value);
        }
    }
    return ESP_OK;
}

/* Open a stream URL, chasing redirects by hand. Fills `content_type` (may stay
 * empty) and, on failure, a message for the user. */
static esp_err_t pr_http_open(const char *url, esp_http_client_handle_t *out, char *content_type, size_t ct_len,
    char *msg, size_t msg_len)
{
    char current[PANEL_RADIO_URL_MAX];
    snprintf(current, sizeof(current), "%s", url);
    content_type[0] = '\0';
    msg[0] = '\0';

    for (int hop = 0; hop <= PR_MAX_REDIRECTS; hop++) {
        pr_hop_t headers;
        memset(&headers, 0, sizeof(headers));

        esp_http_client_config_t cfg = {
            .url = current,
            .method = HTTP_METHOD_GET,
            .timeout_ms = PR_CONNECT_TIMEOUT_MS,
            .buffer_size = 2048,
            .user_agent = PR_USER_AGENT,
            .keep_alive_enable = false,
            .event_handler = pr_http_event,
            .user_data = &headers,
#if defined(CONFIG_MBEDTLS_CERTIFICATE_BUNDLE)
            .crt_bundle_attach = esp_crt_bundle_attach,
#endif
        };

        esp_http_client_handle_t client = esp_http_client_init(&cfg);
        if (client == NULL) {
            ESP_LOGW(PR_TAG, "cannot init the HTTP client");
            snprintf(msg, msg_len, "Błąd klienta HTTP");
            return ESP_FAIL;
        }
        /* Interleaved ICY metadata would inject bytes the decoder cannot skip. */
        esp_http_client_set_header(client, "Icy-MetaData", "0");
        esp_http_client_set_header(client, "Accept", "*/*");

        const esp_err_t err = esp_http_client_open(client, 0);
        if (err != ESP_OK) {
            ESP_LOGW(PR_TAG, "connect to %s failed: %s", current, esp_err_to_name(err));
            snprintf(msg, msg_len, "Nie można połączyć ze stacją");
            esp_http_client_cleanup(client);
            return err;
        }
        (void)esp_http_client_fetch_headers(client);

        const int status = esp_http_client_get_status_code(client);
        if (status == 301 || status == 302 || status == 303 || status == 307 || status == 308) {
            if (headers.location[0] == '\0') {
                ESP_LOGW(PR_TAG, "HTTP %d without a usable Location header", status);
                snprintf(msg, msg_len, "Błędne przekierowanie stacji");
                esp_http_client_cleanup(client);
                return ESP_ERR_INVALID_RESPONSE;
            }
            char next[PANEL_RADIO_URL_MAX];
            if (!pr_resolve_url(current, headers.location, next, sizeof(next))) {
                ESP_LOGW(PR_TAG, "HTTP %d redirect to an unusable address: %s", status, headers.location);
                snprintf(msg, msg_len, "Błędne przekierowanie stacji");
                esp_http_client_cleanup(client);
                return ESP_ERR_INVALID_RESPONSE;
            }
            ESP_LOGI(PR_TAG, "HTTP %d -> %s", status, next);
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            snprintf(current, sizeof(current), "%s", next);
            continue;
        }
        if (status >= 400) {
            ESP_LOGW(PR_TAG, "stream answered HTTP %d", status);
            snprintf(msg, msg_len, "Stacja odrzuca połączenie (HTTP %d)", status);
            esp_http_client_cleanup(client);
            return ESP_ERR_NOT_FOUND;
        }

        if (headers.content_type[0] != '\0') {
            snprintf(content_type, ct_len, "%s", headers.content_type);
        }

        /* From here on the connection is a stream: keep the read timeout short
         * so a stop request takes effect within roughly half a second. */
        esp_http_client_set_timeout_ms(client, PR_READ_TIMEOUT_MS);
        ESP_LOGI(PR_TAG, "streaming %s (%s, HTTP %d)", current, content_type[0] != '\0' ? content_type : "?", status);
        *out = client;
        return ESP_OK;
    }

    ESP_LOGW(PR_TAG, "more than %d redirects for %s", PR_MAX_REDIRECTS, url);
    snprintf(msg, msg_len, "Zbyt wiele przekierowań stacji");
    return ESP_ERR_INVALID_RESPONSE;
}

/* --------------------------------------------------------------- buffers */

/* PSRAM keeps the internal heap free for LVGL; boards without PSRAM (the S3
 * variant) fall back to internal RAM. */
static void *pr_alloc(size_t size)
{
    void *p = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (p == NULL) {
        p = heap_caps_malloc(size, MALLOC_CAP_8BIT);
    }
    return p;
}

static bool pr_alloc_buffers(pr_stream_t *st)
{
    memset(st, 0, sizeof(*st));
    st->in = pr_alloc(PR_IN_BUF_SIZE);
    st->pcm = pr_alloc(PR_PCM_BUF_SIZE);
    st->mono = pr_alloc(PR_MONO_BUF_SIZE);
    if (st->in == NULL || st->pcm == NULL || st->mono == NULL) {
        heap_caps_free(st->in);
        heap_caps_free(st->pcm);
        heap_caps_free(st->mono);
        memset(st, 0, sizeof(*st));
        return false;
    }
    st->pcm_size = PR_PCM_BUF_SIZE;
    st->channels = 2;
    return true;
}

static void pr_free_buffers(pr_stream_t *st)
{
    heap_caps_free(st->in);
    heap_caps_free(st->pcm);
    heap_caps_free(st->mono);
    memset(st, 0, sizeof(*st));
}

static bool pr_grow_pcm(pr_stream_t *st, uint32_t needed)
{
    if (needed <= st->pcm_size) {
        return true;
    }
    if (needed > PR_PCM_BUF_MAX) {
        return false;
    }
    size_t next = st->pcm_size;
    while (next < needed) {
        next *= 2;
    }
    uint8_t *bigger = pr_alloc(next);
    if (bigger == NULL) {
        return false;
    }
    heap_caps_free(st->pcm);
    st->pcm = bigger;
    st->pcm_size = next;
    ESP_LOGI(PR_TAG, "PCM buffer grown to %u bytes", (unsigned)next);
    return true;
}

/* ---------------------------------------------------------- jitter buffer */

static uint32_t pr_ring_used(void)
{
    return s_ring_wr - s_ring_rd;
}

static uint32_t pr_ring_free(void)
{
    return PR_RING_BYTES - pr_ring_used();
}

/* Producer side. Blocks (in small steps, so a stop stays responsive) while the
 * buffer is full. Returns false when the stream must not continue. */
static bool pr_ring_push(const uint8_t *src, size_t len, uint32_t generation)
{
    while (len > 0) {
        if (pr_should_exit(generation) || s_play_failed) {
            return false;
        }
        const uint32_t room = pr_ring_free();
        if (room == 0) {
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }
        const uint32_t chunk = room < len ? room : (uint32_t)len;
        const uint32_t wr = s_ring_wr & (PR_RING_BYTES - 1);
        const uint32_t first = (PR_RING_BYTES - wr) < chunk ? (PR_RING_BYTES - wr) : chunk;
        memcpy(s_ring + wr, src, first);
        if (chunk > first) {
            memcpy(s_ring, src + first, chunk - first);
        }
        s_ring_wr += chunk;
        src += chunk;
        len -= chunk;
    }
    return true;
}

/* Consumer side: returns how many bytes were taken (0 when empty). */
static size_t pr_ring_pop(uint8_t *dst, size_t len)
{
    const uint32_t used = pr_ring_used();
    if (used == 0) {
        return 0;
    }
    const uint32_t chunk = used < len ? used : (uint32_t)len;
    const uint32_t rd = s_ring_rd & (PR_RING_BYTES - 1);
    const uint32_t first = (PR_RING_BYTES - rd) < chunk ? (PR_RING_BYTES - rd) : chunk;
    memcpy(dst, s_ring + rd, first);
    if (chunk > first) {
        memcpy(dst + first, s_ring, chunk - first);
    }
    s_ring_rd += chunk;
    return chunk;
}

/* --------------------------------------------------------- codec detection */

static pr_fmt_t pr_fmt_from_content_type(const char *ct)
{
    if (ct == NULL || ct[0] == '\0') {
        return PR_FMT_AUTO;
    }
    if (strstr(ct, "mpegurl") != NULL) {
        return PR_FMT_HLS;
    }
    if (strstr(ct, "aac") != NULL) {
        return PR_FMT_AAC;
    }
    if (strstr(ct, "mpeg") != NULL || strstr(ct, "mp3") != NULL) {
        return PR_FMT_MP3;
    }
    if (strstr(ct, "ogg") != NULL || strstr(ct, "opus") != NULL) {
        return PR_FMT_OGG;
    }
    return PR_FMT_AUTO;
}

/* Sample rates an ADTS header can name, indexed by its sample-rate index. */
static const uint32_t PR_AAC_RATES[13] = {
    96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050, 16000, 12000, 11025, 8000, 7350,
};

/* MP3 rates, indexed by version (MPEG 1, MPEG 2, MPEG 2.5) and rate index. */
static const uint32_t PR_MP3_RATES[3][3] = {
    {44100, 48000, 32000},
    {22050, 24000, 16000},
    {11025, 12000, 8000},
};

/* ADTS header: 12 sync bits, then 1 version bit, 2 layer bits (always 00 for
 * AAC), 1 protection bit, 2 profile bits, 4 sample-rate-index bits. The header
 * carries its own frame length, which is what tells a real AAC frame from a
 * coincidental 0xFF in the payload. */
static bool pr_aac_header(const uint8_t *b, size_t len, size_t i)
{
    if (i + 7 > len || b[i] != 0xFF || (b[i + 1] & 0xF6) != 0xF0) {
        return false;
    }
    if (((b[i + 2] >> 2) & 0x0F) > 12) {
        return false; /* reserved/forbidden sample-rate index */
    }
    const size_t frame_len = ((size_t)(b[i + 3] & 0x03) << 11) | ((size_t)b[i + 4] << 3) | (b[i + 5] >> 5);
    return frame_len >= 7;
}

/* Name the codec from the first bytes of the stream: live stations have no
 * container header, so the first frame header decides. MP3 and AAC both start
 * with 0xFFE/0xFFF; the layer field tells them apart (MP3: layer I-III, AAC:
 * layer 0), and each candidate has to survive its own header validation.
 * `out_rate` receives the rate the frame header names, which is the only number
 * that cannot disagree with the audio itself. */
static pr_fmt_t pr_sniff(const uint8_t *b, size_t len, const char *content_type, unsigned *out_rate)
{
    if (out_rate != NULL) {
        *out_rate = 0;
    }
    if (len >= 7 && memcmp(b, "#EXTM3U", 7) == 0) {
        return PR_FMT_HLS;
    }
    if (len >= 4 && memcmp(b, "OggS", 4) == 0) {
        return PR_FMT_OGG;
    }
    if (len >= 4 && memcmp(b, "fLaC", 4) == 0) {
        return PR_FMT_OTHER;
    }

    size_t off = 0;
    if (len >= 10 && memcmp(b, "ID3", 3) == 0) {
        const size_t tag = ((size_t)(b[6] & 0x7F) << 21) | ((size_t)(b[7] & 0x7F) << 14) |
            ((size_t)(b[8] & 0x7F) << 7) | (size_t)(b[9] & 0x7F);
        if (10 + tag > len) {
            return PR_FMT_AUTO; /* the tag is still arriving */
        }
        off = 10 + tag;
    }

    for (size_t i = off; i + 2 < len; i++) {
        if (b[i] != 0xFF || (b[i + 1] & 0xE0) != 0xE0) {
            continue;
        }
        if ((b[i + 1] & 0x06) == 0) { /* layer 0: AAC */
            if (!pr_aac_header(b, len, i)) {
                continue;
            }
            const size_t frame_len = ((size_t)(b[i + 3] & 0x03) << 11) | ((size_t)b[i + 4] << 3) | (b[i + 5] >> 5);
            if (i + frame_len + 7 <= len && !pr_aac_header(b, len, i + frame_len)) {
                continue; /* the next frame is not where this header says it is */
            }
            if (out_rate != NULL) {
                *out_rate = PR_AAC_RATES[(b[i + 2] >> 2) & 0x0F];
            }
            return PR_FMT_AAC;
        }
        const unsigned bitrate = (b[i + 2] >> 4) & 0x0F;
        const unsigned rate = (b[i + 2] >> 2) & 0x03;
        if (bitrate == 0x0F || rate == 0x03) {
            continue; /* reserved values: not a frame header */
        }
        if (out_rate != NULL) {
            const unsigned version = (b[i + 1] >> 3) & 0x03;
            *out_rate = PR_MP3_RATES[version == 3 ? 0 : (version == 2 ? 1 : 2)][rate];
        }
        return PR_FMT_MP3;
    }
    return pr_fmt_from_content_type(content_type);
}

static const char *pr_fmt_name(pr_fmt_t fmt)
{
    switch (fmt) {
        case PR_FMT_MP3:
            return "MP3";
        case PR_FMT_AAC:
            return "AAC";
        case PR_FMT_HLS:
            return "HLS/m3u8";
        case PR_FMT_OGG:
            return "Ogg";
        case PR_FMT_OTHER:
            return "inny";
        default:
            return "nieznany";
    }
}

/* The decoders are registered once; both stay available for later streams. */
static esp_audio_dec_handle_t pr_dec_open(pr_fmt_t fmt)
{
    static bool mp3_registered;
    static bool aac_registered;
    esp_audio_dec_cfg_t cfg = {
        .type = ESP_AUDIO_TYPE_MP3,
        .cfg = NULL,
        .cfg_sz = 0,
    };
    /* ADTS carries rate/channels, so these are only fallbacks; HE-AAC (the
     * audio/aacp stations) needs the SBR part enabled. */
    esp_aac_dec_cfg_t aac_cfg = {
        .sample_rate = PR_DEFAULT_RATE,
        .channel = ESP_AUDIO_DUAL,
        .bits_per_sample = ESP_AUDIO_BIT16,
        .no_adts_header = false,
        .aac_plus_enable = true,
    };

    if (fmt == PR_FMT_AAC) {
        if (!aac_registered) {
            if (esp_aac_dec_register() != ESP_AUDIO_ERR_OK) {
                ESP_LOGE(PR_TAG, "cannot register the AAC decoder");
                return NULL;
            }
            aac_registered = true;
        }
        cfg.type = ESP_AUDIO_TYPE_AAC;
        cfg.cfg = &aac_cfg;
        cfg.cfg_sz = sizeof(aac_cfg);
    } else {
        if (!mp3_registered) {
            if (esp_mp3_dec_register() != ESP_AUDIO_ERR_OK) {
                ESP_LOGE(PR_TAG, "cannot register the MP3 decoder");
                return NULL;
            }
            mp3_registered = true;
        }
    }

    esp_audio_dec_handle_t dec = NULL;
    if (esp_audio_dec_open(&cfg, &dec) != ESP_AUDIO_ERR_OK || dec == NULL) {
        ESP_LOGE(PR_TAG, "cannot open the %s decoder", pr_fmt_name(fmt));
        return NULL;
    }
    ESP_LOGI(PR_TAG, "%s decoder open", pr_fmt_name(fmt));
    return dec;
}

/* ------------------------------------------------------------ decode step */

/* The speaker is opened mono: average stereo frames so nothing plays at double
 * speed. `dst` holds at least PR_MONO_BUF_SIZE bytes. */
static size_t pr_to_mono(const int16_t *src, size_t frames, unsigned channels, int16_t *dst)
{
    if (channels < 2) {
        memcpy(dst, src, frames * sizeof(int16_t));
    } else {
        for (size_t i = 0; i < frames; i++) {
            dst[i] = (int16_t)(((int32_t)src[i * channels] + (int32_t)src[i * channels + 1]) / 2);
        }
    }
    return frames * sizeof(int16_t);
}

/* Hand a playback rate over to the playback task, which owns the speaker. The
 * handshake goes through the mutex because the task clears the slot per block. */
static void pr_publish_rate(uint32_t rate, bool exact)
{
    pr_lock();
    s_rate_exact = exact;
    s_pending_rate = rate;
    pr_unlock();
}

/* The rate that matters is the one the decoder produces PCM at, and that is
 * derived from the frame header together with the sample count of the decoded
 * frame: an AAC frame carries 1024 samples of its core rate, and SBR (HE-AAC)
 * adds as many again at twice that rate. The decoder's own report is only the
 * fallback - for HE-AAC stations it named 44100 Hz while the PCM was the
 * 22050 Hz core, which plays the stream at double speed. The sample count is
 * watched over the first frames because SBR needs a moment to lock on. */
static void pr_note_rate(pr_stream_t *st, esp_audio_dec_handle_t dec, size_t decoded_size)
{
    esp_audio_dec_info_t info = {0};
    const bool has_info = esp_audio_dec_get_info(dec, &info) == ESP_AUDIO_ERR_OK;
    if (has_info && info.channel >= 1) {
        st->channels = info.channel;
    }
    if (st->frames == 0) {
        ESP_LOGI(PR_TAG, "stream: %u Hz / %u bit / %u ch / %u bit/s (header %u Hz)",
            (unsigned)(has_info ? info.sample_rate : 0), (unsigned)(has_info ? info.bits_per_sample : 0),
            (unsigned)st->channels, (unsigned)(has_info ? info.bitrate : 0), (unsigned)st->hdr_rate);
        if (has_info && info.bits_per_sample != 0 && info.bits_per_sample != 16) {
            ESP_LOGW(PR_TAG, "%u bit samples are treated as 16 bit", (unsigned)info.bits_per_sample);
        }
    }
    st->frames += 1;

    if (st->hdr_rate == 0) {
        /* No frame header to trust: the decoder's word is all there is. */
        if (!st->rate_known && st->frames == 1 && has_info && info.sample_rate != 0) {
            st->rate_known = true;
            st->rate_pub = info.sample_rate;
            pr_publish_rate(info.sample_rate, false);
        }
        return;
    }

    const unsigned channels = st->channels != 0 ? st->channels : 2;
    const uint32_t per_ch = (uint32_t)(decoded_size / ((size_t)channels * sizeof(int16_t)));
    if (per_ch <= st->best_per_ch) {
        return; /* the frames say nothing new about the rate */
    }
    st->best_per_ch = per_ch;

    uint32_t rate = st->hdr_rate;
    if (st->fmt == PR_FMT_AAC && per_ch >= 1536) {
        rate *= 2; /* SBR: 2048 samples per frame at twice the core rate */
    }
    if (rate > PR_MAX_RATE) {
        if (!st->rate_capped) {
            st->rate_capped = true;
            ESP_LOGW(PR_TAG, "the stream needs %u Hz, above the %u Hz speaker limit",
                (unsigned)rate, PR_MAX_RATE);
        }
        rate = PR_MAX_RATE;
    }
    if (st->rate_known && rate == st->rate_pub) {
        return;
    }
    st->rate_known = true;
    st->rate_pub = rate;
    pr_publish_rate(rate, true);
    ESP_LOGI(PR_TAG, "playing at %u Hz (header %u Hz, decoder %u Hz, %u samples per frame)",
        (unsigned)rate, (unsigned)st->hdr_rate, (unsigned)(has_info ? info.sample_rate : 0), (unsigned)per_ch);
}

/* Live AAC and MP3 streams are chains of self-describing frames: an ADTS
 * header carries its own frame length, and an MPEG audio header yields one from
 * its bitrate. Splitting the frames here is what keeps the decoder honest - the
 * only input the AAC decoder can decode is exactly one whole frame, and handing
 * it a partial one makes it fail. */
typedef struct {
    size_t len;    /* total frame length in bytes */
    uint32_t rate; /* the rate the header names */
} pr_frame_t;

/* kbit/s per bitrate index: MPEG1 layers I, II, III, then MPEG2 or MPEG2.5
 * layer I, and MPEG2 or MPEG2.5 layers II and III. */
static const uint16_t PR_MP3_KBPS[5][16] = {
    {0, 32, 64, 96, 128, 160, 192, 224, 256, 288, 320, 352, 384, 416, 448, 0},
    {0, 32, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 384, 0},
    {0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 0},
    {0, 32, 48, 56, 64, 80, 96, 112, 128, 144, 160, 176, 192, 224, 256, 0},
    {0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160, 0},
};

/* Length and rate of the frame that starts at `b`, or false when `b` does not
 * begin with a header the sniffed format can produce. */
static bool pr_frame_at(pr_fmt_t fmt, const uint8_t *b, size_t len, pr_frame_t *out)
{
    out->len = 0;
    out->rate = 0;

    if (fmt == PR_FMT_AAC) {
        if (!pr_aac_header(b, len, 0)) {
            return false;
        }
        out->len = ((size_t)(b[3] & 0x03) << 11) | ((size_t)b[4] << 3) | (b[5] >> 5);
        out->rate = PR_AAC_RATES[(b[2] >> 2) & 0x0F];
        return true;
    }

    if (len < 4 || b[0] != 0xFF || (b[1] & 0xE0) != 0xE0) {
        return false;
    }
    const uint32_t version = (uint32_t)((b[1] >> 3) & 0x03); /* 3: MPEG1, 2: MPEG2, 0: MPEG2.5 */
    const uint32_t layer = (uint32_t)((b[1] >> 1) & 0x03);   /* 3: I, 2: II, 1: III */
    const uint32_t kbps_idx = (uint32_t)(b[2] >> 4);
    const uint32_t rate_idx = (uint32_t)((b[2] >> 2) & 0x03);
    if (version == 1 || layer == 0 || kbps_idx == 0 || kbps_idx == 15 || rate_idx == 3) {
        return false;
    }
    const uint32_t kbps = PR_MP3_KBPS[version == 3 ? 3 - layer : (layer == 3 ? 3 : 4)][kbps_idx];
    const uint32_t rate = PR_MP3_RATES[version == 3 ? 0 : (version == 2 ? 1 : 2)][rate_idx];
    /* 384 samples per frame for layer I, 1152 for layer II and MPEG1 layer III,
     * 576 for MPEG2/2.5 layer III. A frame is that many samples split over the
     * channels and bytes the bitrate and rate describe, plus the padding bit. */
    const uint32_t samples = layer == 3 ? 384 : (layer == 2 || version == 3 ? 1152 : 576);
    const size_t frame = (size_t)(samples / 8 * kbps * 1000U / rate) + (size_t)((b[2] >> 1) & 0x01);
    if (frame < 4 || frame > PR_MAX_FRAME_BYTES) {
        return false;
    }
    out->len = frame;
    out->rate = rate;
    return true;
}

/* Decode as much of the input buffer as possible and push mono PCM into the
 * jitter buffer. Returns false when the stream must be abandoned. */
static bool pr_pump(pr_stream_t *st, esp_audio_dec_handle_t dec)
{
    uint8_t *p = st->in;
    size_t left = st->in_size;
    esp_audio_dec_out_frame_t out = {
        .buffer = st->pcm,
        .len = (uint32_t)st->pcm_size,
    };
    int guard = 0;

    while (left > 0 && guard++ < 512) {
        if (pr_should_exit(st->generation) || s_play_failed) {
            return false;
        }

        pr_frame_t frame = {.len = 0, .rate = 0};
        const bool framed = st->fmt == PR_FMT_AAC || st->fmt == PR_FMT_MP3;
        if (!framed) {
            /* A container this decoder cannot split (Ogg and friends): hand the
             * buffer over as one chunk, exactly as it always was. */
            frame.len = left;
        } else if (!pr_frame_at(st->fmt, p, left, &frame)) {
            /* Nothing frame-like at the buffer start: hunt for the next header
             * inside what is already here. It is a byte scan, not a decoder
             * call, which is what used to wreck the frame grid. */
            size_t skip = 1;
            while (skip + 4 <= left && !pr_frame_at(st->fmt, p + skip, left - skip, &frame)) {
                skip++;
            }
            if (frame.len == 0) {
                /* Every byte of the buffer is junk: throw it away and let the
                 * next read start over. */
                st->resynced += (uint32_t)left;
                p += left;
                left = 0;
                break;
            }
            st->resynced += (uint32_t)skip;
            p += skip;
            left -= skip;
            if (left < frame.len) {
                /* The header is there but its frame is still incomplete. */
                break;
            }
        }
        if (left < frame.len) {
            /* Only part of the frame has arrived; the rest is still on the
             * wire. Holding it back is the point of this loop. */
            break;
        }
        if (st->hdr_rate == 0 && frame.rate != 0) {
            st->hdr_rate = frame.rate;
        }

        for (int grow = 0; ; grow++) {
            esp_audio_dec_in_raw_t raw = {
                .buffer = p,
                .len = (uint32_t)frame.len,
                .frame_recover = ESP_AUDIO_DEC_RECOVERY_NONE,
            };
            out.len = (uint32_t)st->pcm_size;
            out.decoded_size = 0;
            const esp_audio_err_t err = esp_audio_dec_process(dec, &raw, &out);
            st->calls += 1;
            if (err != ESP_AUDIO_ERR_OK) {
                st->err_log += 1;
            }
            if (st->calls <= 24 || (err != ESP_AUDIO_ERR_OK && st->err_log <= 24)) {
                /* Framing evidence: every call has to see one whole frame, and
                 * a healthy stream never reports an error here. */
                ESP_LOGI(PR_TAG, "dec %u: frame=%u len=%u err=%d consumed=%u decoded=%u head=%02X%02X",
                    (unsigned)st->calls, (unsigned)frame.len, (unsigned)raw.len, (int)err,
                    (unsigned)raw.consumed, (unsigned)(err == ESP_AUDIO_ERR_OK ? out.decoded_size : 0),
                    p[0], frame.len > 1 ? p[1] : 0);
            }

            if (err == ESP_AUDIO_ERR_BUFF_NOT_ENOUGH) {
                /* A report per frame: grow the PCM buffer and decode the same
                 * frame again, nothing of it was consumed. */
                if (grow < 2 && out.needed_size > st->pcm_size && pr_grow_pcm(st, out.needed_size)) {
                    out.buffer = st->pcm;
                    continue;
                }
                st->decerr += 1;
                break;
            }
            if (err != ESP_AUDIO_ERR_OK) {
                /* A frame the decoder refuses: drop exactly that frame and go
                 * on at the next header. Advancing one byte from here loses the
                 * frame grid for good, which is what used to happen. */
                st->decerr += 1;
                break;
            }

            if (out.decoded_size != 0) {
                st->ok += 1;
                st->pcm_bytes += out.decoded_size;
                pr_note_rate(st, dec, out.decoded_size);
                const size_t frame_bytes = sizeof(int16_t) * st->channels;
                const size_t frames = out.decoded_size / frame_bytes;
                if (frames > PR_MONO_BUF_SIZE / (size_t)sizeof(int16_t)) {
                    /* A frame that cannot fit the mono scratch: drop it. */
                    st->resynced += 1;
                } else {
                    const size_t mono_bytes = pr_to_mono((const int16_t *)out.buffer, frames, st->channels, st->mono);
                    st->samples += (uint32_t)frames;
                    if (mono_bytes > 0 && !pr_ring_push((const uint8_t *)st->mono, mono_bytes, st->generation)) {
                        return false;
                    }
                }
            }
            break;
        }

        p += frame.len;
        left -= frame.len;
    }

    /* Keep the leftover of a partial frame for the next round. */
    if (left > 0 && p != st->in) {
        memmove(st->in, p, left);
    }
    st->in_size = left;

    /* The rate the PCM arrives at has to match the rate the speaker drains at,
     * otherwise the jitter buffer drifts empty or full and the audio runs away
     * from real time. This line is the measurement of exactly that. */
    const int64_t now_us = esp_timer_get_time();
    if (st->stat_us == 0) {
        st->stat_us = now_us;
    } else if (now_us - st->stat_us >= 10000000) {
        const uint32_t elapsed_s = (uint32_t)((now_us - st->stat_us) / 1000000);
        const uint32_t ok = st->ok - st->stat_ok;
        const uint32_t rate = s_rate != 0 ? s_rate : PR_DEFAULT_RATE;
        const uint32_t ring_kb = (uint32_t)(pr_ring_used() / 1024);
        ESP_LOGI(PR_TAG,
            "%u samples/s, %u ok of %u calls/s (err %u/s, resync %u/s), %u B/frame, in %u B/s, "
            "playing at %u Hz, buffer %u KB",
            (unsigned)((st->samples - st->stat_samples) / elapsed_s), (unsigned)(ok / elapsed_s),
            (unsigned)((st->calls - st->stat_calls) / elapsed_s),
            (unsigned)((st->decerr - st->stat_decerr) / elapsed_s),
            (unsigned)((st->resynced - st->stat_resync) / elapsed_s),
            (unsigned)(ok != 0 ? (st->pcm_bytes - st->stat_pcm) / ok : 0),
            (unsigned)((st->bytes_in - st->stat_bytes) / elapsed_s),
            (unsigned)rate, (unsigned)ring_kb);
        s_stats.samples = st->samples;
        s_stats.calls = st->calls;
        s_stats.ok = st->ok;
        s_stats.decerr = st->decerr;
        s_stats.resync = st->resynced;
        s_stats.bytes_in = st->bytes_in;
        s_stats.window_s = elapsed_s;
        s_stats.samples_s = (st->samples - st->stat_samples) / elapsed_s;
        s_stats.ok_s = ok / elapsed_s;
        s_stats.calls_s = (st->calls - st->stat_calls) / elapsed_s;
        s_stats.err_s = (st->decerr - st->stat_decerr) / elapsed_s;
        s_stats.resync_s = (st->resynced - st->stat_resync) / elapsed_s;
        s_stats.pcm_per_ok = ok != 0 ? (st->pcm_bytes - st->stat_pcm) / ok : 0;
        s_stats.in_bps = (st->bytes_in - st->stat_bytes) / elapsed_s;
        s_stats.ring_kb = ring_kb;
        s_stats.rate = rate;
        st->stat_us = now_us;
        st->stat_samples = st->samples;
        st->stat_decerr = st->decerr;
        st->stat_calls = st->calls;
        st->stat_ok = st->ok;
        st->stat_pcm = st->pcm_bytes;
        st->stat_bytes = st->bytes_in;
        st->stat_resync = st->resynced;
    }
    return true;
}

/* --------------------------------------------------------- playback task */

/* Drain the jitter buffer into the speaker, writing silence while it is empty
 * so a network stall never becomes an audible click. */
static void pr_play_loop(uint8_t *block)
{
    esp_codec_dev_handle_t speaker = xz_audio_get_speaker();
    const uint32_t generation = s_play_generation;
    bool started = false;
    bool starved = false;
    int64_t full_since_us = 0;
    uint32_t underruns = 0;
    uint64_t played_bytes = 0;
    int64_t last_log_us = esp_timer_get_time();

    ESP_LOGI(PR_TAG, "playback starting (prebuffer %u KB)", (unsigned)(PR_PREBUFFER_BYTES / 1024));
    while (s_play_run && speaker != NULL) {
        uint32_t rate;
        pr_lock();
        rate = s_pending_rate;
        s_pending_rate = 0;
        pr_unlock();
        if (rate != 0) {
            if (rate != xz_audio_get_output_rate()) {
                if (xz_audio_set_output_rate(rate) == ESP_OK) {
                    pr_lock();
                    if (s_generation == generation) {
                        s_rate = rate;
                    }
                    pr_unlock();
                } else {
                    /* Playing at the wrong pitch beats not playing at all. */
                    ESP_LOGW(PR_TAG, "speaker refused %u Hz", (unsigned)rate);
                }
            }
        }

        size_t len = 0;
        if (started || pr_ring_used() >= PR_PREBUFFER_BYTES) {
            if (!started) {
                started = true;
                ESP_LOGI(PR_TAG, "buffer filled (%u KB), playing", (unsigned)(pr_ring_used() / 1024));
                pr_publish(generation, PANEL_RADIO_PLAYING, NULL);
            }
            len = pr_ring_pop(block, PR_BLOCK_BYTES);
        }

        if (len == 0) {
            memset(block, 0, PR_BLOCK_BYTES);
            len = PR_BLOCK_BYTES;
            if (started && !starved) {
                starved = true;
                underruns++;
                pr_publish(generation, PANEL_RADIO_BUFFERING, NULL);
                ESP_LOGW(PR_TAG, "buffer ran dry, filling silence (underrun %u)", (unsigned)underruns);
            }
        } else {
            played_bytes += len;
            if (starved) {
                starved = false;
                pr_publish(generation, PANEL_RADIO_PLAYING, NULL);
            }
            if (len < PR_BLOCK_BYTES) {
                memset(block + len, 0, PR_BLOCK_BYTES - len);
                len = PR_BLOCK_BYTES;
            }
        }

        if (esp_codec_dev_write(speaker, block, (int)len) < 0) {
            ESP_LOGE(PR_TAG, "write to the speaker failed");
            s_play_failed = true;
            break;
        }

        const int64_t now = esp_timer_get_time();

        /* The decoder reports the rate it decodes at, and for HE-AAC that is
         * occasionally the core rate instead of the SBR output rate. Playing
         * at half speed then looks exactly like a stream that delivers twice
         * as fast as we drain it: the buffer stays full. A rate taken from the
         * frame header needs no such correction, it only needs watching. */
        const uint32_t rate_now = s_rate != 0 ? s_rate : PR_DEFAULT_RATE;
        const bool ring_full = started && pr_ring_used() >= (PR_RING_BYTES * 4) / 5;
        if (!ring_full) {
            full_since_us = 0;
        } else if (full_since_us == 0) {
            full_since_us = now;
        } else if (now - full_since_us >= 20 * 1000 * 1000) {
            full_since_us = 0;
            if (!s_rate_exact && rate_now * 2 <= PR_MAX_RATE) {
                ESP_LOGW(PR_TAG, "buffer stays full at %u Hz, playing faster: %u Hz", (unsigned)rate_now,
                    (unsigned)(rate_now * 2));
                pr_lock();
                s_pending_rate = rate_now * 2;
                pr_unlock();
            } else {
                ESP_LOGW(PR_TAG, "buffer stays full at %u Hz, the stream delivers audio faster than real time",
                    (unsigned)rate_now);
            }
        }

        if (now - last_log_us >= 10 * 1000 * 1000) {
            last_log_us = now;
            ESP_LOGI(PR_TAG, "playing %llu s in total, buffer %u KB, underruns %u",
                (unsigned long long)(played_bytes / (2ULL * (s_rate != 0 ? s_rate : PR_DEFAULT_RATE))),
                (unsigned)(pr_ring_used() / 1024), (unsigned)underruns);
        }
    }
    s_play_underruns = underruns;
    ESP_LOGI(PR_TAG, "playback stopped (underruns %u)", (unsigned)underruns);
}

static void pr_play_task(void *arg)
{
    (void)arg;
    uint8_t *block = pr_alloc(PR_BLOCK_BYTES);
    if (block == NULL) {
        ESP_LOGE(PR_TAG, "no memory for the playback block");
        s_play_failed = true;
    }

    for (;;) {
        if (xSemaphoreTake(s_play_go, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        if (block != NULL) {
            pr_play_loop(block);
        }
        xSemaphoreGive(s_play_idle);
    }
}

/* ------------------------------------------------------------ stream task */

static void pr_run_stream(const char *url, const char *title, uint32_t generation)
{
    pr_stream_t st;
    if (!pr_alloc_buffers(&st)) {
        ESP_LOGE(PR_TAG, "no PSRAM for the stream buffers");
        pr_publish(generation, PANEL_RADIO_ERROR, "Brak pamięci na bufor strumienia");
        return;
    }
    st.generation = generation;

    const esp_err_t out_err = xz_audio_acquire_output(PR_DEFAULT_RATE);
    if (out_err != ESP_OK) {
        ESP_LOGW(PR_TAG, "speaker is not available (%s)", esp_err_to_name(out_err));
        pr_free_buffers(&st);
        pr_publish(generation, PANEL_RADIO_ERROR,
            out_err == ESP_ERR_INVALID_STATE ? "Głośnik zajęty" : "Brak dostępu do głośnika");
        return;
    }
    pr_lock();
    if (s_generation == generation) {
        s_rate = PR_DEFAULT_RATE;
    }
    s_rate_exact = false;
    pr_unlock();

    ESP_LOGI(PR_TAG, "playing '%s' (%s)", title != NULL && title[0] != '\0' ? title : "?", url);
    pr_publish(generation, PANEL_RADIO_BUFFERING, NULL);

    /* Hand the jitter buffer over to a fresh playback run. */
    s_ring_rd = 0;
    s_ring_wr = 0;
    s_pending_rate = 0;
    s_play_failed = false;
    s_play_generation = generation;
    s_play_run = true;
    xSemaphoreGive(s_play_go);

    esp_audio_dec_handle_t dec = NULL;
    pr_fmt_t forced_fmt = PR_FMT_AUTO; /* set after one failed codec guess */
    pr_fmt_t used_fmt = PR_FMT_AUTO;   /* codec the open decoder was created for */
    bool tried_other_fmt = false;
    bool played_ever = false;
    bool terminal = false; /* a terminal error keeps its message on screen */
    int fail_streak = 0;

    while (!pr_should_exit(generation)) {
        char content_type[64];
        char msg[PANEL_RADIO_TEXT_MAX];
        esp_http_client_handle_t http = NULL;

        if (pr_http_open(url, &http, content_type, sizeof(content_type), msg, sizeof(msg)) != ESP_OK) {
            if (!played_ever && ++fail_streak >= PR_MAX_CONNECTS) {
                pr_publish(generation, PANEL_RADIO_ERROR, msg[0] != '\0' ? msg : "Nie można połączyć ze stacją");
                break;
            }
            ESP_LOGW(PR_TAG, "%s, retry %d/%d", msg[0] != '\0' ? msg : "connect failed", fail_streak, PR_MAX_CONNECTS);
            vTaskDelay(pdMS_TO_TICKS(PR_RETRY_DELAY_MS));
            continue;
        }
        st.in_size = 0; /* a new response starts on a frame boundary */
        st.resynced = 0;
        st.rate_known = false;
        st.rate_capped = false;
        st.hdr_rate = 0;
        st.best_per_ch = 0;
        st.rate_pub = 0;
        st.frames = 0;
        if (dec != NULL) {
            /* A reconnect may be a different stream altogether. */
            esp_audio_dec_reset(dec);
        }

        int64_t last_data_us = esp_timer_get_time();
        int64_t last_audio_us = last_data_us;

        while (!pr_should_exit(generation) && !s_play_failed) {
            /* The jitter buffer is the flow control: stop reading while it is
             * full, the speaker drains it at real time. */
            if (pr_ring_free() < PR_RING_LOW_WATER) {
                vTaskDelay(pdMS_TO_TICKS(5));
                continue;
            }

            if (st.in_size < PR_IN_BUF_SIZE) {
                const int read =
                    esp_http_client_read(http, (char *)st.in + st.in_size, (int)(PR_IN_BUF_SIZE - st.in_size));
                if (read == -ESP_ERR_HTTP_EAGAIN) {
                    /* Read timeout, not the end of the stream: the ring (or the
                     * station) decides whether this connection is still worth
                     * keeping. */
                    if (pr_ring_used() == 0 &&
                        esp_timer_get_time() - last_data_us >= (int64_t)PR_STREAM_IDLE_MS * 1000) {
                        ESP_LOGW(PR_TAG, "no data for %d ms and nothing buffered, reconnecting", PR_STREAM_IDLE_MS);
                        break;
                    }
                    continue;
                }
                if (read <= 0) {
                    ESP_LOGI(PR_TAG, "stream ended (read=%d)", read);
                    break;
                }
                st.in_size += (size_t)read;
                st.bytes_in += (uint32_t)read;
                last_data_us = esp_timer_get_time();
            }

            if (dec == NULL) {
                if (st.in_size < PR_SNIFF_BYTES) {
                    continue; /* read a little more before naming the codec */
                }
                unsigned sniff_rate = 0;
                const pr_fmt_t fmt = forced_fmt != PR_FMT_AUTO
                    ? forced_fmt
                    : pr_sniff(st.in, st.in_size, content_type, &sniff_rate);
                st.fmt = fmt;
                st.hdr_rate = sniff_rate;
                if (fmt == PR_FMT_AUTO) {
                    pr_publish(generation, PANEL_RADIO_ERROR, "Nieobsługiwany format strumienia");
                    terminal = true;
                    break;
                }
                if (fmt == PR_FMT_HLS || fmt == PR_FMT_OGG || fmt == PR_FMT_OTHER) {
                    char err[PANEL_RADIO_TEXT_MAX];
                    snprintf(err, sizeof(err), "Nieobsługiwany format (%s)", pr_fmt_name(fmt));
                    pr_publish(generation, PANEL_RADIO_ERROR, err);
                    terminal = true;
                    break;
                }
                ESP_LOGI(PR_TAG, "codec: %s (%s)", pr_fmt_name(fmt),
                    content_type[0] != '\0' ? content_type : "brak Content-Type");
                dec = pr_dec_open(fmt);
                if (dec == NULL) {
                    char err[PANEL_RADIO_TEXT_MAX];
                    snprintf(err, sizeof(err), "Brak dekodera %s", pr_fmt_name(fmt));
                    pr_publish(generation, PANEL_RADIO_ERROR, err);
                    terminal = true;
                    break;
                }
                used_fmt = fmt;
            }

            const uint32_t samples_before = st.samples;
            if (!pr_pump(&st, dec)) {
                break;
            }
            if (st.samples != samples_before) {
                last_audio_us = esp_timer_get_time();
            } else if (esp_timer_get_time() - last_audio_us >= (int64_t)PR_AUDIO_STALL_MS * 1000) {
                ESP_LOGW(PR_TAG, "no decoded audio for %d ms while data keeps arriving, reconnecting",
                    PR_AUDIO_STALL_MS);
                break;
            }

            if (st.in_size >= PR_IN_BUF_SIZE) {
                /* The decoder cannot advance on a full buffer: drop it so the
                 * stream does not wedge behind a broken frame. */
                ESP_LOGW(PR_TAG, "decoder stalled, dropping %u bytes", (unsigned)st.in_size);
                st.in_size = 0;
            }
            if (!played_ever && st.resynced > PR_RESYNC_MAX) {
                ESP_LOGW(PR_TAG, "no usable frame in %u bytes", (unsigned)st.resynced);
                if (!tried_other_fmt) {
                    /* The sniff or the Content-Type may have been wrong: give
                     * the other codec one chance before giving up. */
                    tried_other_fmt = true;
                    forced_fmt = used_fmt == PR_FMT_AAC ? PR_FMT_MP3 : PR_FMT_AAC;
                    ESP_LOGW(PR_TAG, "retrying as %s", pr_fmt_name(forced_fmt));
                }
                break;
            }
        }

        esp_http_client_close(http);
        esp_http_client_cleanup(http);

        if (pr_should_exit(generation)) {
            break;
        }
        if (s_play_failed) {
            pr_publish(generation, PANEL_RADIO_ERROR, "Błąd zapisu do głośnika");
            terminal = true;
            break;
        }
        if (terminal) {
            break;
        }

        if (st.samples == 0 && !played_ever && dec != NULL && !tried_other_fmt) {
            /* Data arrived and the decoder accepted it, yet no audio came out:
             * the codec guess was wrong. Give the other decoder one chance. */
            tried_other_fmt = true;
            forced_fmt = used_fmt == PR_FMT_AAC ? PR_FMT_MP3 : PR_FMT_AAC;
            esp_audio_dec_close(dec);
            dec = NULL;
            used_fmt = PR_FMT_AUTO;
            fail_streak = 0;
            ESP_LOGW(PR_TAG, "no audio decoded, retrying as %s", pr_fmt_name(forced_fmt));
            continue;
        }

        if (st.samples > 0) {
            played_ever = true;
            fail_streak = 0;
            ESP_LOGI(PR_TAG, "connection delivered %u samples (%u s)", (unsigned)st.samples,
                (unsigned)(st.samples / (s_rate != 0 ? s_rate : PR_DEFAULT_RATE)));
        } else if (!played_ever && ++fail_streak >= PR_MAX_CONNECTS) {
            pr_publish(generation, PANEL_RADIO_ERROR,
                st.resynced > PR_RESYNC_MAX ? "Nieobsługiwany format strumienia" : "Stacja nie wysyła dźwięku");
            terminal = true;
            break;
        } else {
            ESP_LOGW(PR_TAG, "no audio from the stream, retry %d/%d", fail_streak, PR_MAX_CONNECTS);
        }

        if (!played_ever) {
            pr_publish(generation, PANEL_RADIO_BUFFERING, NULL);
        }
        vTaskDelay(pdMS_TO_TICKS(PR_RETRY_DELAY_MS));
    }

    /* Let the playback task finish the current block, then give the speaker
     * back to the voice path. */
    s_play_run = false;
    if (s_play_idle != NULL) {
        xSemaphoreTake(s_play_idle, pdMS_TO_TICKS(1000));
    }
    ESP_LOGI(PR_TAG, "stopped (total %u samples, underruns %u)", (unsigned)st.samples, (unsigned)s_play_underruns);
    if (dec != NULL) {
        esp_audio_dec_close(dec);
    }
    xz_audio_release_output();
    pr_free_buffers(&st);
    if (!terminal) {
        /* A stop (or a spent retry budget) returns the page to idle; a terminal
         * error keeps its message on screen. */
        pr_publish(generation, PANEL_RADIO_IDLE, NULL);
    }
}

static void pr_task(void *arg)
{
    (void)arg;

    for (;;) {
        if (xSemaphoreTake(s_wake, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        char url[PANEL_RADIO_URL_MAX];
        char title[PANEL_RADIO_TITLE_MAX];
        int cmd;
        uint32_t generation;

        pr_lock();
        cmd = s_cmd;
        s_cmd = PR_CMD_NONE;
        generation = s_generation;
        snprintf(url, sizeof(url), "%s", s_url);
        snprintf(title, sizeof(title), "%s", s_title);
        pr_unlock();

        if (cmd != PR_CMD_PLAY || url[0] == '\0') {
            /* A stop that arrived while the engine was idle; the state was
             * already published by panel_radio_stop(). */
            continue;
        }
        pr_run_stream(url, title, generation);
    }
}

static esp_err_t pr_task_start(void)
{
    if (s_task_started) {
        return ESP_OK;
    }

    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutex();
        s_wake = xSemaphoreCreateBinary();
        s_play_go = xSemaphoreCreateBinary();
        s_play_idle = xSemaphoreCreateBinary();
        if (s_lock == NULL || s_wake == NULL || s_play_go == NULL || s_play_idle == NULL) {
            ESP_LOGE(PR_TAG, "cannot create the engine primitives");
            return ESP_ERR_NO_MEM;
        }
    }

    if (s_ring == NULL) {
        s_ring = pr_alloc(PR_RING_BYTES);
        if (s_ring == NULL) {
            ESP_LOGE(PR_TAG, "no memory for the %u KB jitter buffer", (unsigned)(PR_RING_BYTES / 1024));
            return ESP_ERR_NO_MEM;
        }
    }

    if (s_play_task == NULL &&
        xTaskCreatePinnedToCore(pr_play_task, "panel_radio_out", PR_PLAY_STACK, NULL, PR_PLAY_PRIORITY, &s_play_task,
            PR_PLAY_CORE) != pdPASS) {
        ESP_LOGE(PR_TAG, "cannot create the playback task");
        return ESP_FAIL;
    }

    /* The AAC/MP3 decoders log one ERROR line for every input they cannot turn
     * into a frame, and at 115200 baud that console write alone costs the
     * decode task more time than the decoding itself. The frames are now fed
     * one at a time, so the flood should be gone; keeping the tags at WARN is
     * the safety net for a stream that still misbehaves, and every rejection is
     * counted and printed by the statistic line below either way. */
    esp_log_level_set("ESP_AAC_DEC", ESP_LOG_WARN);
    esp_log_level_set("ESP_MP3_DEC", ESP_LOG_WARN);

    if (xTaskCreatePinnedToCore(pr_task, "panel_radio", PR_TASK_STACK, NULL, PR_TASK_PRIORITY, &s_task,
            PR_TASK_CORE) != pdPASS) {
        ESP_LOGE(PR_TAG, "cannot create the stream task");
        return ESP_FAIL;
    }
    s_task_started = true;
    ESP_LOGI(PR_TAG, "radio engine started (buffer %u KB)", (unsigned)(PR_RING_BYTES / 1024));
    return ESP_OK;
}

/* ------------------------------------------------------------------- API */

esp_err_t panel_radio_play(const char *url, const char *title)
{
    if (url == NULL || url[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    const esp_err_t err = pr_task_start();
    if (err != ESP_OK) {
        return err;
    }

    pr_lock();
    snprintf(s_url, sizeof(s_url), "%s", url);
    snprintf(s_title, sizeof(s_title), "%s", title != NULL ? title : "");
    s_stop = false;
    s_error[0] = '\0';
    s_state = PANEL_RADIO_BUFFERING;
    s_cmd = PR_CMD_PLAY;
    s_generation++;
    pr_unlock();

    xSemaphoreGive(s_wake);
    ESP_LOGI(PR_TAG, "play requested: %s", url);
    return ESP_OK;
}

void panel_radio_stop(void)
{
    if (s_lock == NULL) {
        return;
    }

    pr_lock();
    const bool was_active = s_state != PANEL_RADIO_IDLE;
    s_stop = true;
    s_cmd = PR_CMD_STOP;
    s_generation++;
    s_state = PANEL_RADIO_IDLE;
    s_error[0] = '\0';
    pr_unlock();

    /* Also wake the task when it is idle so a pending stop does not fire on
     * the next play. */
    xSemaphoreGive(s_wake);
    if (was_active) {
        ESP_LOGI(PR_TAG, "stop requested");
    }
}

bool panel_radio_is_active(void)
{
    const panel_radio_state_t state = panel_radio_get_state();
    return state == PANEL_RADIO_BUFFERING || state == PANEL_RADIO_PLAYING;
}

panel_radio_state_t panel_radio_get_state(void)
{
    if (s_lock == NULL) {
        return PANEL_RADIO_IDLE;
    }
    pr_lock();
    const panel_radio_state_t state = s_state;
    pr_unlock();
    return state;
}

void panel_radio_get_title(char *out, size_t out_len)
{
    if (out == NULL || out_len == 0) {
        return;
    }
    out[0] = '\0';
    if (s_lock == NULL) {
        return;
    }
    pr_lock();
    snprintf(out, out_len, "%s", s_title);
    pr_unlock();
}

void panel_radio_get_error(char *out, size_t out_len)
{
    if (out == NULL || out_len == 0) {
        return;
    }
    out[0] = '\0';
    if (s_lock == NULL) {
        return;
    }
    pr_lock();
    snprintf(out, out_len, "%s", s_error);
    pr_unlock();
}

uint32_t panel_radio_get_rate(void)
{
    if (s_lock == NULL) {
        return 0;
    }
    pr_lock();
    const uint32_t rate = s_rate;
    pr_unlock();
    return rate;
}

/* TEMPORARY (see panel_radio.h): the counters of the decode loop, so the audio
 * path can be measured while the decoder's log flood is silenced. */
void panel_radio_get_stats(panel_radio_stats_t *out)
{
    if (out == NULL) {
        return;
    }
    if (s_lock == NULL) {
        memset(out, 0, sizeof(*out));
        return;
    }
    pr_lock();
    *out = s_stats;
    pr_unlock();
}
