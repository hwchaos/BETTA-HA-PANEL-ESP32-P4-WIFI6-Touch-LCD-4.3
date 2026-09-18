/* SPDX-License-Identifier: LicenseRef-FNCL-1.1
 * Copyright (c) 2026 Cpt_Kirk
 *
 * microSD card on the ESP32-4848S040 (panels3) and the 7" Waveshare
 * ESP32-P4-WIFI6-Touch-LCD-7B ("panel7") boards.
 *
 * On panels3 the TF socket is wired to an SPI bus (IO48 SCK, IO47 MOSI, IO41
 * MISO, IO42 CS) as documented by the manufacturer demo, and IO47/IO48 are
 * shared with the bit-banged ST7701 init sequence.  `display_init()` asks the
 * RGB panel driver to delete its IO descriptors after init
 * (enable_io_multiplex = 1), so the card can only be mounted afterwards; that
 * is why sd_card_init() is called from app_main right after display_init()
 * returned.  SPI2 is used because the panel class does not use it for
 * anything else.
 *
 * On panel7 the socket hangs off the SDMMC peripheral instead: 4-bit slot 0 on
 * the IO_MUX pins, with the slot rail enabled through on-chip LDO channel 4
 * (the same wiring the board BSP drives).  There is no pin sharing, so mount
 * order does not matter; the card is addressed through SDMMC_HOST_DEFAULT()
 * / SDMMC_SLOT_CONFIG_DEFAULT() and the rest of this file is bus agnostic.
 */
#include "sd/sd_card.h"

#include <dirent.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <string.h>

#if !APP_SD_USE_SDMMC
#include "driver/sdspi_host.h"
#include "driver/spi_common.h"
#else
#include "driver/sdmmc_host.h"
#include "sd_pwr_ctrl_by_on_chip_ldo.h"
#endif
#include "esp_check.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sdmmc_cmd.h"

#include "sd/sd_logs.h"
#include "diag/storage_guard.h"
#include "diag/system_log.h"
#include "settings/runtime_settings.h"
#include "util/log_tags.h"

#if APP_SD_SUPPORTED

static const char *TAG = TAG_SD;

static sdmmc_card_t *s_card;
static bool s_mounted;
static bool s_enabled = true;
static bool s_detected;
static char s_card_name[APP_SD_CARD_NAME_LEN];
static char s_fs_name[APP_SD_FS_NAME_LEN];
static uint32_t s_sector_bytes;
static esp_err_t s_mount_err = ESP_OK;
static sd_card_event_cb_t s_event_cb;
static TaskHandle_t s_auto_task;
static SemaphoreHandle_t s_lock;
static volatile bool s_hotplug_paused;

/* Formatting is destructive and can take tens of seconds on a large card, so a
 * second request (a double click, two browser tabs, a retry) must not queue up
 * behind the first one. */
static portMUX_TYPE s_maint_mux = portMUX_INITIALIZER_UNLOCKED;
static bool s_format_active;

static bool format_begin(void)
{
    bool started = false;
    portENTER_CRITICAL(&s_maint_mux);
    if (!s_format_active) {
        s_format_active = true;
        started = true;
    }
    portEXIT_CRITICAL(&s_maint_mux);
    return started;
}

static void format_finish(void)
{
    portENTER_CRITICAL(&s_maint_mux);
    s_format_active = false;
    portEXIT_CRITICAL(&s_maint_mux);
}

static void notify_mount_event(sd_card_event_t event)
{
    if (s_event_cb != NULL) {
        s_event_cb(event);
    }
}

static bool lock_take(void)
{
    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutex();
        if (s_lock == NULL) {
            return false;
        }
    }
    return xSemaphoreTake(s_lock, pdMS_TO_TICKS(5000)) == pdTRUE;
}

static void lock_give(void)
{
    if (s_lock != NULL) {
        xSemaphoreGive(s_lock);
    }
}

static void ensure_dir(const char *rel)
{
    char path[APP_SD_MAX_PATH_LEN];
    if (!sd_card_build_path(rel, path, sizeof(path))) {
        return;
    }
    if (mkdir(path, 0777) != 0) {
        struct stat st;
        if (stat(path, &st) != 0 || !S_ISDIR(st.st_mode)) {
            ESP_LOGW(TAG, "Cannot create %s", path);
        }
    }
}

