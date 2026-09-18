/* SPDX-License-Identifier: LicenseRef-FNCL-1.1
 * Copyright (c) 2026 Cpt_Kirk
 */
#pragma once

#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"

/* Panel RAM split: internal DRAM (~341 KB on the S3 variant) has to carry Wi-Fi,
 * lwIP, the IDF services and every small (<1024 B) heap_caps_malloc, while PSRAM
 * has megabytes free. Long-lived application tasks therefore take their stack
 * from PSRAM; internal RAM stays for the safety nets (boot guard, OTA) that must
 * still work when PSRAM or the heap is in a bad state.
 *
 * Tasks created this way are never deleted. If one ever has to be, use
 * app_task_delete() instead of vTaskDelete(), otherwise the PSRAM stack leaks. */

#if defined(CONFIG_SPIRAM) && CONFIG_SPIRAM && defined(CONFIG_FREERTOS_TASK_CREATE_ALLOW_EXT_MEM) \
    && CONFIG_FREERTOS_TASK_CREATE_ALLOW_EXT_MEM
#define APP_TASK_STACK_CAPS (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
#define APP_TASK_STACK_EXT  1
#else
#define APP_TASK_STACK_CAPS (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)
#define APP_TASK_STACK_EXT  0
#endif

static inline BaseType_t app_task_create(TaskFunction_t fn, const char *name, uint32_t stack_depth, void *arg,
    UBaseType_t prio, TaskHandle_t *handle)
{
#if APP_TASK_STACK_EXT
    BaseType_t created = xTaskCreateWithCaps(fn, name, stack_depth, arg, prio, handle, APP_TASK_STACK_CAPS);
    if (created == pdPASS) {
        return created;
    }
    /* PSRAM exhausted: a task on internal RAM is still better than no task. */
#endif
    return xTaskCreate(fn, name, stack_depth, arg, prio, handle);
}

static inline BaseType_t app_task_create_pinned(TaskFunction_t fn, const char *name, uint32_t stack_depth, void *arg,
    UBaseType_t prio, TaskHandle_t *handle, BaseType_t core_id)
{
#if CONFIG_FREERTOS_NUMBER_OF_CORES > 1
    if (core_id >= 0) {
#if APP_TASK_STACK_EXT
        BaseType_t created = xTaskCreatePinnedToCoreWithCaps(fn, name, stack_depth, arg, prio, handle, core_id,
            APP_TASK_STACK_CAPS);
        if (created == pdPASS) {
            return created;
        }
#endif
        return xTaskCreatePinnedToCore(fn, name, stack_depth, arg, prio, handle, core_id);
    }
#endif
    (void)core_id;
    return app_task_create(fn, name, stack_depth, arg, prio, handle);
}

static inline void app_task_delete(TaskHandle_t handle)
{
#if APP_TASK_STACK_EXT
    vTaskDeleteWithCaps(handle);
#else
    vTaskDelete(handle);
#endif
}
