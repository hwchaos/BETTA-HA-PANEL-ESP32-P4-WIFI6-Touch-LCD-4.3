/* SPDX-License-Identifier: LicenseRef-FNCL-1.1
 * Copyright (c) 2026 Cpt_Kirk
 */
#include "diag/storage_guard.h"

#include <stdio.h>
#include <string.h>

#include "app_config.h"
#include "diag/system_log.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#if CONFIG_ESP_TASK_WDT_EN
#include "esp_task_wdt.h"
#endif

static const char *TAG = "storage";

/* The task watchdog is configured by the startup code from Kconfig; restore
 * exactly that configuration when a window closes. */
#if CONFIG_ESP_TASK_WDT_CHECK_IDLE_TASK_CPU0
#define TWDT_IDLE_MASK_CPU0 (1u << 0)
#else
#define TWDT_IDLE_MASK_CPU0 0u
#endif

#if CONFIG_ESP_TASK_WDT_CHECK_IDLE_TASK_CPU1
#define TWDT_IDLE_MASK_CPU1 (1u << 1)
#else
#define TWDT_IDLE_MASK_CPU1 0u
#endif

#define TWDT_IDLE_MASK_WATCHED (TWDT_IDLE_MASK_CPU0 | TWDT_IDLE_MASK_CPU1)
#define TWDT_TIMEOUT_MS ((unsigned)(CONFIG_ESP_TASK_WDT_TIMEOUT_S) * 1000u)

#if CONFIG_ESP_TASK_WDT_PANIC
#define TWDT_TRIGGER_PANIC true
#else
#define TWDT_TRIGGER_PANIC false
#endif

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static uint32_t s_depth;
static int64_t s_started_ms;
static char s_reason[20] = "storage";

/* Counted window that forbids *all* background flash writes; see
 * storage_guard_flash_writers_pause().  Kept separate from s_depth because it
 * covers only the few hundred milliseconds of MMU reprogramming inside
 * esp_ota_end(), while the guard window is open for the whole transfer. */
static portMUX_TYPE s_writer_mux = portMUX_INITIALIZER_UNLOCKED;
static uint32_t s_writer_depth;

#if CONFIG_ESP_TASK_WDT_EN
/* Leaving idle_core_mask at 0 unsubscribes both idle tasks; IDF then finds an
 * empty watch list and keeps the timer stopped, so nothing can reset the chip
 * for the duration of the window. */
static void task_wdt_watch_idle(bool watch)
{
    esp_task_wdt_config_t config = {
        .timeout_ms = TWDT_TIMEOUT_MS,
        .idle_core_mask = watch ? TWDT_IDLE_MASK_WATCHED : 0u,
        .trigger_panic = TWDT_TRIGGER_PANIC,
    };

    esp_err_t err = esp_task_wdt_reconfigure(&config);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "Task watchdog reconfigure (%s) failed: %s", watch ? "watch" : "pause", esp_err_to_name(err));
    }
}
#else
static void task_wdt_watch_idle(bool watch)
{
    (void)watch;
}
#endif

void storage_guard_begin(const char *reason)
{
    bool first;
    portENTER_CRITICAL(&s_mux);
    first = (s_depth == 0);
    s_depth++;
    portEXIT_CRITICAL(&s_mux);

    if (!first) {
        return;
    }

    if (reason != NULL) {
        strlcpy(s_reason, reason, sizeof(s_reason));
    }
    s_started_ms = esp_timer_get_time() / 1000;

    task_wdt_watch_idle(false);
    system_log_watchdog_suspend();
}

void storage_guard_end(void)
{
    bool last;
    int64_t duration_ms = 0;

    portENTER_CRITICAL(&s_mux);
    if (s_depth > 0) {
        s_depth--;
    }
    last = (s_depth == 0);
    portEXIT_CRITICAL(&s_mux);

    if (!last) {
        return;
    }

    duration_ms = esp_timer_get_time() / 1000 - s_started_ms;

    system_log_watchdog_resume();
    task_wdt_watch_idle(true);

    if (duration_ms >= APP_STORAGE_GUARD_LOG_MS) {
        ESP_LOGW(TAG, "%s took %lld ms with the watchdogs held off", s_reason, (long long)duration_ms);
    }
}

bool storage_guard_active(void)
{
    bool active;
    portENTER_CRITICAL(&s_mux);
    active = (s_depth > 0);
    portEXIT_CRITICAL(&s_mux);
    return active;
}

void storage_guard_flash_writers_pause(void)
{
    bool first;

    portENTER_CRITICAL(&s_writer_mux);
    first = (s_writer_depth == 0);
    s_writer_depth++;
    portEXIT_CRITICAL(&s_writer_mux);

    if (!first) {
        return;
    }

    system_log_gated_writes_reset();

    /* Any append that already passed its check and is inside LittleFS has to
     * reach the flash before the caller reprograms the MMU, otherwise the race
     * this window exists for is simply moved a few microseconds earlier. */
    vTaskDelay(pdMS_TO_TICKS(APP_FLASH_WRITER_SETTLE_MS));
}

