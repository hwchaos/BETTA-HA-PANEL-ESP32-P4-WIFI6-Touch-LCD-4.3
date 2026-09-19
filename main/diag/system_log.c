/* SPDX-License-Identifier: LicenseRef-FNCL-1.1
 * Copyright (c) 2026 Cpt_Kirk
 *
 * Persistent, bounded system log for the BETTA panel.
 *
 * Layers:
 *  1. An esp_log_set_vprintf() hook that captures log lines up to the
 *     configured verbosity (INFO by default, see system_log_set_verbosity())
 *     into a small PSRAM ring buffer (console output is forwarded untouched).
 *  2. A low-priority task that drains the ring into the persistent log file -
 *     /sd/logs/system.log when a microSD card is mounted, /littlefs/logs/system.log
 *     otherwise.  The card is preferred because writing the internal flash parks
 *     both cores with the caches off, which stalls the MIPI-DSI scan-out and
 *     makes the panel flash (see SYSTEM_LOG_SD_FILE below).
 *  3. File rotation: when system.log reaches APP_LOG_MAX_FILE_BYTES it is
 *     renamed to system.log.1 (up to APP_LOG_MAX_ROTATED generations); the
 *     oldest generation is deleted, so storage usage is strictly bounded.
 *  4. A boot header (app version + reset reason) and a periodic heartbeat with
 *     heap/PSRAM statistics. A hard freeze (task WDT / panic) reboots the
 *     device, and the next boot header records the reset reason — the
 *     heartbeat gaps then show how much runtime was lost.
 *
 * NOTE: The scheduled (24h) restart from the 7" panel is intentionally NOT
 * included here; the configurable auto-restart task in app_main.c replaces it.
 */

#include "diag/system_log.h"

#include <errno.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "app_config.h"
#include "app_task.h"
#include "diag/display_flash_watch.h"
#include "diag/dsi_underrun_watch.h"
#include "diag/storage_guard.h"
#include "drivers/display_init.h"
#include "sd/sd_card.h"
#include "ui/ui_runtime.h"

#define TAG "syslog"

/* The log task does the heavy formatting: the periodic heartbeat (a 640-byte
 * line plus eight auxiliary strings), freeze_build(), the task dump and, under
 * them, newlib's vfprintf.  4096 was not enough headroom: with the render/flash
 * summary fields added to the heartbeat the canary tripped ~300 bytes below the
 * stack base, so the task rebooted the panel in a loop. */
#define SYSTEM_LOG_TASK_STACK 8192
#define SYSTEM_LOG_TASK_PRIO 1
#define SYSTEM_LOG_TASK_PERIOD_MS 2000
#define SYSTEM_LOG_CHUNK_BYTES 512
/* Longest log line that may be carried through the logging hook.  The health
 * line carries the newest flash report (kind, coverage, luma, rgb) as well, and
 * with that field it runs to ~580 characters; the budget is raised so the line
 * is never truncated mid-field. */
#define SYSTEM_LOG_LINE_MAX 640
#define SYSTEM_LOG_TAG_MAX 40

/* ---- Capture ring ------------------------------------------------------ */

typedef struct {
    uint8_t *buf;
    size_t cap;
    size_t read;    /* monotonic byte counter: bytes consumed */
    size_t written; /* monotonic byte counter: bytes produced */
    SemaphoreHandle_t mutex;
} log_ring_t;

static log_ring_t s_ring;
static TaskHandle_t s_task;
static vprintf_like_t s_orig_vprintf;
static bool s_init;
static SemaphoreHandle_t s_capture_mutex;

/* Appends suppressed because background flash writers were paused (see
 * storage_guard_flash_writers_pause()).  Atomic because the vprintf hook, the
 * log task and the HTTP task all call system_log_file_append(). */
static uint32_t s_gated_writes;

/* Serializes every access to the log files themselves (append / rotate / read /
 * clear).  esp_littlefs refuses to rename a file that still has an open handle
 * ("Cannot rename; src \"/logs/system.log\" is open."), so a log read that kept
 * the live file open while the writer rotated used to make the rotation fail:
 * the live file then grew past APP_LOG_MAX_FILE_BYTES and, worse,
 * freeze_reserve_space() could not reserve room for a crash dump. */
static SemaphoreHandle_t s_file_mutex;

static bool log_fs_lock(void)
{
    if (s_file_mutex == NULL) {
        return false;
    }
    return xSemaphoreTake(s_file_mutex, portMAX_DELAY) == pdTRUE;
}

static void log_fs_unlock(bool locked)
{
    if (locked) {
        xSemaphoreGive(s_file_mutex);
    }
}

/* Highest level letter copied into the persistent ring.  0 = nothing, then
 * E(1) / W(2) / I(3) / D(4).  Defaults to INFO so the log actually shows what
 * the panel is doing; the WebUI selector (and system_log_set_verbosity()) can
 * change it at runtime. */
static int s_capture_level = APP_LOG_VERBOSITY_DEFAULT;

/* Highest level letter still written to the USB-serial console.  Kept separate
 * from s_capture_level on purpose: the capture ring may be set to VERBOSE
 * without the serial port having to carry the same flood (see
 * APP_LOG_CONSOLE_LEVEL).  s_console_segment remembers the verdict for the line
 * currently being built, because with ESP_LOG_VERSION == 2 one line arrives as
 * several vprintf calls and the level is only known from the first one. */
static int s_console_level = APP_LOG_CONSOLE_LEVEL;
static bool s_console_segment = true;

/* UI watchdog state: last UI heartbeat value and when it last changed. */
static uint32_t s_ui_last_hb = 0;
static int64_t s_ui_last_hb_ms = 0;

/* Lateness of the last log-task wakeup.  The log task runs at priority 1 and
 * every other task/user code blocks it; when IT is late, the whole system was
 * starved (a long blocking filesystem or Wi-Fi operation, an interrupt storm),
 * which is not evidence of a hung UI task. */
static int64_t s_task_wait_late_ms;

/* Set once when a system-wide stall has already been forgiven.  It is cleared
 * again as soon as the UI heartbeat moves, so a genuinely hung UI task still
 * gets restarted after the following timeout window. */
static bool s_stall_grace_used;

/* Freeze forensics: where the UI task was last seen and which HTTP request was
 * being served.  Written by other tasks, read only when a freeze is reported. */
static volatile const char *s_ui_stage = "boot";
static char s_http_uri[80];
static portMUX_TYPE s_trace_mux = portMUX_INITIALIZER_UNLOCKED;

/* LVGL servicing forensics - see system_log.h.  Millisecond stamps (32-bit so
 * the reads from the reporting task can never tear) are compared against the
 * current time when a freeze is reported. */
static volatile const char *s_lvgl_cb = "?";
static volatile int32_t s_lvgl_cb_ms;
static volatile int32_t s_lvgl_hand_ms;
static volatile int32_t s_lvgl_slow_report_ms;

/* A gap this long between two LVGL callbacks can only mean a callback - or the
 * display refresh that runs between two of them - held the UI loop for that
 * long, which is what the user sees as a late repaint / flicker.  The port task
 * sleeps at most 100 ms between passes, so the idle wait cannot trigger it. */
#define SYSTEM_LOG_LVGL_SLOW_CB_MS 400
#define SYSTEM_LOG_LVGL_SLOW_CB_COOLDOWN_MS 2000

/* When the previous UI-watchdog evaluation ran.  A gap far beyond the log
 * heartbeat period proves the log task itself was blocked, i.e. the whole
 * system stalled - the strongest evidence that a reboot would be premature. */
static int64_t s_wd_check_prev_ms;
static bool s_freeze_warned;

/* Render watchdog state: the LVGL pipeline is considered stalled when the
 * render pass counter (display_render_stats_t.passes) stops advancing even
 * though the trace timer pings a 1x1 invalidation every APP_LVGL_TRACE_PERIOD_MS
 * (see display_trace_timer_cb in display_init_panel7.c). */
static uint32_t s_render_last_passes;
static int64_t s_render_last_passes_ms;
static bool s_render_baseline_valid;
static bool s_render_warned;
static bool s_render_recovered;

/* Maintenance window in which the stale-heartbeat restart is suppressed. */
static portMUX_TYPE s_wd_pause_mux = portMUX_INITIALIZER_UNLOCKED;
static uint32_t s_wd_pause_depth;
static int64_t s_wd_pause_since_ms;

/* vprintf line-reconstruction state. With ESP_LOG_VERSION == 2 the hook is
 * called three times per line (prefix, message body, newline) and those calls
 * are serialized by esp_log's stdout lock; with version 1 the whole line
 * arrives in one call and is serialized by s_capture_mutex instead. */
static bool s_line_open;
static char s_line_level;
static char s_line_tag[SYSTEM_LOG_TAG_MAX];
static char s_line[SYSTEM_LOG_LINE_MAX];
static size_t s_line_len;

static size_t ring_avail(const log_ring_t *r)
{
    return r->written - r->read;
}

static void ring_push(log_ring_t *r, const uint8_t *data, size_t len)
{
    if (r->buf == NULL || r->mutex == NULL || data == NULL || len == 0) {
        return;
    }

    xSemaphoreTake(r->mutex, portMAX_DELAY);
    while (len > 0) {
        size_t avail = ring_avail(r);
        if (avail >= r->cap) {
            /* Drop the oldest bytes so the newest diagnostics are kept. */
            r->read += (avail - r->cap) + 1U;
            continue;
        }
        size_t space = r->cap - avail;
        size_t n = len < space ? len : space;
        size_t off = r->written % r->cap;
        size_t first = r->cap - off;
        size_t c1 = n < first ? n : first;
        memcpy(r->buf + off, data, c1);
        if (n > c1) {
            memcpy(r->buf, data + c1, n - c1);
        }
        r->written += n;
        data += n;
        len -= n;
    }
    xSemaphoreGive(r->mutex);

    /* Wake the drain task immediately so W/E lines reach flash right away
     * instead of waiting up to SYSTEM_LOG_TASK_PERIOD_MS.  A panic/reboot can
     * strike between wakeups, so minimising the in-RAM window is what makes
     * the last lines before a crash survive. */
    if (s_task != NULL) {
        xTaskNotifyGive(s_task);
    }
}

