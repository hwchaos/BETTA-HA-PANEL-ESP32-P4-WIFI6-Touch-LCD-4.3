/* SPDX-License-Identifier: LicenseRef-FNCL-1.1
 * Copyright (c) 2026 Cpt_Kirk
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "app_config.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char name[APP_SD_MAX_NAME_LEN];
    uint64_t size;
    bool is_dir;
} sd_dir_entry_t;

typedef struct {
    bool supported;
    bool enabled;
    bool mounted;
    /* A card that answers on the bus even though it carries no filesystem the
     * panel can use: the difference between "insert a card" and "format this
     * card". */
    bool detected;
    char card_name[APP_SD_CARD_NAME_LEN];
    /* Volume found on a card the panel cannot mount ("exFAT", "NTFS", "FAT"
     * when the volume is broken, empty when there is none). */
    char fs_name[APP_SD_FS_NAME_LEN];
    uint64_t capacity_bytes;
    uint64_t free_bytes;
    /* Logical sector size reported by the card (0 when unknown). */
    uint32_t sector_bytes;
    /* Last failure of a mount attempt (0 when the last one worked). */
    int mount_error;
} sd_card_info_t;

/* Card lifecycle events, delivered from the task that changed the state. */
typedef enum {
    /* The card is mounted and ready for reads and writes. */
    SD_CARD_EVENT_MOUNTED = 0,
    /* The card is still mounted but is about to be unmounted or erased: the
     * last moment at which its content can be read or saved elsewhere. */
    SD_CARD_EVENT_RELEASING,
    /* The card is no longer available. */
    SD_CARD_EVENT_UNMOUNTED,
} sd_card_event_t;

typedef void (*sd_card_event_cb_t)(sd_card_event_t event);

/* Mounts the card when the stored setting allows it and creates the standard
 * folder layout.  Safe to call on boards without a socket (no-op).
 * Must run after display_init(): the panel init sequence shares IO47/IO48 with
 * the SPI bus and only releases them once the display is up. */
esp_err_t sd_card_init(void);

/* Mount / unmount the card without touching the persisted setting. */
esp_err_t sd_card_mount(void);
esp_err_t sd_card_unmount(void);

/* Persists the new value and mounts/unmounts accordingly. */
esp_err_t sd_card_set_enabled(bool enabled);

/* Suspends (or resumes) the hot-plug retry loop.  While paused the task stops
 * probing the SPI bus, which keeps flash access quiet during an OTA write and
 * silences the "No usable microSD card yet" log spam.  A mounted card is
 * untouched. */
void sd_card_set_hotplug_paused(bool paused);

/* Makes the card usable: mounts it and, when it is present but carries no FAT
 * filesystem, creates one (which erases everything on the card).  The card
 * stays mounted. */
esp_err_t sd_card_format(void);

bool sd_card_is_mounted(void);

/* True when a card answers on the SPI bus, even if it has no usable
 * filesystem yet. */
bool sd_card_is_detected(void);

void sd_card_set_event_callback(sd_card_event_cb_t cb);

/* Fills *out. */
esp_err_t sd_card_get_info(sd_card_info_t *out);

/* Lists one directory of the card.  `dir` is a path relative to the mount
 * point ("" = root).  Returns at most max_entries names. */
esp_err_t sd_card_list(const char *dir, sd_dir_entry_t *entries, size_t max_entries, size_t *out_count);

/* Rejects absolute paths and "..", then builds the VFS path of a card file.
 * Returns false when the relative name is not acceptable. */
bool sd_card_build_path(const char *rel, char *out, size_t out_len);

/* Deletes a single file (directories are refused). */
esp_err_t sd_card_remove(const char *rel);

#ifdef __cplusplus
}
#endif
