/*
 * SPDX-License-Identifier: LicenseRef-FNCL-1.1
 * Copyright (c) 2026 Cpt_Kirk
 */
#include "crash_core.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_core_dump.h"
#include "esp_log.h"
#include "esp_partition.h"

static const char *TAG = "crash_core";

static crash_core_info_t s_info = {
    .cause_str = "unknown",
};
static bool s_initialized;
static bool s_reason_from_dump; /* the panic handler stored a reason string */
static const esp_partition_t *s_partition;
static size_t s_dump_offset; /* dump start relative to the partition start */

/* Decoded fault cause.  The panel speaks RISC-V (ESP32-P4); the Xtensa branch
 * only exists so the S3 variant of this tree keeps compiling. */
static const char *crash_core_cause_str(uint32_t cause)
{
#if CONFIG_IDF_TARGET_ARCH_RISCV
    if ((cause & 0x80000000U) != 0U) {
        return "interrupt";
    }
    /* The SoC-level faults of the RISC-V parts are not standard exception
     * codes: they are reported as pseudo "interrupt numbers" (see the
     * ETS_*_INUM defines in soc/soc.h, same numbering since the C3).
     * They must be matched before the standard codes below, which is why this
     * block sits in front of the switch - 24 is a valid interrupt watchdog
     * number, not "Environment call from..." of the standard table. */
    if ((cause & 0xFFU) == 24U) { /* ETS_INT_WDT_INUM */
        return ((cause & 0x1000U) != 0U) ? "Interrupt wdt timeout on CPU1"
                                         : "Interrupt wdt timeout on CPU0";
    }
    switch (cause & 0xFFU) {
        case 0: return "Instruction address misaligned";
        case 1: return "Instruction access fault";
        case 2: return "Illegal instruction";
        case 3: return "Breakpoint";
        case 4: return "Load address misaligned";
        case 5: return "Load access fault (LoadProhibited)";
        case 6: return "Store/AMO address misaligned";
        case 7: return "Store/AMO access fault (StoreProhibited)";
        case 25: return "Cache error";
        case 26: return "Memory protection fault";
        /* Watch this one: the hardware stack guard fires when a task stack or an
         * ISR stack is written below its allocated range.  The task name in the
         * report is only the task that happened to be interrupted - when sp sits
         * at the bottom of the ISR stack the interrupt chain, not that task, ran
         * out of stack. */
        case 27: return "Stack protection fault (task or ISR stack overflow)";
        default: return "unknown exception";
    }
#else
    switch (cause) {
        case 0: return "IllegalInstruction";
        case 1: return "Syscall";
        case 2: return "InstructionFetchError";
        case 3: return "LoadStoreError";
        case 4: return "Level1Interrupt";
        case 5: return "Alloca";
        case 6: return "IntegerDivideByZero";
        case 8: return "Privileged";
        case 9: return "LoadStoreAlignment";
        case 12: return "InstrFetchProhibited";
        case 13: return "LoadProhibited";
        case 14: return "StoreProhibited";
        default: return "unknown exception";
    }
#endif
}

/* The panic reason string written by the panic handler is multi-line; the
 * persistent log stores one line per entry, so fold it into a single line. */
static void crash_core_fold_reason(const char *src)
{
    size_t out = 0;
    bool last_space = false;
    for (const char *p = src; *p != '\0' && out + 1U < sizeof(s_info.reason); p++) {
        char c = *p;
        if (c == '\r' || c == '\n' || c == '\t' || c == ' ') {
            if (last_space || out == 0) {
                continue;
            }
            c = ' ';
            last_space = true;
        } else {
            last_space = false;
        }
        s_info.reason[out++] = c;
    }
    s_info.reason[out] = '\0';
}