static size_t ring_pop(log_ring_t *r, uint8_t *dst, size_t max)
{
    if (r->buf == NULL || r->mutex == NULL || dst == NULL || max == 0) {
        return 0;
    }

    size_t got = 0;
    xSemaphoreTake(r->mutex, portMAX_DELAY);
    size_t avail = ring_avail(r);
    if (avail > max) {
        avail = max;
    }
    size_t off = r->read % r->cap;
    size_t first = r->cap - off;
    size_t c1 = avail < first ? avail : first;
    memcpy(dst, r->buf + off, c1);
    if (avail > c1) {
        memcpy(dst + c1, r->buf, avail - c1);
    }
    r->read += avail;
    got = avail;
    xSemaphoreGive(r->mutex);
    return got;
}

/* ---- vprintf line reconstruction -------------------------------------- */

/* Rank of a level letter, matching s_capture_level.  Unknown letters rank 0
 * (never captured). */
static int level_rank(char level)
{
    switch (level) {
    case 'E': return 1;
    case 'W': return 2;
    case 'I': return 3;
    case 'D': return 4;
    /* Verbose ranks above debug so that the "everything" mode (5) is the only
     * one that copies V lines into the persistent ring. */
    case 'V': return 5;
    default:  return 0;
    }
}

static void line_finalize(void)
{
    if (!s_line_open) {
        return;
    }
    s_line_open = false;

    if (s_line_len == 0) {
        return;
    }

    char out[SYSTEM_LOG_LINE_MAX + SYSTEM_LOG_TAG_MAX + 8];
    int n = snprintf(out, sizeof(out), "%c %s: %s\n", s_line_level, s_line_tag, s_line);
    if (n <= 0) {
        return;
    }
    size_t len = (size_t)n;
    if (len >= sizeof(out)) {
        len = sizeof(out) - 1U;
    }
    ring_push(&s_ring, (const uint8_t *)out, len);
    s_line_len = 0;
    s_line[0] = '\0';
}

static void prefix_parse(const char *fmt, va_list args)
{
    if (s_line_open) {
        /* Defensive: a previous line never got its newline. */
        line_finalize();
    }

    char buf[160];
    va_list cp;
    va_copy(cp, args);
    vsnprintf(buf, sizeof(buf), fmt, cp);
    va_end(cp);

    const char *p = buf;
    if (p[0] == '\x1b' && p[1] == '[') {
        const char *m = strchr(p, 'm');
        if (m != NULL) {
            p = m + 1;
        }
    }

    if (level_rank(*p) == 0 || level_rank(*p) > s_capture_level) {
        /* Level is off or above the configured verbosity: keep the log compact. */
        s_line_open = false;
        return;
    }

    const char *tag_start = strstr(p, ") ");
    if (tag_start == NULL) {
        s_line_open = false;
        return;
    }
    tag_start += 2;
    const char *tag_end = strstr(tag_start, ": ");
    size_t tag_len = tag_end != NULL ? (size_t)(tag_end - tag_start) : strlen(tag_start);
    if (tag_len >= sizeof(s_line_tag)) {
        tag_len = sizeof(s_line_tag) - 1U;
    }

    s_line_level = *p;
    memcpy(s_line_tag, tag_start, tag_len);
    s_line_tag[tag_len] = '\0';
    s_line[0] = '\0';
    s_line_len = 0;
    s_line_open = true;
}

static bool is_newline_segment(const char *fmt, va_list args)
{
    if (strcmp(fmt, "%s") != 0) {
        return false;
    }

    char buf[16];
    va_list cp;
    va_copy(cp, args);
    int n = vsnprintf(buf, sizeof(buf), fmt, cp);
    va_end(cp);

    if (n <= 0 || n >= (int)sizeof(buf)) {
        return false;
    }
    if (buf[n - 1] != '\n') {
        return false;
    }

    /* Accept only whitespace + ANSI color-reset before the newline. */
    const char *s = buf;
    while (*s != '\0') {
        if (*s == '\n' || *s == '\r' || *s == ' ') {
            s++;
            continue;
        }
        if (*s == '\x1b') {
            const char *m = strchr(s, 'm');
            if (m == NULL) {
                return false;
            }
            s = m + 1;
            continue;
        }
        return false;
    }
    return true;
}

static void line_append(const char *fmt, va_list args)
{
    if (s_line_len >= sizeof(s_line) - 1U) {
        return;
    }

    va_list cp;
    va_copy(cp, args);
    int n = vsnprintf(s_line + s_line_len, sizeof(s_line) - s_line_len, fmt, cp);
    va_end(cp);

    if (n > 0) {
        size_t add = (size_t)n;
        size_t remain = sizeof(s_line) - 1U - s_line_len;
        if (add > remain) {
            add = remain;
        }
        s_line_len += add;
        s_line[s_line_len] = '\0';
    }
}

/* ESP_LOG_VERSION == 1 path: the vprintf hook receives each log line as a
 * single call whose format is the fully-composed line (LOG_FORMAT in
 * esp_log_format.h):
 *
 *   LOG_COLOR_x "x (%" PRIu32 ") %s: " user_format LOG_RESET_COLOR "\n"
 *
 * With colors disabled (this project) the format string starts directly with
 * the level letter; with colors enabled it starts with an ANSI escape. The
 * level is detected from the format string itself so I/D/V lines are rejected
 * without rendering. E/W lines are rendered, stripped of color sequences, and
 * re-emitted in the compact "<level> <tag>: <message>\n" form used by the
 * ring. */
static void capture_v1_line(const char *fmt, va_list args)
{
    const char *f = fmt;
    if (f[0] == '\x1b' && f[1] == '[') {
        const char *m = strchr(f, 'm');
        if (m != NULL) {
            f = m + 1;
        }
    }

    const int rank = level_rank(*f);
    if (rank == 0 || rank > s_capture_level) {
        return;
    }
    if (f[1] != ' ' || f[2] != '(') {
        return;
    }

    static char buf[SYSTEM_LOG_LINE_MAX + 160];
    va_list cp;
    va_copy(cp, args);
    int n = vsnprintf(buf, sizeof(buf), fmt, cp);
    va_end(cp);
    if (n <= 0) {
        return;
    }

    size_t len = (size_t)n;
    if (len >= sizeof(buf)) {
        len = sizeof(buf) - 1U;
    }
    buf[len] = '\0';

    const char *p = buf;
    if (p[0] == '\x1b' && p[1] == '[') {
        const char *m = strchr(p, 'm');
        if (m != NULL) {
            p = m + 1;
        }
    }

    const char *tag_start = strstr(p, ") ");
    if (tag_start == NULL) {
        return;
    }
    tag_start += 2;

    const char *tag_end = strstr(tag_start, ": ");
    if (tag_end == NULL) {
        return;
    }
    const char *msg_start = tag_end + 2;

    /* Strip the trailing LOG_RESET_COLOR and newline from the message. */
    size_t msg_len = strlen(msg_start);
    while (msg_len > 0 && (msg_start[msg_len - 1] == '\n' || msg_start[msg_len - 1] == '\r')) {
        msg_len--;
    }
    static const char reset[] = "\x1b[0m";
    if (msg_len >= (sizeof(reset) - 1) &&
        memcmp(msg_start + msg_len - (sizeof(reset) - 1), reset, sizeof(reset) - 1) == 0) {
        msg_len -= sizeof(reset) - 1;
    }

    size_t tag_len = (size_t)(tag_end - tag_start);
    if (tag_len >= sizeof(s_line_tag)) {
        tag_len = sizeof(s_line_tag) - 1U;
    }
    memcpy(s_line_tag, tag_start, tag_len);
    s_line_tag[tag_len] = '\0';

    static char out[SYSTEM_LOG_LINE_MAX + SYSTEM_LOG_TAG_MAX + 8];
    int m = snprintf(out, sizeof(out), "%c %s: %.*s\n", *p, s_line_tag, (int)msg_len, msg_start);
    if (m <= 0) {
        return;
    }
    size_t out_len = (size_t)m;
    if (out_len >= sizeof(out)) {
        out_len = sizeof(out) - 1U;
    }
    ring_push(&s_ring, (const uint8_t *)out, out_len);
}

/* Decide whether the line about to be printed may go to the serial console.
 * Only the level is needed, and with ESP_LOG_VERSION == 1 it is the first
 * character of the composed line, so nothing has to be rendered.  Anything that
 * is not a log line (a plain printf, a partially written line) is always
 * forwarded. */
static bool console_allows(const char *fmt, va_list args)
{
    if (fmt == NULL || s_console_level >= SYSTEM_LOG_VERBOSITY_VERBOSE) {
        return true;
    }

    /* ESP_LOG_VERSION == 2 prefix call: "%s%c " with (tag, level) as arguments.
     * The level letter is only reachable through the argument list. */
    if (strncmp(fmt, "%s%c ", 5) == 0) {
        va_list probe;
        va_copy(probe, args);
        (void)va_arg(probe, const char *);
        const int level = va_arg(probe, int);
        va_end(probe);
        return level_rank((char)level) <= s_console_level;
    }

    /* Continuation of a v2 line: same verdict as its prefix. */
    if (s_line_open) {
        return s_console_segment;
    }

    const char *f = fmt;
    if (f[0] == '\x1b' && f[1] == '[') {
        const char *m = strchr(f, 'm');
        if (m == NULL) {
            return true;
        }
        f = m + 1;
    }

    const int rank = level_rank(*f);
    return rank == 0 || rank <= s_console_level;
}