bool sd_card_build_path(const char *rel, char *out, size_t out_len)
{
    if (out == NULL || out_len <= strlen(APP_SD_MOUNT_POINT) + 2U) {
        return false;
    }
    out[0] = '\0';
    if (rel == NULL) {
        rel = "";
    }

    while (rel[0] == '/') {
        rel++;
    }
    if (strlen(rel) >= out_len - strlen(APP_SD_MOUNT_POINT) - 1U) {
        return false;
    }

    for (const char *cursor = rel; cursor[0] != '\0'; cursor++) {
        const char c = cursor[0];
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                        c == '_' || c == '-' || c == '.' || c == ' ' || c == '/';
        if (!ok) {
            return false;
        }
    }
    /* ".." would escape the mount point. */
    if (strstr(rel, "..") != NULL) {
        return false;
    }

    if (rel[0] == '\0') {
        snprintf(out, out_len, "%s", APP_SD_MOUNT_POINT);
    } else {
        snprintf(out, out_len, APP_SD_MOUNT_POINT "/%s", rel);
    }
    return true;
}

#if APP_SD_USE_SDMMC

/* The slot rail is controlled through LDO channel 4.  The handle is created
 * once and kept for the lifetime of the panel: the hot-plug task below retries
 * a failed mount indefinitely, and creating the handle on every attempt would
 * leak it.  The slot itself is initialised here so that probing a card that
 * carries no filesystem (`probe_card()`) can run without the FAT layer. */
static sd_pwr_ctrl_handle_t s_sd_pwr;

static sdmmc_slot_config_t slot_config(void)
{
    sdmmc_slot_config_t cfg = SDMMC_SLOT_CONFIG_DEFAULT();
    /* Only DAT0..DAT3 are wired; the default (0) would ask for the widest bus
     * the slot supports. */
    cfg.width = 4;
    return cfg;
}

static esp_err_t bus_acquire(void)
{
    if (s_sd_pwr == NULL) {
        sd_pwr_ctrl_ldo_config_t ldo_cfg = { .ldo_chan_id = APP_SD_LDO_CHAN };
        esp_err_t err = sd_pwr_ctrl_new_on_chip_ldo(&ldo_cfg, &s_sd_pwr);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "SD power rail init failed: %s", esp_err_to_name(err));
            return err;
        }
    }

    esp_err_t err = sdmmc_host_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "SDMMC host init failed: %s", esp_err_to_name(err));
        return err;
    }

    sdmmc_slot_config_t slot_cfg = slot_config();
    err = sdmmc_host_init_slot(APP_SD_SDMMC_SLOT, &slot_cfg);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "SDMMC slot init failed: %s", esp_err_to_name(err));
    }
    return err;
}

static void bus_release(void)
{
    /* esp_vfs_fat_sdcard_unmount() tears the slot down, and a failed mount has
     * already done so on its own cleanup path.  Nothing to undo here: keeping
     * the rail powered is what lets the next attempt probe the card again. */
}

#else /* !APP_SD_USE_SDMMC */

/* The TF socket shares IO47/IO48 with the display init, so the bus is created
 * here and torn down again on every failed attempt to leave the pins idle. */
static esp_err_t bus_acquire(void)
{
    spi_bus_config_t bus_cfg = {
        .mosi_io_num = APP_SD_PIN_MOSI,
        .miso_io_num = APP_SD_PIN_MISO,
        .sclk_io_num = APP_SD_PIN_SCK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 4000,
    };
    esp_err_t err = spi_bus_initialize((spi_host_device_t)APP_SD_SPI_HOST, &bus_cfg, SDSPI_DEFAULT_DMA);
    if (err == ESP_ERR_INVALID_STATE) {
        return ESP_OK;
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "SPI bus init failed: %s", esp_err_to_name(err));
    }
    return err;
}

static void bus_release(void)
{
    spi_bus_free((spi_host_device_t)APP_SD_SPI_HOST);
}

#endif /* APP_SD_USE_SDMMC */

/* FatFS is built without exFAT/LBA64, so an exFAT volume looks exactly like an
 * unformatted card to the panel.  The OEM name of the first sector says which
 * of the two it is, and that is the difference between "format this card" and
 * "this card cannot be used at all". */
