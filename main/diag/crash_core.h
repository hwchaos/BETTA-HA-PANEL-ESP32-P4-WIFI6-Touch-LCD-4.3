/*
 * SPDX-License-Identifier: LicenseRef-FNCL-1.1
 * Copyright (c) 2026 Cpt_Kirk
 *
 * Crash forensics for the panel app.
 *
 * The panel reboots spontaneously with reset=PANIC(4) and the only trace is the
 * light-blue flash of the re-initialising display: the panic backtrace goes to
 * the UART console, which nobody is watching, while the persistent ring log
 * keeps nothing but the reset reason.  With CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH
 * the panic handler writes the failing task's TCB, registers and stack into the
 * `coredump` partition; this module decodes that dump on the next boot so the
 * crash is visible from /api/logs (and /api/diagnostics) alone, and serves the
 * raw ELF dump over HTTP for `idf.py coredump-info` when a full backtrace with
 * source lines is needed.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

typedef struct {
    bool present;        /* a valid core dump is stored in flash */
    bool app_match;      /* the dump was produced by the running app image */
    bool partition_ok;   /* the `coredump` partition exists in this build */
    size_t size;         /* dump size in bytes (0 when unknown) */
    esp_err_t check_err; /* result of esp_core_dump_image_check() */
    char task[24];       /* name of the task that panicked */
    char app_sha[17];    /* first 16 hex chars of the app SHA256 in the dump */
    char reason[192];    /* panic reason recorded by the panic handler */
    const char *cause_str; /* decoded fault cause ("Load access fault", ...) */
    uint32_t pc;         /* program counter at the fault */
    uint32_t cause;      /* RISC-V mcause / Xtensa EXCCAUSE */
    uint32_t fault_addr; /* RISC-V mtval / Xtensa EXCVADDR */
    uint32_t ra;         /* leaf return address (RISC-V only) */
    uint32_t sp;         /* stack pointer at the fault (RISC-V only) */
} crash_core_info_t;

/* Reads (once) whatever the previous boot left in the core dump partition and
 * logs a summary through the persistent log.  Safe to call from any task after
 * system_log_init(); no-op when core dump support is disabled in this build. */
void crash_core_init(void);

/* Summary of the last decoded dump; never NULL. */
const crash_core_info_t *crash_core_get_info(void);

/* Raw access to the stored dump, for the /api/crash/raw download endpoint.
 * crash_core_read() returns ESP_ERR_NOT_FOUND when no dump is stored. */
esp_err_t crash_core_read(size_t offset, void *buf, size_t len, size_t *out_read);

/* Discards the stored dump (also clears crash_core_get_info()->present). */
esp_err_t crash_core_erase(void);