static int system_log_vprintf(const char *fmt, va_list args)
{
    int ret = 0;
    /* Always refreshed for a line's first segment; a continuation segment gets
     * the verdict its prefix stored (console_allows() returns it unchanged). */
    s_console_segment = console_allows(fmt, args);
    if (s_orig_vprintf != NULL && s_console_segment) {
        va_list fwd;
        va_copy(fwd, args);
        ret = s_orig_vprintf(fmt, fwd);
        va_end(fwd);
    }

    if (fmt == NULL || !s_init) {
        return ret;
    }

    /* ESP_LOG_VERSION == 2: prefix call starts the line. */
    if (strncmp(fmt, "%s%c ", 5) == 0) {
        prefix_parse(fmt, args);
        return ret;
    }

    if (s_line_open) {
        if (is_newline_segment(fmt, args)) {
            line_finalize();
        } else {
            line_append(fmt, args);
        }
        return ret;
    }

    /* ESP_LOG_VERSION == 1: the entire line arrives as a single call. The
     * capture path renders into shared static buffers, so it is serialized by
     * s_capture_mutex (v1 has no stdout lock around the hook). */
    if (s_capture_mutex != NULL &&
        xSemaphoreTake(s_capture_mutex, portMAX_DELAY) == pdTRUE) {
        capture_v1_line(fmt, args);
        xSemaphoreGive(s_capture_mutex);
    }
    return ret;
}

/* ---- Storage ----------------------------------------------------------- */

/* ---- Where the log file lives -------------------------------------------
 * Writing the internal flash is the one operation that can stall the DSI
 * scan-out: it switches the caches off and parks *both* cores until the
 * erase/program finished, and the panel driver re-arms the scan-out from an
 * interrupt once per frame - so a write that lands on a frame boundary leaves
 * the panel showing the bridge's filler colour for as long as it lasts.  See
 * diag/storage_guard.h for the meter and diag/dsi_underrun_watch.c for the
 * witness.
 *
 * The persistent log therefore lives on the microSD card whenever one is
 * mounted: SDMMC/FATFS goes through the card's IDMA with the interrupts
 * enabled and never parks a core.  LittleFS stays the fallback, so a panel
 * without a card keeps logging exactly as before.
 *
 * Only the path changes - the drain cadence, the ring and the rotation limits
 * are untouched - and the reader falls back to the LittleFS file, so the lines
 * written before the card was mounted stay readable. */
#define SYSTEM_LOG_SD_DIR  APP_SD_MOUNT_POINT "/logs"
#define SYSTEM_LOG_SD_FILE SYSTEM_LOG_SD_DIR "/system.log"

/* Room for either storage's path plus ".<n>". */
#define SYSTEM_LOG_SEGMENT_PATH_BYTES 96

static bool system_log_on_sd(void)
{
    return sd_card_is_mounted();
}

/* Path of log generation `rot` (0 = the live file) on the active storage. */
static void system_log_path(int rot, char *out, size_t out_len)
{
    const char *file = system_log_on_sd() ? SYSTEM_LOG_SD_FILE : APP_LOG_FILE;
    if (rot <= 0) {
        snprintf(out, out_len, "%s", file);
    } else {
        snprintf(out, out_len, "%s.%d", file, rot);
    }
}

/* Rotated log files are named "<log>.<n>" with larger n = older, the same
 * convention system_log_rotate() writes. */
static void system_log_segment_path(int rot, char *out, size_t out_len)
{
    system_log_path(rot, out, out_len);
}

/* Caller must hold the file lock (log_fs_lock()). */
static void system_log_rotate(void)
{
    char old_path[SYSTEM_LOG_SEGMENT_PATH_BYTES];
    char new_path[SYSTEM_LOG_SEGMENT_PATH_BYTES];

    /* Delete the oldest generation first. */
    system_log_path(APP_LOG_MAX_ROTATED, old_path, sizeof(old_path));
    remove(old_path);

    for (int i = APP_LOG_MAX_ROTATED - 1; i >= 1; i--) {
        system_log_path(i, old_path, sizeof(old_path));
        system_log_path(i + 1, new_path, sizeof(new_path));
        rename(old_path, new_path);
    }

    system_log_path(1, old_path, sizeof(old_path));
    system_log_path(0, new_path, sizeof(new_path));
    rename(new_path, old_path);
}

static void system_log_file_write(const uint8_t *data, size_t len)
{
    const bool locked = log_fs_lock();

    char path[SYSTEM_LOG_SEGMENT_PATH_BYTES];
    system_log_path(0, path, sizeof(path));

    FILE *f = fopen(path, "ab");
    if (f == NULL && system_log_on_sd()) {
        /* The card task creates the folder when the card is mounted, but the
         * log task can get there first - and a fresh card needs it again. */
        (void)mkdir(SYSTEM_LOG_SD_DIR, 0777);
        f = fopen(path, "ab");
    }
    if (f == NULL) {
        log_fs_unlock(locked);
        return;
    }

    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        log_fs_unlock(locked);
        return;
    }
    long size = ftell(f);
    if (size < 0) {
        fclose(f);
        log_fs_unlock(locked);
        return;
    }
    if ((unsigned long)size + (unsigned long)len > (unsigned long)APP_LOG_MAX_FILE_BYTES) {
        fclose(f);
        system_log_rotate();
        f = fopen(path, "ab");
        if (f == NULL) {
            log_fs_unlock(locked);
            return;
        }
    }

    fwrite(data, 1, len, f);
    fclose(f);
    log_fs_unlock(locked);
}

static void system_log_file_append(const uint8_t *data, size_t len)
{
    if (data == NULL || len == 0) {
        return;
    }

    /* While flash writers are paused (esp_ota_end() is remapping MMU pages and
     * this build runs its code from PSRAM through the same MMU) keep the bytes
     * in RAM: a flash write now is what resets the chip. */
    if (storage_guard_flash_writers_paused()) {
        (void)__atomic_add_fetch(&s_gated_writes, 1u, __ATOMIC_RELAXED);
        ring_push(&s_ring, data, len);
        return;
    }

    /* The bracket is the point of this whole split: an append to the card is
     * attributed to the SD bus (it must never disable the caches), an append to
     * LittleFS is announced to the flash meter - which is what the frame-gap
     * witness reads when it reports a screen stall. */
    if (system_log_on_sd()) {
        dsi_bus_activity_begin(DSI_BUS_SD);
        system_log_file_write(data, len);
        dsi_bus_activity_end(DSI_BUS_SD);
    } else {
        storage_guard_flash_op_begin(FLASH_OP_LOG);
        system_log_file_write(data, len);
        storage_guard_flash_op_end(FLASH_OP_LOG);
    }
}

/* Caller must hold the file lock (log_fs_lock()). */
static unsigned long system_log_file_size(void)
{
    char path[SYSTEM_LOG_SEGMENT_PATH_BYTES];
    system_log_path(0, path, sizeof(path));

    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        return 0;
    }
    unsigned long size = 0;
    if (fseek(f, 0, SEEK_END) == 0) {
        long pos = ftell(f);
        if (pos > 0) {
            size = (unsigned long)pos;
        }
    }
    fclose(f);
    return size;
}

/* Rotates up front so that a whole forensic dump still fits afterwards.
 * system_log_file_append() rotates mid-write when the file is full and drops
 * the tail silently if the reopen fails, which is how the task table went
 * missing from an earlier freeze report. */
static void freeze_reserve_space(size_t bytes)
{
    /* Held across the size check and the rotation: log_fs_lock() keeps other
     * appenders out, so nothing can rotate the file in between. */
    const bool locked = log_fs_lock();
    if ((unsigned long)bytes + system_log_file_size() <= (unsigned long)APP_LOG_MAX_FILE_BYTES) {
        log_fs_unlock(locked);
        return;
    }
    system_log_rotate();
    log_fs_unlock(locked);
}

void system_log_watchdog_suspend(void)
{
    if (!s_init) {
        return;
    }
    portENTER_CRITICAL(&s_wd_pause_mux);
    if (s_wd_pause_depth == 0) {
        s_wd_pause_since_ms = esp_timer_get_time() / 1000;
    }
    s_wd_pause_depth++;
    portEXIT_CRITICAL(&s_wd_pause_mux);
}

void system_log_watchdog_resume(void)
{
    if (!s_init) {
        return;
    }

    bool last = false;
    int64_t since_ms = 0;
    portENTER_CRITICAL(&s_wd_pause_mux);
    if (s_wd_pause_depth > 0) {
        s_wd_pause_depth--;
    }
    last = (s_wd_pause_depth == 0);
    since_ms = s_wd_pause_since_ms;
    portEXIT_CRITICAL(&s_wd_pause_mux);

    if (!last) {
        return;
    }

    /* A long window makes the old baseline meaningless, so re-seed it and do not
     * charge the maintenance time to the UI task.  Short windows (an SD mount
     * probe) leave the baseline alone so a real hang is still caught. */
    int64_t now_ms = esp_timer_get_time() / 1000;
    if ((now_ms - since_ms) >= APP_LOG_WATCHDOG_REBASE_MIN_MS) {
        s_ui_last_hb = ui_runtime_get_heartbeat();
        s_ui_last_hb_ms = now_ms;
    }
}

uint32_t system_log_gated_writes(void)
{
    return __atomic_load_n(&s_gated_writes, __ATOMIC_RELAXED);
}

void system_log_gated_writes_reset(void)
{
    __atomic_store_n(&s_gated_writes, 0u, __ATOMIC_RELAXED);
}

static bool watchdog_suspended(void)
{
    bool suspended = false;
    int64_t since_ms = 0;

    portENTER_CRITICAL(&s_wd_pause_mux);
    if (s_wd_pause_depth > 0) {
        suspended = true;
        since_ms = s_wd_pause_since_ms;
    }
    portEXIT_CRITICAL(&s_wd_pause_mux);

    if (!suspended) {
        return false;
    }

    /* Bounded window: an operation that hangs for far longer than any SDK call
     * we know of must not keep the watchdog disabled forever. */
    if ((esp_timer_get_time() / 1000 - since_ms) > APP_LOG_WATCHDOG_PAUSE_MAX_MS) {
        return false;
    }
    return true;
}

/* ---- Freeze forensics -------------------------------------------------- */

void system_log_note_stage(const char *stage)
{
    s_ui_stage = (stage != NULL) ? stage : "?";
}

void system_log_note_http(const char *uri)
{
    portENTER_CRITICAL(&s_trace_mux);
    if (uri == NULL) {
        s_http_uri[0] = '\0';
    } else {
        snprintf(s_http_uri, sizeof(s_http_uri), "%s", uri);
    }
    portEXIT_CRITICAL(&s_trace_mux);
}