static void detect_fs_label(sdmmc_card_t *card)
{
    const uint32_t sector_size = card->csd.sector_size;
    if (sector_size < 512U || sector_size > 4096U) {
        return;
    }

    uint8_t *sector = malloc(sector_size);
    if (sector == NULL) {
        return;
    }

    s_fs_name[0] = '\0';
    if (sdmmc_read_sectors(card, sector, 0, 1) == ESP_OK) {
        if (memcmp(sector + 3, "EXFAT   ", 8) == 0) {
            strlcpy(s_fs_name, "exFAT", sizeof(s_fs_name));
        } else if (memcmp(sector + 3, "NTFS    ", 8) == 0) {
            strlcpy(s_fs_name, "NTFS", sizeof(s_fs_name));
        } else if (sector[510] == 0x55U && sector[511] == 0xAAU) {
            strlcpy(s_fs_name, "FAT", sizeof(s_fs_name));
        }
    }
    free(sector);
}

/* A card that answers on the bus but carries no FAT filesystem fails the mount
 * with the very same error as unplugged wiring, so the panel could not tell the
 * user "insert a card" from "format this card".  This bare sdmmc_card_init()
 * runs at the slow probing clock and only tells the two apart; it also reports
 * the logical sector size shown in the diagnostics. */
static void probe_card(void)
{
#if APP_SD_USE_SDMMC
    if (bus_acquire() != ESP_OK) {
        return;
    }

    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.slot = APP_SD_SDMMC_SLOT;
    host.max_freq_khz = SDMMC_FREQ_PROBING / 1000;
    host.pwr_ctrl_handle = s_sd_pwr;

    sdmmc_card_t *card = calloc(1, sizeof(sdmmc_card_t));
    if (card != NULL) {
        if (sdmmc_card_init(&host, card) == ESP_OK) {
            s_detected = true;
            s_sector_bytes = card->csd.sector_size;
            snprintf(s_card_name, sizeof(s_card_name), "%s", card->cid.name);
            detect_fs_label(card);
            ESP_LOGI(TAG, "Card present but not usable yet: %s %llu MB, %u B sectors, %s",
                s_card_name,
                (unsigned long long)((uint64_t)card->csd.capacity * card->csd.sector_size / (1024ULL * 1024ULL)),
                (unsigned)s_sector_bytes,
                s_fs_name[0] != '\0' ? s_fs_name : "no filesystem");
        }
        free(card);
    }
#else
    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = APP_SD_SPI_HOST;
    host.max_freq_khz = SDMMC_FREQ_PROBING / 1000;

    if (bus_acquire() != ESP_OK) {
        return;
    }

    sdspi_device_config_t slot_cfg = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_cfg.gpio_cs = (gpio_num_t)APP_SD_PIN_CS;
    slot_cfg.host_id = (spi_host_device_t)APP_SD_SPI_HOST;

    sdspi_dev_handle_t handle = 0;
    if (sdspi_host_init_device(&slot_cfg, &handle) == ESP_OK) {
        sdmmc_card_t *card = calloc(1, sizeof(sdmmc_card_t));
        if (card != NULL) {
            if (sdmmc_card_init(&host, card) == ESP_OK) {
                s_detected = true;
                s_sector_bytes = card->csd.sector_size;
                snprintf(s_card_name, sizeof(s_card_name), "%s", card->cid.name);
                detect_fs_label(card);
                ESP_LOGI(TAG, "Card present but not usable yet: %s %llu MB, %u B sectors, %s",
                    s_card_name,
                    (unsigned long long)((uint64_t)card->csd.capacity * card->csd.sector_size / (1024ULL * 1024ULL)),
                    (unsigned)s_sector_bytes,
                    s_fs_name[0] != '\0' ? s_fs_name : "no filesystem");
            }
            free(card);
        }
        sdspi_host_remove_device(handle);
    }
#endif
    bus_release();
}

