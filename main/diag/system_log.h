/* SPDX-License-Identifier: LicenseRef-FNCL-1.1
 * Copyright (c) 2026 Cpt_Kirk
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Initialise the persistent system log:
 *  - installs an esp_log vprintf hook that captures lines up to the configured
 *    verbosity (see system_log_set_verbosity(); INFO by default),
 *  - starts a low-priority task that flushes the capture ring to LittleFS,
 *  - writes a boot header (version + reset reason) and periodic heap/heartbeat
 *    snapshots so freezes, panics and memory-exhaustion trends survive a reboot.
 *
 * Must be called after the LittleFS filesystem is mounted. */
esp_err_t system_log_init(void);

/* Flush any pending buffered log lines to storage (best effort). */
esp_err_t system_log_flush(void);

/* Read the tail of the active log file into buf (NUL-terminated).
 * Returns the number of bytes copied (excluding the NUL), or 0 if empty. */
int system_log_read_tail(char *buf, size_t buf_len);

/* Read one log segment the same way: rot 0 is the active file, rot 1..N the
 * rotated history with higher numbers being older.  Returns the number of bytes
 * copied (excluding the NUL), or -1 when that segment does not exist. */
int system_log_read_segment(int rot, char *buf, size_t buf_len);

/* Delete the active log file and all rotated generations, then write a fresh
 * heartbeat line so the log viewer is not left empty. */
esp_err_t system_log_clear(void);

/* Suspend/resume the UI heartbeat watchdog around a deliberately long
 * maintenance operation (microSD format, wallpaper transfer).  While a
 * suspension is active the stale-heartbeat restart is suppressed, because the
 * operation itself may block the UI task.  The suspension is counted, so
 * nested or overlapping operations are supported, and it expires on its own
 * after APP_LOG_WATCHDOG_PAUSE_MAX_MS so a genuinely hung operation does not
 * disable the watchdog forever. */
void system_log_watchdog_suspend(void);
void system_log_watchdog_resume(void);

/* Log-file appends held back in RAM while background flash writers are paused
 * (see diag/storage_guard.h: storage_guard_flash_writers_pause()).  The lines
 * are not lost - they stay in the capture ring and reach the file as soon as
 * the window closes - but the counter is what proves the window actually
 * covered the MMU reprogramming inside esp_ota_end(). */
uint32_t system_log_gated_writes(void);
void system_log_gated_writes_reset(void);

/* Explicitly record a diagnostic error line. Use from error paths that would
 * otherwise fail silently. */
void system_log_write(const char *tag, const char *fmt, ...);
/* Same as system_log_write(), but stored as an informational line so it does
 * not show up red in the log viewer. The automatic capture hook only forwards
 * lines up to the configured verbosity, so INFO diagnostics have to be written
 * explicitly. */
void system_log_write_info(const char *tag, const char *fmt, ...);

/* Log verbosity, 0..5, matching the numbers used by the WebUI selector:
 *   0 = off, 1 = errors, 2 = +warnings, 3 = +info (default), 4 = +debug,
 *   5 = +verbose ("everything": every component at ESP_LOG_VERBOSE).
 *
 * Changing it does two things:
 *  - sets the runtime level of every esp_log tag (ESP_LOG_* filtering), and
 *  - sets the level up to which the capture hook copies lines into the
 *    persistent ring, so the log file grows/shrinks with the setting.
 *
 * Lines above the *compiled-in* maximum (CONFIG_LOG_MAXIMUM_LEVEL) can never
 * appear - the macros are removed at compile time.  This build compiles
 * VERBOSE in, so level 5 really does capture everything the firmware emits. */
#define SYSTEM_LOG_VERBOSITY_OFF     0
#define SYSTEM_LOG_VERBOSITY_ERROR   1
#define SYSTEM_LOG_VERBOSITY_WARN    2
#define SYSTEM_LOG_VERBOSITY_INFO    3
#define SYSTEM_LOG_VERBOSITY_DEBUG   4
#define SYSTEM_LOG_VERBOSITY_VERBOSE 5

void system_log_set_verbosity(int level);
int system_log_get_verbosity(void);

/* Record a notable UI/display/network event with a timestamp, e.g. a page
 * switch, a backlight change or a screenshot request.  The line is written as
 * information (visible from verbosity 3) and is also handed to the screen
 * flash detector, so a flash observed seconds later can be attributed to the
 * event that caused it. */
void system_log_event(const char *tag, const char *fmt, ...);

/* Freeze forensics: the UI task publishes the phase it is currently executing
 * and the HTTP layer publishes the request it is serving, so the watchdog can
 * name the guilty code path in the persistent log. Both are lock-free and
 * cheap enough to call on every pass of the UI loop. Pass NULL to clear the
 * HTTP marker (no request in flight). The stage strings must stay valid
 * forever - string literals are expected. */
void system_log_note_stage(const char *stage);
void system_log_note_http(const char *uri);

/* LVGL forensics: every lv_timer callback announces itself on entry (name must
 * be a string literal) and the port task reports that lv_timer_handler() ran.
 * When the UI heartbeat stops, the freeze line then shows whether LVGL is still
 * being serviced at all (lvhand=) and which callback was entered last (lvcb=),
 * which distinguishes a slow drawing pass from a stalled port task. */
void system_log_note_lvgl_cb(const char *name);
void system_log_lvgl_handler_beat(void);

/* Lock forensics: whoever acquires/releases a tracked resource registers
 * itself, so the freeze dump can name the task that is sitting on it.  Both
 * calls are cheap (a handful of stores), so they can wrap hot paths.
 * lock_name must be a string literal that stays valid forever. */
void system_log_lock_acquire(const char *lock_name);
void system_log_lock_release(const char *lock_name);
/* Renders the current holder of lock_name as "<task>@<held ms>ms/d<depth>" or
 * "free" when nobody holds it.  Returns true when the lock is held. */
bool system_log_lock_owner(const char *lock_name, char *out, size_t out_len);
/* Worst-case hold time across all tracked locks, formatted as
 * "<name>:<ms>/<slow count>" or "none" when nothing was measured yet. */
void system_log_lock_worst(char *out, size_t out_len);

#ifdef __cplusplus
}
#endif