void system_log_note_lvgl_cb(const char *name)
{
    const int32_t now_ms = (int32_t)(esp_timer_get_time() / 1000);
    const int32_t prev_ms = s_lvgl_cb_ms;
    const char *prev_name = (const char *)s_lvgl_cb;

    /* Time from entering the previous callback to entering this one.  The LVGL
     * refresh timer runs between two callbacks and is not named here, so the pair
     * can also point at the drawing pass; either way the delta is the stall. */
    if (prev_ms != 0) {
        const int32_t delta = now_ms - prev_ms;
        if (delta >= SYSTEM_LOG_LVGL_SLOW_CB_MS &&
            (int32_t)(now_ms - s_lvgl_slow_report_ms) >= SYSTEM_LOG_LVGL_SLOW_CB_COOLDOWN_MS) {
            s_lvgl_slow_report_ms = now_ms;
            ESP_LOGW(TAG, "lvgl stall: %d ms between callbacks %s -> %s (repaint delayed)",
                     (int)delta, prev_name, (name != NULL) ? name : "?");
        }
    }

    s_lvgl_cb = (name != NULL) ? name : "?";
    s_lvgl_cb_ms = now_ms;
}

void system_log_lvgl_handler_beat(void)
{
    s_lvgl_hand_ms = (int32_t)(esp_timer_get_time() / 1000);
}

/* Live view of the LVGL port task for the heartbeat line: how long the current
 * lv_timer_handler() call has been running and which callback the handler
 * entered last.  "trace" as the callback name means the timer list was processed
 * and nothing else started afterwards, so a stall reported with it sits in the
 * refresh/render part of the handler rather than in a UI callback.  Stamps are
 * 32-bit so a read from the reporting task cannot tear, and the values are
 * advisory exactly like the lock tracker. */
static void system_log_lvgl_trace_fields(char *out, size_t out_len)
{
    const int32_t now_ms = (int32_t)(esp_timer_get_time() / 1000);
    const int32_t hand_stamp = s_lvgl_hand_ms;
    const int32_t cb_stamp = s_lvgl_cb_ms;

    if (hand_stamp == 0) {
        snprintf(out, out_len, "lvcb=none lvhand=none");
        return;
    }
    if (cb_stamp == 0) {
        snprintf(out, out_len, "lvcb=none lvhand=%dms", (int)(now_ms - hand_stamp));
        return;
    }
    snprintf(out, out_len, "lvcb=%s@%dms lvhand=%dms", (const char *)s_lvgl_cb,
             (int)(now_ms - cb_stamp), (int)(now_ms - hand_stamp));
}

/* ---- Tracked lock ownership -------------------------------------------- */
/* The freeze dump has to answer "who is sitting on the display lock / the HA
 * model mutex", which FreeRTOS cannot tell us for a recursive LVGL mutex and
 * which is easy to get wrong for a mutex held from many sites.  Each tracked
 * resource therefore keeps a tiny shadow record of its owner while it is held.
 * The record is advisory: it is written without a lock so that a lock holder
 * never has to take another one to report itself. */

#define SYSTEM_LOG_LOCK_TRACK_SLOTS 4

/* Holding the display or model lock this long is already a stall the user can
 * see (a refresh pass takes single-digit milliseconds), so it gets a warning
 * line naming the holder instead of only showing up in a later dump. */
#define SYSTEM_LOG_LOCK_SLOW_MS 250

typedef struct {
    const char *name;
    TaskHandle_t owner;
    char owner_name[configMAX_TASK_NAME_LEN];
    uint32_t held_since_ms;
    uint32_t depth;
    uint32_t max_hold_ms;   /* worst single hold seen since boot */
    uint32_t slow_count;    /* holds that crossed SYSTEM_LOG_LOCK_SLOW_MS */
} system_log_lock_track_t;

static system_log_lock_track_t s_lock_track[SYSTEM_LOG_LOCK_TRACK_SLOTS];
static bool s_anon_owner_warned = false;

static system_log_lock_track_t *lock_track_find(const char *lock_name)
{
    if (lock_name == NULL || lock_name[0] == '\0') {
        return NULL;
    }
    for (size_t i = 0; i < SYSTEM_LOG_LOCK_TRACK_SLOTS; i++) {
        if (s_lock_track[i].name != NULL && strcmp(s_lock_track[i].name, lock_name) == 0) {
            return &s_lock_track[i];
        }
    }
    return NULL;
}

static system_log_lock_track_t *lock_track_slot(const char *lock_name)
{
    system_log_lock_track_t *slot = lock_track_find(lock_name);
    if (slot != NULL) {
        return slot;
    }
    for (size_t i = 0; i < SYSTEM_LOG_LOCK_TRACK_SLOTS; i++) {
        if (s_lock_track[i].name == NULL) {
            s_lock_track[i].name = lock_name;
            return &s_lock_track[i];
        }
    }
    return NULL;
}

void system_log_lock_acquire(const char *lock_name)
{
    system_log_lock_track_t *slot = lock_track_slot(lock_name);
    if (slot == NULL) {
        return;
    }
    if (slot->depth == 0) {
        TaskHandle_t self = xTaskGetCurrentTaskHandle();
        const char *task_name = (self != NULL) ? pcTaskGetName(self) : NULL;
        if (task_name != NULL && task_name[0] != '\0') {
            snprintf(slot->owner_name, sizeof(slot->owner_name), "%s", task_name);
        } else {
            /* Some tasks are created without a name; the handle still lets us
             * match the holder against the task table in the dump. */
            snprintf(slot->owner_name, sizeof(slot->owner_name), "anon%p", (void *)self);
            if (!s_anon_owner_warned) {
                s_anon_owner_warned = true;
                ESP_LOGW(TAG, "%s taken by an unnamed task (handle=%p prio=%u)",
                         lock_name, (void *)self,
                         (unsigned)((self != NULL) ? uxTaskPriorityGet(self) : 0));
            }
        }
        slot->owner = self;
        slot->held_since_ms = (uint32_t)(esp_timer_get_time() / 1000);
    }
    slot->depth++;
}

void system_log_lock_release(const char *lock_name)
{
    system_log_lock_track_t *slot = lock_track_find(lock_name);
    if (slot == NULL || slot->depth == 0) {
        return;
    }
    slot->depth--;
    if (slot->depth == 0) {
        /* The outermost release is the interesting one: that is how long the
         * resource was unavailable to anybody else.  A slow release is logged
         * from here (not from the acquire side) because the release happens
         * outside the protected section on every call site. */
        uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
        uint32_t held_ms = now_ms - slot->held_since_ms;
        if (held_ms > slot->max_hold_ms) {
            slot->max_hold_ms = held_ms;
        }
        if (held_ms >= SYSTEM_LOG_LOCK_SLOW_MS) {
            slot->slow_count++;
            ESP_LOGW(TAG, "%s held %ums by %s (slow #%u)", lock_name, (unsigned)held_ms,
                     slot->owner_name, (unsigned)slot->slow_count);
            char note[96];
            snprintf(note, sizeof(note), "%s held %ums by %s", lock_name, (unsigned)held_ms,
                     slot->owner_name);
            display_flash_watch_note(note);
        }
        slot->owner = NULL;
        slot->owner_name[0] = '\0';
    }
}

void system_log_lock_worst(char *out, size_t out_len)
{
    if (out == NULL || out_len == 0) {
        return;
    }
    const system_log_lock_track_t *worst = NULL;
    for (size_t i = 0; i < SYSTEM_LOG_LOCK_TRACK_SLOTS; i++) {
        if (s_lock_track[i].name == NULL || s_lock_track[i].max_hold_ms == 0) {
            continue;
        }
        if (worst == NULL || s_lock_track[i].max_hold_ms > worst->max_hold_ms) {
            worst = &s_lock_track[i];
        }
    }
    if (worst == NULL) {
        snprintf(out, out_len, "none");
        return;
    }
    snprintf(out, out_len, "%s:%ums/%u", worst->name, (unsigned)worst->max_hold_ms,
             (unsigned)worst->slow_count);
}

bool system_log_lock_owner(const char *lock_name, char *out, size_t out_len)
{
    if (out == NULL || out_len == 0) {
        return false;
    }
    system_log_lock_track_t *slot = lock_track_find(lock_name);
    if (slot == NULL || slot->owner == NULL) {
        snprintf(out, out_len, "free");
        return false;
    }
    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
    snprintf(out, out_len, "%s@%ums/d%u", slot->owner_name,
             (unsigned)(now_ms - slot->held_since_ms), (unsigned)slot->depth);
    return true;
}

static void freeze_append(const char *text, int n)
{
    if (text == NULL || n <= 0) {
        return;
    }
    size_t len = (size_t)n;
    size_t avail = strlen(text);
    if (len > avail) {
        len = avail;
    }
    system_log_file_append((const uint8_t *)text, len);
}

/* Renders one forensic line: who stalled, for how long, which UI stage and
 * which HTTP request were in flight, and whether a storage operation was
 * holding the watchdogs off.  Returns the number of characters written. */
static const char *ui_state_name(void)
{
    /* Mirrors eTaskState; a ready task that is not progressing is starved by
     * higher-priority work, a blocked one is waiting for something. */
    switch (ui_runtime_get_task_state()) {
    case 0:  return "run";
    case 1:  return "ready";
    case 2:  return "block";
    case 3:  return "susp";
    case 4:  return "del";
    default: return "?";
    }
}

/* Ground truth for "which task is actually executing on this core": the task
 * table reports the affinity a task was created with, which need not match the
 * core it is scheduled on.  Returns a static task name, never NULL. */
static const char *freeze_core_task_name(BaseType_t core)
{
#if (!CONFIG_FREERTOS_SMP) && (configUSE_MUTEXES == 1)
    TaskHandle_t handle = xTaskGetCurrentTaskHandleForCore(core);
    return (handle != NULL) ? pcTaskGetName(handle) : "-";
#else
    (void)core;
    return "?";
#endif
}