static esp_err_t do_mount(bool format_if_mount_failed)
{
    if (s_mounted) {
        return ESP_OK;
    }

    esp_err_t err = bus_acquire();
    if (err != ESP_OK) {
        s_mount_err = err;
        return err;
    }

#if APP_SD_USE_SDMMC
    /* bus_acquire() has created the power handle by now, so the host can
     * reference it (the card init sequence switches the IO rail through it). */
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.slot = APP_SD_SDMMC_SLOT;
    host.max_freq_khz = APP_SD_FREQ_HZ / 1000;
    host.pwr_ctrl_handle = s_sd_pwr;

    sdmmc_slot_config_t slot_cfg = slot_config();
#else
    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = APP_SD_SPI_HOST;
    host.max_freq_khz = APP_SD_FREQ_HZ / 1000;

    sdspi_device_config_t slot_cfg = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_cfg.gpio_cs = (gpio_num_t)APP_SD_PIN_CS;
    slot_cfg.host_id = (spi_host_device_t)APP_SD_SPI_HOST;
#endif

    esp_vfs_fat_sdmmc_mount_config_t mount_cfg = {
        .format_if_mount_failed = format_if_mount_failed,
        .max_files = APP_SD_MAX_FILES,
        .allocation_unit_size = 16 * 1024,
        .disk_status_check_enable = false,
        .use_one_fat = false,
    };

#if APP_SD_USE_SDMMC
    err = esp_vfs_fat_sdmmc_mount(APP_SD_MOUNT_POINT, &host, &slot_cfg, &mount_cfg, &s_card);
#else
    err = esp_vfs_fat_sdspi_mount(APP_SD_MOUNT_POINT, &host, &slot_cfg, &mount_cfg, &s_card);
#endif
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Card mount failed: %s", esp_err_to_name(err));
        s_mount_err = err;
        s_card = NULL;
        bus_release();
        probe_card();
        return err;
    }

    s_mounted = true;
    s_detected = true;
    s_sector_bytes = s_card->csd.sector_size;
    s_mount_err = ESP_OK;
    s_fs_name[0] = '\0';
    snprintf(s_card_name, sizeof(s_card_name), "%s", s_card->cid.name);
    ESP_LOGI(TAG, "Card mounted: %s %llu MB", s_card_name,
        (unsigned long long)((uint64_t)s_card->csd.capacity * s_card->csd.sector_size / (1024ULL * 1024ULL)));

    ensure_dir(APP_SD_LOG_DIR);
    ensure_dir(APP_SD_PHOTO_DIR);
    return ESP_OK;
}

esp_err_t sd_card_mount(void)
{
    if (!lock_take()) {
        return ESP_ERR_TIMEOUT;
    }
    esp_err_t err = do_mount(false);
    lock_give();

    if (err == ESP_OK) {
        sd_logs_start();
        notify_mount_event(SD_CARD_EVENT_MOUNTED);
    }
    return err;
}

esp_err_t sd_card_unmount(void)
{
    if (!s_mounted) {
        return ESP_OK;
    }

    /* Last moment at which the card can still be read: lets the wallpaper cache
     * move its copy back to internal flash. */
    notify_mount_event(SD_CARD_EVENT_RELEASING);

    if (!lock_take()) {
        return ESP_ERR_TIMEOUT;
    }

    esp_err_t err = ESP_OK;
    bool was_mounted = s_mounted;
    if (s_mounted) {
        err = esp_vfs_fat_sdcard_unmount(APP_SD_MOUNT_POINT, s_card);
        s_card = NULL;
        s_mounted = false;
        s_detected = false;
        s_sector_bytes = 0;
        s_card_name[0] = '\0';
        s_fs_name[0] = '\0';
#if !APP_SD_USE_SDMMC
        if (spi_bus_free((spi_host_device_t)APP_SD_SPI_HOST) != ESP_OK) {
            ESP_LOGD(TAG, "SPI bus already released");
        }
#endif
        ESP_LOGI(TAG, "Card unmounted");
    }

    lock_give();

    if (was_mounted) {
        notify_mount_event(SD_CARD_EVENT_UNMOUNTED);
    }
    return err;
}

esp_err_t sd_card_set_enabled(bool enabled)
{
    runtime_settings_t settings;
    esp_err_t err = runtime_settings_load(&settings);
    if (err != ESP_OK) {
        return err;
    }
    if (settings.sd_enabled != enabled) {
        settings.sd_enabled = enabled;
        err = runtime_settings_save(&settings);
        if (err != ESP_OK) {
            return err;
        }
    }
    s_enabled = enabled;

    return enabled ? sd_card_mount() : sd_card_unmount();
}

void sd_card_set_hotplug_paused(bool paused)
{
    if (s_hotplug_paused == paused) {
        return;
    }
    s_hotplug_paused = paused;
    ESP_LOGI(TAG, "microSD hot-plug %s", paused ? "paused (OTA)" : "resumed");
}