#if CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH && CONFIG_ESP_COREDUMP_DATA_FORMAT_ELF
static void crash_core_log_summary(void)
{
    esp_core_dump_summary_t *summary = (esp_core_dump_summary_t *)calloc(1, sizeof(esp_core_dump_summary_t));
    if (summary == NULL) {
        ESP_LOGW(TAG, "PANIC IN PREVIOUS BOOT - core dump present but its summary could not be allocated");
        return;
    }

    if (esp_core_dump_get_summary(summary) != ESP_OK) {
        ESP_LOGW(TAG, "PANIC IN PREVIOUS BOOT - core dump present (%u bytes) but the summary could not be read; "
                      "download it with GET /api/crash/raw and run 'idf.py coredump-info'",
                 (unsigned)s_info.size);
        free(summary);
        return;
    }

    snprintf(s_info.task, sizeof(s_info.task), "%s", summary->exc_task);
    s_info.pc = summary->exc_pc;
    memcpy(s_info.app_sha, summary->app_elf_sha256, sizeof(s_info.app_sha) - 1U);
    s_info.app_sha[sizeof(s_info.app_sha) - 1U] = '\0';

    char running_sha[sizeof(s_info.app_sha)] = {0};
    esp_app_get_elf_sha256(running_sha, sizeof(running_sha));
    s_info.app_match = (strncmp(s_info.app_sha, running_sha, sizeof(s_info.app_sha) - 1U) == 0);

#if CONFIG_IDF_TARGET_ARCH_RISCV
    s_info.cause = summary->ex_info.mcause;
    s_info.fault_addr = summary->ex_info.mtval;
    s_info.ra = summary->ex_info.ra;
    s_info.sp = summary->ex_info.sp;
#else
    s_info.cause = summary->ex_info.exc_cause;
    s_info.fault_addr = summary->ex_info.exc_vaddr;
#endif
    s_info.cause_str = crash_core_cause_str(s_info.cause);

    /* The panic handler only leaves a reason note when a task watchdog fires or
     * when the code aborted itself (ESP_ERROR_CHECK/abort), so a stack guard or
     * cache fault always arrives without one.  Deriving the text from the decoded
     * cause keeps the report self-contained instead of printing an empty
     * placeholder the reader cannot act on. */
    if (!s_reason_from_dump) {
        snprintf(s_info.reason, sizeof(s_info.reason),
                 "not recorded by the panic handler (only task-WDT panics and explicit aborts store one); "
                 "derived from the fault cause: %s",
                 s_info.cause_str);
    }

    /* A single self-contained line: the ring log is trimmed from the front, so a
     * crash report must be readable without its surrounding lines. */
    ESP_LOGW(TAG,
             "PANIC IN PREVIOUS BOOT: task='%s' pc=0x%08" PRIx32 " cause='%s' (0x%" PRIx32 ") fault_addr=0x%08" PRIx32
             " ra=0x%08" PRIx32 " sp=0x%08" PRIx32 " dump=%u bytes%s",
             s_info.task,
             (uint32_t)s_info.pc,
             s_info.cause_str,
             (uint32_t)s_info.cause,
             (uint32_t)s_info.fault_addr,
             (uint32_t)s_info.ra,
             (uint32_t)s_info.sp,
             (unsigned)s_info.size,
             s_info.app_match ? "" : " (dump is from another app image - reflash helpers before addr2line)");
    ESP_LOGW(TAG, "PANIC IN PREVIOUS BOOT: reason: %s", s_info.reason);
    ESP_LOGW(TAG, "PANIC IN PREVIOUS BOOT: decode with 'curl -o core.elf http://<panel>/api/crash/raw' then "
                  "'idf.py coredump-info -c core.elf'");
    free(summary);
}
#else
static void crash_core_log_summary(void)
{
    ESP_LOGI(TAG, "Core dump capture disabled in this build (CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH=n)");
}
#endif

void crash_core_init(void)
{
    if (s_initialized) {
        return;
    }
    s_initialized = true;

    s_partition = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_COREDUMP, NULL);
    s_info.partition_ok = (s_partition != NULL);
    if (!s_info.partition_ok) {
        ESP_LOGW(TAG, "No 'coredump' partition in the partition table - a panic will only reach the UART");
        return;
    }

#if CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH && CONFIG_ESP_COREDUMP_DATA_FORMAT_ELF
    /* A blank partition reports ESP_ERR_INVALID_SIZE (its leading size word is
     * still 0xFFFFFFFF) and a missing partition ESP_ERR_NOT_FOUND - both simply
     * mean the previous boot did not panic. */
    s_info.check_err = esp_core_dump_image_check();
    if (s_info.check_err == ESP_ERR_NOT_FOUND || s_info.check_err == ESP_ERR_INVALID_SIZE) {
        ESP_LOGI(TAG, "No core dump stored: the previous boot ended without a panic");
        return;
    }

    if (s_info.check_err != ESP_OK) {
        /* A half-written dump (power loss during the panic write) fails its
         * checksum; there is nothing to decode and the next panic overwrites it. */
        ESP_LOGW(TAG, "Core dump in flash failed its integrity check (%s), ignoring it",
                 esp_err_to_name(s_info.check_err));
        return;
    }

    size_t addr = 0;
    size_t size = 0;
    if (esp_core_dump_image_get(&addr, &size) != ESP_OK || size == 0) {
        ESP_LOGW(TAG, "Core dump passed its integrity check but its address/size could not be read");
        return;
    }

    s_info.size = size;
    s_dump_offset = (addr >= s_partition->address) ? (addr - s_partition->address) : 0U;
    s_info.present = true;
    {
        /* get_panic_reason() copies the panic handler's multi-line message. */
        char raw[sizeof(s_info.reason)] = {0};
        s_reason_from_dump = (esp_core_dump_get_panic_reason(raw, sizeof(raw)) == ESP_OK);
        if (!s_reason_from_dump) {
            snprintf(raw, sizeof(raw), "(reason string not available)");
        }
        crash_core_fold_reason(raw);
    }

    crash_core_log_summary();
#else
    ESP_LOGI(TAG, "Core dump capture disabled in this build (CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH=n)");
#endif
}

const crash_core_info_t *crash_core_get_info(void)
{
    return &s_info;
}

esp_err_t crash_core_read(size_t offset, void *buf, size_t len, size_t *out_read)
{
    if (out_read != NULL) {
        *out_read = 0;
    }
    if (buf == NULL || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_info.present || s_partition == NULL) {
        return ESP_ERR_NOT_FOUND;
    }
    if (offset >= s_info.size) {
        return ESP_ERR_INVALID_ARG;
    }

    size_t want = len;
    if (want > (s_info.size - offset)) {
        want = s_info.size - offset;
    }
    esp_err_t err = esp_partition_read(s_partition, s_dump_offset + offset, buf, want);
    if (err != ESP_OK) {
        return err;
    }
    if (out_read != NULL) {
        *out_read = want;
    }
    return ESP_OK;
}

esp_err_t crash_core_erase(void)
{
    esp_err_t err = esp_core_dump_image_erase();
    if (err == ESP_OK) {
        s_info.present = false;
        s_info.size = 0;
        ESP_LOGI(TAG, "Stored core dump erased");
    }
    return err;
}