static int freeze_build(char *dst, size_t dst_len, const char *kind,
                        int64_t stalled_ms, int64_t log_gap_ms)
{
    char http[sizeof(s_http_uri)];
    char lvgl[sizeof(s_lock_track[0].owner_name) + 24];
    char model[sizeof(s_lock_track[0].owner_name) + 24];
    char lvcb[48];
    char lvhand[16];
    char core0[24];
    char core1[24];
    char render[128];
    char apply[80];
    const char *stage = (const char *)s_ui_stage;
    int32_t now_ms = (int32_t)(esp_timer_get_time() / 1000);
    int32_t cb_stamp = s_lvgl_cb_ms;
    int32_t hand_stamp = s_lvgl_hand_ms;
    int64_t cb_age = (cb_stamp != 0) ? (int32_t)(now_ms - cb_stamp) : -1;
    int64_t hand_age = (hand_stamp != 0) ? (int32_t)(now_ms - hand_stamp) : -1;

    portENTER_CRITICAL(&s_trace_mux);
    snprintf(http, sizeof(http), "%s", s_http_uri);
    portEXIT_CRITICAL(&s_trace_mux);

    (void)system_log_lock_owner("lvgl", lvgl, sizeof(lvgl));
    (void)system_log_lock_owner("model", model, sizeof(model));
    display_render_stats_format(render, sizeof(render));
    (void)ui_runtime_get_apply_stats(apply, sizeof(apply));

    snprintf(core0, sizeof(core0), "%s", freeze_core_task_name(0));
    snprintf(core1, sizeof(core1), "%s", freeze_core_task_name(1));

    if (cb_age >= 0) {
        snprintf(lvcb, sizeof(lvcb), "%s@%lldms", (const char *)s_lvgl_cb, (long long)cb_age);
    } else {
        snprintf(lvcb, sizeof(lvcb), "-");
    }
    if (hand_age >= 0) {
        snprintf(lvhand, sizeof(lvhand), "%lldms", (long long)hand_age);
    } else {
        snprintf(lvhand, sizeof(lvhand), "-");
    }

    return snprintf(dst, dst_len,
                    "!!! FREEZE: %s ui_stall=%lldms log_task_gap=%lldms stage=%s ui=%s http=%s storage_guard=%s lvgl=%s model=%s core0=%s core1=%s lvcb=%s lvhand=%s %s %s !!!\n",
                    kind, (long long)stalled_ms, (long long)log_gap_ms,
                    (stage != NULL) ? stage : "?", ui_state_name(),
                    (http[0] != '\0') ? http : "-",
                    storage_guard_active() ? "held" : "idle", lvgl, model,
                    core0, core1, lvcb, lvhand, render, apply);
}

/* Task table for the post-mortem: which tasks were blocked, on which core, and
 * how close they are to overflowing their stacks.  Needs
 * CONFIG_FREERTOS_USE_TRACE_FACILITY. */
#define FREEZE_TASK_MAX 48

static void system_log_dump_tasks(void)
{
#if CONFIG_FREERTOS_USE_TRACE_FACILITY
    /* uxTaskGetSystemState() returns 0 (and fills nothing) when the array is
     * smaller than the live task count - which is what silently turned this
     * report into "count=0" for a panel that runs more than two dozen tasks. */
    static TaskStatus_t tasks[FREEZE_TASK_MAX];
    UBaseType_t total = uxTaskGetNumberOfTasks();
    UBaseType_t count = uxTaskGetSystemState(tasks, FREEZE_TASK_MAX, NULL);

    /* Reserve the whole table before writing the first line; a mid-dump
     * rotation is what silently truncated this report before. */
    freeze_reserve_space(1024 + (size_t)count * 192);

    char header[96];
    int hn = snprintf(header, sizeof(header), "!!! TASK dump count=%u total=%u !!!\n",
                      (unsigned)count, (unsigned)total);
    freeze_append(header, hn);

    if (count == 0) {
        char note[96];
        int n = snprintf(note, sizeof(note),
                         "!!! TASK dump failed: %u tasks do not fit in %u slots !!!\n",
                         (unsigned)total, (unsigned)FREEZE_TASK_MAX);
        freeze_append(note, n);
    }

    for (UBaseType_t i = 0; i < count; i++) {
        const char *state = "?";
        switch (tasks[i].eCurrentState) {
        case eRunning:   state = "run";    break;
        case eReady:     state = "ready";  break;
        case eBlocked:   state = "block";  break;
        case eSuspended: state = "susp";   break;
        case eDeleted:   state = "del";    break;
        default:         state = "?";      break;
        }

        bool is_ui = (tasks[i].pcTaskName != NULL) && (strcmp(tasks[i].pcTaskName, "ui_runtime") == 0);

        /* Affinity, not the core the task happened to be on: a hog pinned to
         * the display core is the case that matters.  TaskStatus_t.xCoreID is
         * only filled with CONFIG_FREERTOS_VTASKLIST_INCLUDE_COREID, which
         * pulls in the vTaskList formatting code, so ask the kernel instead. */
        BaseType_t affinity = xTaskGetCoreID(tasks[i].xHandle);
        char core_txt[16];
        if (affinity == tskNO_AFFINITY) {
            snprintf(core_txt, sizeof(core_txt), "any");
        } else {
            snprintf(core_txt, sizeof(core_txt), "%ld", (long)affinity);
        }

        char line[176];
        int n = snprintf(line, sizeof(line),
                         "!!! TASK %-14s state=%-5s prio=%-2u stack_hwm=%-6u core=%-3s%s\n",
                         tasks[i].pcTaskName, state,
                         (unsigned)tasks[i].uxCurrentPriority,
                         (unsigned)tasks[i].usStackHighWaterMark,
                         core_txt,
                         is_ui ? " [UI]" : "");
        freeze_append(line, n);
    }

    char tail[48];
    int tn = snprintf(tail, sizeof(tail), "!!! TASK dump end !!!\n");
    freeze_append(tail, tn);
#else
    char note[72];
    int n = snprintf(note, sizeof(note), "!!! TASK dump unavailable (trace facility off) !!!\n");
    freeze_append(note, n);
#endif
}

/* How much stack a task has left is the one danger the panel cannot see coming:
 * a stack protection fault (mcause 27) reboots before anything can be logged and
 * the core-dump summary carries no stack trace.  Sampling every task on a slow
 * timer turns that into a trend the log shows long before it panics - this is
 * how the SD export task was found running with 88 bytes of its 16 KB left.
 * The ISR stacks cannot be sampled from here, so their configured size is
 * printed alongside for context. */
#define STACK_AUDIT_WARN_BYTES 512U
#define STACK_AUDIT_REPORT_MAX 5

/* Task stack audit cadence.  These are wall-clock milliseconds rather than
 * loop passes: the log task is woken by every captured line, so a pass count
 * would stretch the interval whenever the panel goes quiet and shorten it when
 * the panel is busy - the opposite of what a trend report needs. */
#define STACK_AUDIT_FIRST_MS 10000
#define STACK_AUDIT_INTERVAL_MS 600000

/* First report 10 s after boot, when every task exists, then one every 10
 * minutes.  Returns immediately on all the other passes. */
static void system_log_stack_audit(void)
{
#if CONFIG_FREERTOS_USE_TRACE_FACILITY
    static int64_t s_next_audit_ms = STACK_AUDIT_FIRST_MS;

    int64_t now_ms = esp_timer_get_time() / 1000;
    if (now_ms < s_next_audit_ms) {
        return;
    }
    s_next_audit_ms = now_ms + STACK_AUDIT_INTERVAL_MS;

    UBaseType_t total = uxTaskGetNumberOfTasks();
    if (total == 0) {
        return;
    }

    /* From the heap rather than BSS: the report runs a few times per hour and
     * internal DRAM is the scarcest resource on this panel. */
    TaskStatus_t *tasks =
        (TaskStatus_t *)heap_caps_malloc((size_t)total * sizeof(TaskStatus_t), MALLOC_CAP_8BIT);
    if (tasks == NULL) {
        return;
    }

    UBaseType_t count = uxTaskGetSystemState(tasks, total, NULL);

    /* Keep the tasks with the least stack left, worst first. */
    const TaskStatus_t *worst[STACK_AUDIT_REPORT_MAX] = {0};
    for (UBaseType_t i = 0; i < count; i++) {
        for (int slot = 0; slot < STACK_AUDIT_REPORT_MAX; slot++) {
            if (worst[slot] == NULL || tasks[i].usStackHighWaterMark < worst[slot]->usStackHighWaterMark) {
                for (int move = STACK_AUDIT_REPORT_MAX - 1; move > slot; move--) {
                    worst[move] = worst[move - 1];
                }
                worst[slot] = &tasks[i];
                break;
            }
        }
    }

    char line[320];
    size_t used = 0;
    int n = snprintf(line, sizeof(line), "stack audit isr=%uB/core",
                     (unsigned)CONFIG_FREERTOS_ISR_STACKSIZE);
    if (n > 0) {
        used = (size_t)n;
    }

    uint32_t lowest = 0xFFFFFFFFu;
    for (int slot = 0; slot < STACK_AUDIT_REPORT_MAX && worst[slot] != NULL; slot++) {
        uint32_t bytes = (uint32_t)worst[slot]->usStackHighWaterMark * (uint32_t)sizeof(StackType_t);
        if (bytes < lowest) {
            lowest = bytes;
        }
        if (used + 1U < sizeof(line)) {
            n = snprintf(line + used, sizeof(line) - used, " %s=%uB", worst[slot]->pcTaskName,
                         (unsigned)bytes);
            if (n > 0) {
                used += (size_t)n;
                if (used >= sizeof(line)) {
                    used = sizeof(line) - 1U;
                }
            }
        }
    }

    if (lowest < STACK_AUDIT_WARN_BYTES) {
        /* A low margin is an error line: those are stored whatever the log
         * verbosity is set to, so the trend survives a quiet log setting. */
        ESP_LOGW(TAG, "STACK LOW: %s", line);
        system_log_write("stack", "STACK LOW: %s", line);
    } else {
        system_log_write_info("stack", "%s", line);
    }

    heap_caps_free(tasks);
#endif
}