esp_err_t sd_card_format(void)
{
    if (!format_begin()) {
        ESP_LOGW(TAG, "Format already in progress; request ignored");
        return ESP_ERR_INVALID_STATE;
    }

    /* Keep both watchdogs quiet for the whole destructive operation: it can
     * take tens of seconds, it runs the storage callback synchronously and it
     * parks the idle tasks far longer than the task watchdog tolerates. */
    storage_guard_begin("sd-format");

    /* Formatting erases the card, so anything kept on it has to be salvaged
     * before the first sector is rewritten. */
    notify_mount_event(SD_CARD_EVENT_RELEASING);

    esp_err_t err = ESP_OK;
    if (!lock_take()) {
        err = ESP_ERR_TIMEOUT;
    } else {
        /* A card without a usable filesystem cannot be mounted, and the format call
         * below needs a mounted card.  The first attempt therefore runs without
         * touching the card; only when it fails does the second one let FatFS
         * create a fresh FAT32 volume instead of giving up. */
        err = do_mount(false);
        if (s_mounted) {
            ESP_LOGW(TAG, "Formatting card");
            err = esp_vfs_fat_sdcard_format(APP_SD_MOUNT_POINT, s_card);
            if (err == ESP_OK) {
                ensure_dir(APP_SD_LOG_DIR);
                ensure_dir(APP_SD_PHOTO_DIR);
                system_log_write_info(TAG_SD, "microSD card formatted from the web UI");
            } else {
                ESP_LOGE(TAG, "Format failed: %s", esp_err_to_name(err));
            }
        } else {
            ESP_LOGW(TAG, "Card has no usable filesystem, creating one");
            err = do_mount(true);
            if (err == ESP_OK) {
                system_log_write_info(TAG_SD, "microSD card formatted from the web UI (new filesystem)");
            }
        }

        lock_give();
    }

    if (err == ESP_OK) {
        sd_logs_start();
        notify_mount_event(SD_CARD_EVENT_MOUNTED);
    }

    storage_guard_end();
    format_finish();
    return err;
}

bool sd_card_is_mounted(void)
{
    return s_mounted;
}

bool sd_card_is_detected(void)
{
    return s_detected;
}

void sd_card_set_event_callback(sd_card_event_cb_t cb)
{
    s_event_cb = cb;
}

/* The socket has no card-detect line, so a card inserted while the panel is
 * running is only noticed by trying again.  The retry period starts short (the
 * user is probably standing at the panel) and then grows, so an empty slot
 * stops costing a mount attempt every few seconds for the whole uptime. */
static uint32_t auto_mount_delay_ms(unsigned attempts)
{
    if (attempts < 5U) {
        return 3000U;
    }
    if (attempts < 20U) {
        return 15000U;
    }
    return 60000U;
}

static void auto_mount_task(void *arg)
{
    (void)arg;
    unsigned attempts = 0;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(auto_mount_delay_ms(attempts)));

        if (s_hotplug_paused) {
            attempts = 0;
            continue;
        }

        if (s_mounted || !s_enabled) {
            attempts = 0;
            continue;
        }

        if (sd_card_mount() == ESP_OK) {
            system_log_write_info(TAG_SD, "microSD card mounted after insertion");
            attempts = 0;
        } else {
            attempts++;
            if (attempts <= 3U) {
                ESP_LOGW(TAG, "No usable microSD card yet (attempt %u)", attempts);
            }
        }
    }
}

static void start_auto_mount_task(void)
{
    if (s_auto_task != NULL) {
        return;
    }
    if (xTaskCreate(auto_mount_task, "sd_auto", 5120, NULL, 3, &s_auto_task) != pdPASS) {
        ESP_LOGW(TAG, "Cannot start the microSD hot-plug task");
        s_auto_task = NULL;
    }
}

esp_err_t sd_card_get_info(sd_card_info_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(out, 0, sizeof(*out));
    out->supported = true;

    runtime_settings_t settings;
    if (runtime_settings_load(&settings) == ESP_OK) {
        out->enabled = settings.sd_enabled;
    } else {
        out->enabled = true;
    }

    out->mounted = s_mounted;
    out->detected = s_detected;
    out->sector_bytes = s_sector_bytes;
    out->mount_error = (int)s_mount_err;
    if (!s_mounted || s_card == NULL) {
        /* The probe may still have identified a card that carries no usable
         * filesystem; the web UI offers to format it in that case. */
        snprintf(out->card_name, sizeof(out->card_name), "%s", s_card_name);
        strlcpy(out->fs_name, s_fs_name, sizeof(out->fs_name));
        return ESP_OK;
    }

    snprintf(out->card_name, sizeof(out->card_name), "%s", s_card_name);
    out->capacity_bytes = (uint64_t)s_card->csd.capacity * s_card->csd.sector_size;

    uint64_t total = 0;
    uint64_t free_bytes = 0;
    if (esp_vfs_fat_info(APP_SD_MOUNT_POINT, &total, &free_bytes) == ESP_OK) {
        if (total > 0) {
            out->capacity_bytes = total;
        }
        out->free_bytes = free_bytes;
    }
    return ESP_OK;
}