void storage_guard_flash_writers_resume(void)
{
    bool last;

    portENTER_CRITICAL(&s_writer_mux);
    if (s_writer_depth > 0) {
        s_writer_depth--;
    }
    last = (s_writer_depth == 0);
    portEXIT_CRITICAL(&s_writer_mux);

    if (!last) {
        return;
    }

    const uint32_t held = system_log_gated_writes();
    if (held > 0) {
        ESP_LOGW(TAG, "flash writers resumed, %u log writes flushed from RAM", (unsigned)held);
    }
}

bool storage_guard_flash_writers_paused(void)
{
    bool paused;
    portENTER_CRITICAL(&s_writer_mux);
    paused = (s_writer_depth > 0);
    portEXIT_CRITICAL(&s_writer_mux);
    return paused;
}

/* ---- Flash operation meter (see storage_guard.h) ----------------------- */

/* An operation stays "recent" this long after it ended: the frame interrupt
 * that reports a screen gap runs *after* the park window opened again, so the
 * operation that caused it has usually just finished. */
#define FLASH_OP_RECENT_MS 5u

/* An append that takes tens of milliseconds is an outlier (garbage
 * collection); anything above this is worth counting separately. */
#define FLASH_OP_SLOW_MS 3u

typedef struct {
    volatile uint32_t count;
    volatile uint32_t total_ms;
    volatile uint32_t max_ms;
    volatile uint32_t slow;
} flash_op_stat_t;

static flash_op_stat_t s_flash_op[FLASH_OP_KIND_COUNT];
static volatile uint32_t s_flash_op_depth;
static volatile uint32_t s_flash_op_begin_ms;
static volatile uint32_t s_flash_op_last_kind = FLASH_OP_KIND_COUNT;
static volatile uint32_t s_flash_op_last_ms;
static volatile uint32_t s_flash_op_last_end_ms;

static const char *const k_flash_op_name[FLASH_OP_KIND_COUNT] = {"log", "graph", "nvs", "ui"};

static inline uint32_t flash_op_now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

void storage_guard_flash_op_begin(flash_op_kind_t kind)
{
    const uint32_t now = flash_op_now_ms();
    s_flash_op_last_kind = (kind < FLASH_OP_KIND_COUNT) ? (uint32_t)kind : (uint32_t)FLASH_OP_KIND_COUNT;
    s_flash_op_begin_ms = now;
    s_flash_op_depth++;
}

void storage_guard_flash_op_end(flash_op_kind_t kind)
{
    const uint32_t now = flash_op_now_ms();
    const uint32_t took = (now >= s_flash_op_begin_ms) ? (now - s_flash_op_begin_ms) : 0u;

    if (s_flash_op_depth > 0u) {
        s_flash_op_depth--;
    }
    s_flash_op_last_end_ms = now;
    s_flash_op_last_ms = took;

    if (kind >= FLASH_OP_KIND_COUNT) {
        return;
    }

    flash_op_stat_t *st = &s_flash_op[kind];
    st->count++;
    st->total_ms += took;
    if (took > st->max_ms) {
        st->max_ms = took;
    }
    if (took >= FLASH_OP_SLOW_MS) {
        st->slow++;
    }
}

bool storage_guard_flash_op_probe(uint32_t now_ms, uint32_t within_ms, uint32_t *out_kind, uint32_t *out_ms)
{
    const uint32_t depth = s_flash_op_depth;
    if (out_kind != NULL) {
        *out_kind = s_flash_op_last_kind;
    }

    if (depth > 0u) {
        const uint32_t begin = s_flash_op_begin_ms;
        if (out_ms != NULL) {
            *out_ms = (now_ms >= begin) ? (now_ms - begin) : 0u;
        }
        return true;
    }

    const uint32_t end = s_flash_op_last_end_ms;
    if (end != 0u && now_ms >= end && (now_ms - end) <= within_ms) {
        if (out_ms != NULL) {
            *out_ms = s_flash_op_last_ms;
        }
        return true;
    }

    if (out_ms != NULL) {
        *out_ms = 0u;
    }
    return false;
}

uint32_t storage_guard_flash_op_count(uint32_t kind)
{
    return (kind < FLASH_OP_KIND_COUNT) ? s_flash_op[kind].count : 0u;
}

uint32_t storage_guard_flash_op_total_ms(uint32_t kind)
{
    return (kind < FLASH_OP_KIND_COUNT) ? s_flash_op[kind].total_ms : 0u;
}

uint32_t storage_guard_flash_op_max_ms(uint32_t kind)
{
    return (kind < FLASH_OP_KIND_COUNT) ? s_flash_op[kind].max_ms : 0u;
}

uint32_t storage_guard_flash_op_slow(uint32_t kind)
{
    return (kind < FLASH_OP_KIND_COUNT) ? s_flash_op[kind].slow : 0u;
}

const char *storage_guard_flash_op_kind_name(uint32_t kind)
{
    return (kind < FLASH_OP_KIND_COUNT) ? k_flash_op_name[kind] : "none";
}