static void system_log_check_render_watchdog(int64_t now_ms)
{
    if (!display_is_ready()) {
        return;
    }

    display_render_stats_t rs;
    display_render_stats_get(&rs);

    if (watchdog_suspended()) {
        /* A deliberately long maintenance operation is in progress; re-baseline
         * so the window after it ends starts from a known-good counter. */
        s_render_last_passes = rs.passes;
        s_render_last_passes_ms = now_ms;
        s_render_baseline_valid = false;
        s_render_warned = false;
        s_render_recovered = false;
        return;
    }

    if (!s_render_baseline_valid) {
        s_render_baseline_valid = true;
        s_render_last_passes = rs.passes;
        s_render_last_passes_ms = now_ms;
        return;
    }

    if (rs.passes != s_render_last_passes) {
        s_render_last_passes = rs.passes;
        s_render_last_passes_ms = now_ms;
        s_render_warned = false;
        s_render_recovered = false;
        return;
    }

    int64_t stalled_ms = now_ms - s_render_last_passes_ms;

    /* Early warning: names the stall while it may still recover on its own.
     * No restart at this point. */
    if (stalled_ms >= APP_RENDER_STALL_WARN_MS && !s_render_warned) {
        s_render_warned = true;
        char line[512];
        int n = freeze_build(line, sizeof(line), "render stalled", stalled_ms, 0);
        freeze_append(line, n);
        system_log_dump_tasks();
        ESP_LOGW(TAG, "LVGL render stalled for %lld ms (passes=%u)",
                 (long long)stalled_ms, (unsigned)rs.passes);
    }

    if (stalled_ms < APP_RENDER_STALL_TIMEOUT_MS) {
        return;
    }

    if (!s_render_recovered) {
        s_render_recovered = true;
        ESP_LOGW(TAG, "LVGL render stalled for %lld ms; attempting soft recovery",
                 (long long)stalled_ms);
        display_force_invalidate();
        /* Re-arm the window: if the soft recovery revives the pipeline, passes
         * advances and this state resets; if not, the next timeout restarts. */
        s_render_last_passes_ms = now_ms;
        return;
    }

    char line[512];
    int n = freeze_build(line, sizeof(line), "restarting (render)", stalled_ms, 0);
    freeze_append(line, n);
    system_log_dump_tasks();
    ESP_LOGE(TAG, "LVGL render watchdog triggered (no render passes for %lld ms after soft recovery); restarting",
             (long long)stalled_ms);
    esp_restart();
}

static void system_log_check_ui_watchdog(void)
{
    int64_t now_ms = esp_timer_get_time() / 1000;
    int64_t check_gap_ms = (s_wd_check_prev_ms > 0) ? (now_ms - s_wd_check_prev_ms) : 0;
    s_wd_check_prev_ms = now_ms;

    system_log_check_render_watchdog(now_ms);

    if (!ui_runtime_is_running()) {
        return;
    }

    uint32_t hb = ui_runtime_get_heartbeat();

    if (watchdog_suspended()) {
        /* A deliberately long operation is in progress; it is allowed to keep
         * the UI task busy without being restarted for it. */
        s_ui_last_hb = hb;
        s_ui_last_hb_ms = now_ms;
        s_freeze_warned = false;
        return;
    }

    if (hb != s_ui_last_hb) {
        s_ui_last_hb = hb;
        s_ui_last_hb_ms = now_ms;
        s_stall_grace_used = false;
        s_freeze_warned = false;
        return;
    }

    if (hb == 0) {
        /* UI task hasn't produced its first heartbeat yet. */
        return;
    }

    int64_t stalled_ms = now_ms - s_ui_last_hb_ms;

    /* This check runs once per log-task period, so a much larger gap proves
     * the log task itself was blocked - the whole system stalled, which is not
     * evidence of a hung UI task. */
    int64_t log_gap_ms = (check_gap_ms > (int64_t)SYSTEM_LOG_TASK_PERIOD_MS)
                             ? check_gap_ms - (int64_t)SYSTEM_LOG_TASK_PERIOD_MS
                             : 0;

    /* Early warning: a freeze that recovers on its own is still worth a line,
     * because it names the code path that blocked.  No restart here. */
    if (stalled_ms >= APP_LOG_STALL_WARN_MS && !s_freeze_warned) {
        s_freeze_warned = true;
        char line[512];
        int n = freeze_build(line, sizeof(line), "UI heartbeat stale",
                             stalled_ms, log_gap_ms);
        freeze_append(line, n);
        /* Also capture the task table here: a stall that recovers on its own
         * still shows who was blocked, which is the only clue left when the
         * panel keeps running. */
        system_log_dump_tasks();
        ESP_LOGW(TAG, "UI heartbeat stale for %lld ms (stage=%s)",
                 (long long)stalled_ms, (const char *)s_ui_stage);
    }

    if (stalled_ms <= APP_UI_WATCHDOG_TIMEOUT_MS) {
        return;
    }

    /* The log task was starved (or blocked) too, so this was a system-wide
     * stall and not a UI task holding the LVGL lock: forgive it once and keep
     * running.  The grace is per-stall, so a panel that stays frozen still
     * restarts after the following timeout window. */
    bool system_wide = (log_gap_ms >= APP_LOG_STALL_REARM_MS) ||
                       (s_task_wait_late_ms >= APP_LOG_STALL_REARM_MS);

    /* A UI task that is ready but not running is starved by higher-priority
     * work (a long HTTP handler, an image decode, a flash write): it resumes on
     * its own as soon as that work yields, so rebooting would only add a boot
     * to an already busy moment.  Only a task that is actually blocked is
     * stuck. */
    bool starved = (ui_runtime_get_task_state() == (int)eReady);

    char line[512];
    int n = 0;

    if ((system_wide || starved) && !s_stall_grace_used) {
        s_stall_grace_used = true;
        s_ui_last_hb = hb;
        s_ui_last_hb_ms = now_ms;

        n = freeze_build(line, sizeof(line),
                         starved ? "UI task starved, watchdog re-armed"
                                 : "system-wide stall, watchdog re-armed",
                         stalled_ms, log_gap_ms);
        freeze_append(line, n);
        system_log_dump_tasks();
        if (starved) {
            ESP_LOGW(TAG, "UI task starved (%lld ms, state=ready); watchdog re-armed, not restarting",
                     (long long)stalled_ms);
        } else {
            ESP_LOGW(TAG, "system-wide stall (%lld ms, log task gap %lld ms); watchdog re-armed, not restarting",
                     (long long)stalled_ms, (long long)log_gap_ms);
        }
        return;
    }

    n = freeze_build(line, sizeof(line), "restarting", stalled_ms, log_gap_ms);
    freeze_append(line, n);
    system_log_dump_tasks();
    ESP_LOGE(TAG, "UI task watchdog triggered (no heartbeat for %lld ms); restarting",
             (long long)stalled_ms);
    esp_restart();
}

static void system_log_heartbeat(void)
{
    uint32_t uptime_s = (uint32_t)(esp_timer_get_time() / 1000000ULL);
    size_t free_heap = esp_get_free_heap_size();
    size_t min_heap = esp_get_minimum_free_heap_size();
    size_t free_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    /* No heap_caps_get_largest_free_block() anywhere in this heartbeat: that one
     * walks the free list of a fragmented pool while holding the allocator
     * mutex, and this used to call it four times every 30 s.  The free sizes are
     * counters and cost nothing; the per-region "largest block" figures are
     * still available on demand in /api/diagnostics. */
    size_t free_internal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    size_t free_dma = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    size_t free_iram = heap_caps_get_free_size(MALLOC_CAP_IRAM_8BIT);

    char lock_lvgl[sizeof(s_lock_track[0].owner_name) + 24];
    char lock_model[sizeof(s_lock_track[0].owner_name) + 24];
    char lock_worst[48];
    char lvgl_trace[64];
    /* Only the log task runs system_log_heartbeat(), and the line plus the
     * diagnostics summaries no longer fit in the task's stack comfortably, so
     * the two largest buffers live in BSS instead. */
    static char line[2048];
    static char dsi[640];
    char flash[256];
    char render[128];
    char apply[80];
    (void)system_log_lock_owner("lvgl", lock_lvgl, sizeof(lock_lvgl));
    (void)system_log_lock_owner("model", lock_model, sizeof(lock_model));
    system_log_lock_worst(lock_worst, sizeof(lock_worst));
    system_log_lvgl_trace_fields(lvgl_trace, sizeof(lvgl_trace));
    display_flash_watch_summary(flash, sizeof(flash));
    dsi_underrun_watch_summary(dsi, sizeof(dsi));
    display_render_stats_format(render, sizeof(render));
    (void)ui_runtime_get_apply_stats(apply, sizeof(apply));

    /* Report render passes that repainted the whole screen (the flash the user
     * sees) as a normal log event, with the page that was active.  The note is
     * raised from inside the LVGL refresh pass, which must not log by itself. */
    display_render_note_t note;
    while (display_render_note_take(&note)) {
        system_log_event("render",
                         "%s repaint %ums dirty=%u%% page=%s (#full=%u #slow=%u dropped=%u)",
                         note.full ? "FULL" : "slow", (unsigned)note.ms, (unsigned)note.dirty_pct,
                         note.page_id, (unsigned)note.full_passes, (unsigned)note.slow_passes,
                         (unsigned)note.dropped);
    }

    /* Time spent inside the internal flash, per kind, as count / total / worst
     * (milliseconds).  This is the number the screen artifact is proportional
     * to: every millisecond counted here is a millisecond in which a frame
     * boundary can fall inside a window with the interrupts masked, and the DSI
     * scan-out is left to the bridge filler when it does.  `store` says where
     * the log file lives - "sd" means the periodic writes have left the flash
     * entirely. */
    char flashops[192];
    (void)snprintf(flashops, sizeof(flashops),
                   "flashop log n=%u t=%ums m=%ums graph n=%u t=%ums m=%ums nvs n=%u t=%ums m=%ums ui n=%u t=%ums m=%ums",
                   (unsigned)storage_guard_flash_op_count(FLASH_OP_LOG),
                   (unsigned)storage_guard_flash_op_total_ms(FLASH_OP_LOG),
                   (unsigned)storage_guard_flash_op_max_ms(FLASH_OP_LOG),
                   (unsigned)storage_guard_flash_op_count(FLASH_OP_GRAPH),
                   (unsigned)storage_guard_flash_op_total_ms(FLASH_OP_GRAPH),
                   (unsigned)storage_guard_flash_op_max_ms(FLASH_OP_GRAPH),
                   (unsigned)storage_guard_flash_op_count(FLASH_OP_NVS),
                   (unsigned)storage_guard_flash_op_total_ms(FLASH_OP_NVS),
                   (unsigned)storage_guard_flash_op_max_ms(FLASH_OP_NVS),
                   (unsigned)storage_guard_flash_op_count(FLASH_OP_UI),
                   (unsigned)storage_guard_flash_op_total_ms(FLASH_OP_UI),
                   (unsigned)storage_guard_flash_op_max_ms(FLASH_OP_UI));

    int n = snprintf(line, sizeof(line),
                     "I sys: uptime=%us heap_free=%u heap_min=%u psram_free=%u "
                     "int_free=%u dma_free=%u iram_free=%u store=%s %s "
                     "ui=%s locks lvgl=%s model=%s worst=%s %s %s %s %s %s\n",
                     (unsigned)uptime_s, (unsigned)free_heap, (unsigned)min_heap,
                     (unsigned)free_psram, (unsigned)free_internal, (unsigned)free_dma,
                     (unsigned)free_iram, system_log_on_sd() ? "sd" : "flash", flashops,
                     ui_state_name(), lock_lvgl, lock_model, lock_worst, lvgl_trace, flash, dsi, render,
                     apply);
    if (n <= 0) {
        return;
    }
    size_t len = (size_t)n;
    if (len >= sizeof(line)) {
        len = sizeof(line) - 1U;
    }
    system_log_file_append((const uint8_t *)line, len);
}