esp_err_t sd_card_list(const char *dir, sd_dir_entry_t *entries, size_t max_entries, size_t *out_count)
{
    if (entries == NULL || out_count == NULL || max_entries == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_count = 0;
    if (!s_mounted) {
        return ESP_ERR_INVALID_STATE;
    }

    char path[APP_SD_MAX_PATH_LEN];
    if (!sd_card_build_path(dir, path, sizeof(path))) {
        return ESP_ERR_INVALID_ARG;
    }

    DIR *handle = opendir(path);
    if (handle == NULL) {
        return ESP_ERR_NOT_FOUND;
    }

    size_t count = 0;
    struct dirent *entry = NULL;
    while (count < max_entries && (entry = readdir(handle)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }

        sd_dir_entry_t *slot = &entries[count];
        memset(slot, 0, sizeof(*slot));
        strlcpy(slot->name, entry->d_name, sizeof(slot->name));

        char child[APP_SD_MAX_PATH_LEN];
        strlcpy(child, path, sizeof(child));
        strlcat(child, "/", sizeof(child));
        strlcat(child, entry->d_name, sizeof(child));
        struct stat st;
        if (stat(child, &st) == 0) {
            slot->is_dir = S_ISDIR(st.st_mode);
            slot->size = slot->is_dir ? 0 : (uint64_t)st.st_size;
        }
        count++;
    }
    closedir(handle);

    *out_count = count;
    return ESP_OK;
}

esp_err_t sd_card_remove(const char *rel)
{
    if (!s_mounted) {
        return ESP_ERR_INVALID_STATE;
    }

    char path[APP_SD_MAX_PATH_LEN];
    if (!sd_card_build_path(rel, path, sizeof(path))) {
        return ESP_ERR_INVALID_ARG;
    }

    struct stat st;
    if (stat(path, &st) != 0) {
        return ESP_ERR_NOT_FOUND;
    }
    if (S_ISDIR(st.st_mode)) {
        return ESP_ERR_INVALID_ARG;
    }

    return remove(path) == 0 ? ESP_OK : ESP_FAIL;
}

esp_err_t sd_card_init(void)
{
    runtime_settings_t settings;
    if (runtime_settings_load(&settings) != ESP_OK || !settings.sd_enabled) {
        s_enabled = false;
        ESP_LOGI(TAG, "microSD support disabled");
        start_auto_mount_task();
        return ESP_OK;
    }
    s_enabled = true;

    esp_err_t err = do_mount(false);
    if (err != ESP_OK) {
        /* A missing card is not a boot failure: the panel keeps working and the
         * hot-plug task picks the card up as soon as it is inserted. */
        system_log_write_info(TAG_SD, "microSD card not mounted (%s)", esp_err_to_name(err));
    } else {
        sd_logs_start();
    }

    start_auto_mount_task();
    return ESP_OK;
}

#else /* !APP_SD_SUPPORTED */

esp_err_t sd_card_init(void)
{
    return ESP_OK;
}

esp_err_t sd_card_mount(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t sd_card_unmount(void)
{
    return ESP_OK;
}

esp_err_t sd_card_set_enabled(bool enabled)
{
    (void)enabled;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t sd_card_format(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

bool sd_card_is_mounted(void)
{
    return false;
}

bool sd_card_is_detected(void)
{
    return false;
}

void sd_card_set_event_callback(sd_card_event_cb_t cb)
{
    (void)cb;
}

esp_err_t sd_card_get_info(sd_card_info_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));
    return ESP_OK;
}

esp_err_t sd_card_list(const char *dir, sd_dir_entry_t *entries, size_t max_entries, size_t *out_count)
{
    (void)dir;
    (void)entries;
    (void)max_entries;
    if (out_count != NULL) {
        *out_count = 0;
    }
    return ESP_ERR_NOT_SUPPORTED;
}

bool sd_card_build_path(const char *rel, char *out, size_t out_len)
{
    (void)rel;
    if (out != NULL && out_len > 0) {
        out[0] = '\0';
    }
    return false;
}

esp_err_t sd_card_remove(const char *rel)
{
    (void)rel;
    return ESP_ERR_NOT_SUPPORTED;
}

#endif /* APP_SD_SUPPORTED */