static void system_log_boot_header(void)
{
    const esp_app_desc_t *desc = esp_app_get_description();
    const char *ver = (desc != NULL && desc->version[0] != '\0') ? desc->version : "unknown";

    static const char *const reset_names[] = {
        "UNKNOWN", "POWERON", "EXT", "SW", "PANIC", "INT_WDT", "TASK_WDT",
        "WDT", "DEEPSLEEP", "BROWNOUT", "SDIO", "USB", "JTAG", "EFUSE",
        "PWR_GLITCH", "CPU_LOCKUP", "SUPER_WDT",
    };
    esp_reset_reason_t rr = esp_reset_reason();
    const char *reset_name =
        (rr >= 0 && rr < (esp_reset_reason_t)(sizeof(reset_names) / sizeof(reset_names[0])))
            ? reset_names[rr]
            : "INVALID";

    char line[256];
    int n = snprintf(line, sizeof(line),
                     "=== boot app=%s version=%s reset=%s(%d) isr_stack=%uB/core ===\n",
                     APP_NAME, ver, reset_name, (int)rr, (unsigned)CONFIG_FREERTOS_ISR_STACKSIZE);
    if (n > 0) {
        size_t len = (size_t)n;
        if (len >= sizeof(line)) {
            len = sizeof(line) - 1U;
        }
        system_log_file_append((const uint8_t *)line, len);
    }

    /* Make crashes impossible to miss when scrolling the web log viewer. */
    const bool crash_reset =
        rr == ESP_RST_PANIC || rr == ESP_RST_INT_WDT || rr == ESP_RST_TASK_WDT ||
        rr == ESP_RST_WDT || rr == ESP_RST_BROWNOUT || rr == ESP_RST_CPU_LOCKUP;
    if (crash_reset) {
        char crash_line[128];
        int cn = snprintf(crash_line, sizeof(crash_line),
                          "!!! CRASH: previous boot ended with reset=%s(%d) !!!\n",
                          reset_name, (int)rr);
        if (cn > 0) {
            size_t clen = (size_t)cn;
            if (clen >= sizeof(crash_line)) {
                clen = sizeof(crash_line) - 1U;
            }
            system_log_file_append((const uint8_t *)crash_line, clen);
        }
    }

    /* Immediate first snapshot so a crash very early in the boot is bounded. */
    system_log_heartbeat();
}

/* ---- Task -------------------------------------------------------------- */

static void system_log_task(void *arg)
{
    (void)arg;
    uint8_t chunk[SYSTEM_LOG_CHUNK_BYTES];
    uint32_t loops = 0;
    uint32_t heartbeat_loops =
        (APP_LOG_HEARTBEAT_MS + SYSTEM_LOG_TASK_PERIOD_MS - 1U) / SYSTEM_LOG_TASK_PERIOD_MS;

    for (;;) {
        /* Every period, not only on the 30 s heap snapshot: a freeze should be
         * reported (and, if it is a real UI hang, punished) within seconds of
         * crossing the threshold, not up to half a minute later. */
        system_log_check_ui_watchdog();

        for (;;) {
            /* Paused for the duration of an MMU remap: stop draining instead of
             * popping, because the append below would just push the same bytes
             * back and spin.  The ring keeps them until the window closes. */
            if (storage_guard_flash_writers_paused()) {
                break;
            }
            size_t n = ring_pop(&s_ring, chunk, sizeof(chunk));
            if (n == 0) {
                break;
            }
            system_log_file_append(chunk, n);
        }

        loops++;
        if (heartbeat_loops > 0 && loops % heartbeat_loops == 0) {
            system_log_heartbeat();
        }

        /* 10 s after boot, then every 10 minutes - the gate is inside. */
        system_log_stack_audit();

        int64_t wait_start_ms = esp_timer_get_time() / 1000;
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(SYSTEM_LOG_TASK_PERIOD_MS));
        int64_t waited_ms = (esp_timer_get_time() / 1000) - wait_start_ms;
        s_task_wait_late_ms = (waited_ms > (int64_t)SYSTEM_LOG_TASK_PERIOD_MS)
                                  ? waited_ms - (int64_t)SYSTEM_LOG_TASK_PERIOD_MS
                                  : 0;
    }
}

/* ---- Public API -------------------------------------------------------- */

static void system_log_pin_noisy_tags(void);

esp_err_t system_log_init(void)
{
    if (s_init) {
        return ESP_OK;
    }

    (void)mkdir(APP_LOG_DIR, 0755); /* ignore error: may already exist */
    /* The card is usually mounted later than this; the write path creates the
     * folder on demand as well. */
    if (system_log_on_sd()) {
        (void)mkdir(SYSTEM_LOG_SD_DIR, 0777);
    }

    s_ring.buf = (uint8_t *)heap_caps_malloc(APP_LOG_RING_BYTES,
                                             MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_ring.buf == NULL) {
        s_ring.buf = (uint8_t *)heap_caps_malloc(APP_LOG_RING_BYTES, MALLOC_CAP_8BIT);
    }
    if (s_ring.buf == NULL) {
        ESP_LOGW(TAG, "log ring alloc failed");
        return ESP_ERR_NO_MEM;
    }
    s_ring.cap = APP_LOG_RING_BYTES;

    s_ring.mutex = xSemaphoreCreateMutex();
    if (s_ring.mutex == NULL) {
        heap_caps_free(s_ring.buf);
        s_ring.buf = NULL;
        return ESP_ERR_NO_MEM;
    }

    s_capture_mutex = xSemaphoreCreateMutex();
    if (s_capture_mutex == NULL) {
        vSemaphoreDelete(s_ring.mutex);
        s_ring.mutex = NULL;
        heap_caps_free(s_ring.buf);
        s_ring.buf = NULL;
        return ESP_ERR_NO_MEM;
    }

    s_file_mutex = xSemaphoreCreateMutex();
    if (s_file_mutex == NULL) {
        vSemaphoreDelete(s_capture_mutex);
        s_capture_mutex = NULL;
        vSemaphoreDelete(s_ring.mutex);
        s_ring.mutex = NULL;
        heap_caps_free(s_ring.buf);
        s_ring.buf = NULL;
        return ESP_ERR_NO_MEM;
    }

    s_init = true;

    /* Pin the per-packet/per-DMA chatter before anything else can log: the
     * verbosity comes from the settings *before* the UI comes up, so a stored
     * VERBOSE would otherwise flood the console and the ring from the very first
     * millisecond of every boot. */
    system_log_pin_noisy_tags();
    s_orig_vprintf = esp_log_set_vprintf(system_log_vprintf);

    BaseType_t ok = app_task_create_pinned(system_log_task, TAG, SYSTEM_LOG_TASK_STACK,
                                           NULL, SYSTEM_LOG_TASK_PRIO, &s_task, 0);
    if (ok != pdPASS) {
        s_task = NULL;
        ESP_LOGW(TAG, "log task create failed; ring will drop oldest lines");
    }

    system_log_boot_header();
    return ESP_OK;
}

esp_err_t system_log_flush(void)
{
    if (!s_init) {
        return ESP_ERR_INVALID_STATE;
    }

    /* Nothing to do while the writers are held back: the bytes are safe in the
     * ring and the log task cannot append them anyway.  Returning early also
     * keeps the fallback loop below from popping and re-pushing forever. */
    if (storage_guard_flash_writers_paused()) {
        return ESP_OK;
    }

    if (s_task != NULL) {
        xTaskNotifyGive(s_task);
        /* Give the low-priority task a moment to drain and append. */
        vTaskDelay(pdMS_TO_TICKS(100));
    } else {
        uint8_t chunk[SYSTEM_LOG_CHUNK_BYTES];
        for (;;) {
            size_t n = ring_pop(&s_ring, chunk, sizeof(chunk));
            if (n == 0) {
                break;
            }
            system_log_file_append(chunk, n);
        }
    }
    return ESP_OK;
}

/* Rotated log files are named "<log>.<n>" with larger n = older, the same
 * convention system_log_rotate() writes.  Segment 0 is the live file. */

/* Read one log segment, newest bytes last.  Returns the byte count, or -1 when
 * the segment does not exist (so a caller walking the history can stop).
 * The whole open/read/close runs under the file lock: a handle left open here
 * would block the writer's rotation (esp_littlefs refuses to rename an open
 * file), and a rotation in the middle of this read would silently shorten the
 * segment we report. */
int system_log_read_segment(int rot, char *buf, size_t buf_len)
{
    if (buf == NULL || buf_len == 0 || rot < 0 || rot > APP_LOG_MAX_ROTATED) {
        return -1;
    }

    /* Only the live segment has buffered data waiting in the capture ring.
     * Flushed before taking the lock (the flush appends, so it needs the lock
     * itself). */
    if (rot == 0) {
        system_log_flush();
    }

    char path[SYSTEM_LOG_SEGMENT_PATH_BYTES];
    system_log_segment_path(rot, path, sizeof(path));

    size_t want = buf_len - 1U;
    const bool locked = log_fs_lock();

    FILE *f = fopen(path, "rb");
    if (f == NULL && system_log_on_sd()) {
        /* The card is the active storage now, but the history written before it
         * was mounted (and the lines written after it is pulled) is still in
         * LittleFS - keep it readable rather than reporting an empty log. */
        if (rot <= 0) {
            snprintf(path, sizeof(path), "%s", APP_LOG_FILE);
        } else {
            snprintf(path, sizeof(path), "%s.%d", APP_LOG_FILE, rot);
        }
        f = fopen(path, "rb");
    }
    if (f == NULL) {
        log_fs_unlock(locked);
        return -1;
    }

    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        log_fs_unlock(locked);
        return -1;
    }
    long size = ftell(f);
    if (size > (long)want) {
        if (fseek(f, size - (long)want, SEEK_SET) != 0) {
            fclose(f);
            log_fs_unlock(locked);
            return -1;
        }
    } else {
        rewind(f);
    }

    size_t got = fread(buf, 1, want, f);
    fclose(f);
    log_fs_unlock(locked);
    buf[got] = '\0';
    return (int)got;
}

int system_log_read_tail(char *buf, size_t buf_len)
{
    if (buf == NULL || buf_len == 0) {
        return 0;
    }

    int got = system_log_read_segment(0, buf, buf_len);
    if (got < 0) {
        buf[0] = '\0';
        return 0;
    }
    return got;
}

esp_err_t system_log_clear(void)
{
    if (!s_init) {
        return ESP_ERR_INVALID_STATE;
    }

    /* Drain the capture ring first so a clear request can't race pending
     * lines back into the file after we truncate it. */
    system_log_flush();

    /* Removing files is a filesystem mutation: serialize it with the writer so
     * it can't land between a rotation's remove()/rename() pair.  Both
     * storages are cleared: "clear" means the history is gone, and the fallback
     * the reader walks must not resurrect it. */
    const bool locked = log_fs_lock();
    for (int i = 0; i <= APP_LOG_MAX_ROTATED; i++) {
        char path[SYSTEM_LOG_SEGMENT_PATH_BYTES];
        system_log_segment_path(i, path, sizeof(path));
        remove(path);

        if (i == 0) {
            snprintf(path, sizeof(path), "%s", APP_LOG_FILE);
        } else {
            snprintf(path, sizeof(path), "%s.%d", APP_LOG_FILE, i);
        }
        remove(path);
    }
    log_fs_unlock(locked);

    /* Leave a fresh line so the viewer shows a start point, not "(no logs)". */
    system_log_heartbeat();
    return ESP_OK;
}

static void system_log_write_level(char level, const char *tag, const char *fmt, va_list ap)
{
    if (fmt == NULL) {
        return;
    }

    const int rank = level_rank(level);
    if (rank == 0 || rank > s_capture_level) {
        return;
    }

    char body[SYSTEM_LOG_LINE_MAX];
    int n = vsnprintf(body, sizeof(body), fmt, ap);
    if (n <= 0) {
        return;
    }

    char out[SYSTEM_LOG_LINE_MAX + SYSTEM_LOG_TAG_MAX + 8];
    int m = snprintf(out, sizeof(out), "%c %s: %s\n",
                     level,
                     (tag != NULL && tag[0] != '\0') ? tag : "sys",
                     body);
    if (m <= 0) {
        return;
    }
    size_t len = (size_t)m;
    if (len >= sizeof(out)) {
        len = sizeof(out) - 1U;
    }
    ring_push(&s_ring, (const uint8_t *)out, len);
}

void system_log_write(const char *tag, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    system_log_write_level('E', tag, fmt, ap);
    va_end(ap);
}

void system_log_write_info(const char *tag, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    system_log_write_level('I', tag, fmt, ap);
    va_end(ap);
}

/* Components that log per frame, per packet or per DMA descriptor.  Opening the
 * global filter up to DEBUG/VERBOSE for them turns the log link into the
 * bottleneck of the whole panel: on this board the camera ISP task shares core 0
 * with IDLE0, the queue behind the console pushed one frame past the task
 * watchdog window, the watchdog dumped the ISP registers and reset the panel -
 * and because the verbosity is restored from the settings *before* the UI comes
 * up, the resulting reboot loop survived every restart and took the HTTP server
 * with it (no page, no OTA, no logs).  These tags therefore stay at WARN
 * whatever the verbosity selector says; every other tag still follows it.
 * Keep the list short - the tag filter uses a small cache. */
static const char *const k_noisy_tags[] = {
    "ISP",          /* camera image signal processing: ~60 debug lines per frame */
    "esp_isp",
    "cam_hal",
    "sccb",
    "esp_video",
    "ov5647",
    "H_SDIO_DRV",   /* Wi-Fi coprocessor transport: per DMA event */
    "hci_stub_drv",
    "H_API",
    "rpc_core",     /* ESP-Hosted RPC: per request and per response */
    "rpc_rsp",
    "rpc_evt",
    "transport",
    "wifi_mgr",
    /* Measured over 80 s of VERBOSE on this panel (USB capture, ~11 KB/s): the
     * per-packet, per-DMA and per-allocation components of the Wi-Fi SDIO path
     * and of the DSI panel. They are pinned not only to protect the console but
     * to keep the 8 KB capture ring usable - at VERBOSE they alone turned it over
     * in ~2 s, evicting every screen/saver line the user actually wants. */
    "cache",        /* 4692 lines/80 s: per cache operation */
    "sdmmc_req",    /* 4186: per SDIO transaction */
    "bus_rx",       /* 1247: hex dump of every received packet */
    "sdmmc_cmd",    /* 858: per command */
    "mpool",        /* 742: per heap allocation */
    "sdmmc_periph", /* 387 */
    "sdio_wrapper", /* 346 */
    "bus_TX",       /* 212: hex dump of every transmitted packet */
    "lcd.dsi",      /* 131: per DSI transaction */
    "serial",       /* console driver re-entering the log hook */
    "serial_ll",
    "serial_write",
    "serial_read",
    "ll_read",
};

static void system_log_pin_noisy_tags(void)
{
    for (size_t i = 0; i < (sizeof(k_noisy_tags) / sizeof(k_noisy_tags[0])); i++) {
        esp_log_level_set(k_noisy_tags[i], ESP_LOG_WARN);
    }
}

void system_log_set_verbosity(int level)
{
    if (level < SYSTEM_LOG_VERBOSITY_OFF) {
        level = SYSTEM_LOG_VERBOSITY_OFF;
    }
    if (level > SYSTEM_LOG_VERBOSITY_VERBOSE) {
        level = SYSTEM_LOG_VERBOSITY_VERBOSE;
    }
    s_capture_level = level;

    /* Mirror the setting onto the esp_log runtime filter, but NEVER above
     * APP_LOG_ESP_MAX_LEVEL.  The global default level is the only gate on the
     * ESP_EARLY_LOGV/ESP_DRAM_LOGV lines of the HALs: with CONFIG_LOG_VERSION_1
     * those expand to esp_rom_printf(LOG_FORMAT(...)) and check nothing but
     * esp_log_get_default_level() - no tag filter, no vprintf hook, so neither
     * system_log_pin_noisy_tags() nor the console cap can hold them back.  At
     * VERBOSE the Wi-Fi SDIO path alone (cache, sdmmc_req/cmd, bus_rx/bus_TX,
     * mpool) measured ~11 KB/s on this panel and starved the HTTP server and the
     * UI task until the screen only showed the saver flashing. */
    static const esp_log_level_t runtime_levels[] = {
        ESP_LOG_NONE, ESP_LOG_ERROR, ESP_LOG_WARN, ESP_LOG_INFO, ESP_LOG_DEBUG, ESP_LOG_VERBOSE,
    };
    esp_log_level_t applied = runtime_levels[level];
    if (applied > APP_LOG_ESP_MAX_LEVEL) {
        applied = APP_LOG_ESP_MAX_LEVEL;
    }
    esp_log_level_set("*", applied);
    system_log_pin_noisy_tags();

    /* Record the effective filter, so a log that looks incomplete can be told
     * apart from a log that was configured to be incomplete. */
    static const char *const runtime_level_names[] = {"NONE", "ERROR", "WARN", "INFO", "DEBUG", "VERBOSE"};
    static const char capture_letters[] = {'-', 'E', 'W', 'I', 'D', 'V'};
    char line[160];
    snprintf(line, sizeof(line),
             "log verbosity -> %d (capture <= %c, esp_log * = %s%s, %u noisy tags pinned at WARN)",
             level, capture_letters[level], runtime_level_names[(int)applied],
             (applied != runtime_levels[level]) ? " capped by APP_LOG_ESP_MAX_LEVEL" : "",
             (unsigned)(sizeof(k_noisy_tags) / sizeof(k_noisy_tags[0])));
    system_log_write_info("log", "%s", line);
}

int system_log_get_verbosity(void)
{
    return s_capture_level;
}

void system_log_event(const char *tag, const char *fmt, ...)
{
    char body[SYSTEM_LOG_LINE_MAX];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(body, sizeof(body), fmt, ap);
    va_end(ap);
    if (n <= 0) {
        return;
    }

    system_log_write_info(tag != NULL ? tag : "event", "%s", body);
    display_flash_watch_note(body);
}
