#pragma once

#include "esp_err.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#define SYSTEM_DIAGNOSTICS_MAX_TASKS 10
#define SYSTEM_DIAGNOSTICS_MAX_HEAP_CHECKPOINTS 12
#define SYSTEM_DIAGNOSTICS_TASK_NAME_MAX 16
#define SYSTEM_DIAGNOSTICS_STAGE_MAX 24

typedef struct {
    char name[SYSTEM_DIAGNOSTICS_TASK_NAME_MAX];
    uint32_t configured_stack_bytes;
    uint32_t stack_high_water_bytes;
    UBaseType_t priority;
    bool external_stack;
    bool valid;
} system_diagnostics_task_t;

typedef struct {
    char stage[SYSTEM_DIAGNOSTICS_STAGE_MAX];
    int64_t uptime_ms;
    uint32_t free_heap;
    uint32_t minimum_free_heap;
    uint32_t largest_8bit_block;
    uint32_t internal_free;
    uint32_t spiram_free;
} system_diagnostics_heap_checkpoint_t;

typedef struct {
    int64_t uptime_ms;
    esp_reset_reason_t reset_reason;
    uint32_t free_heap;
    uint32_t minimum_free_heap;
    uint32_t largest_8bit_block;
    uint32_t largest_internal_block;
    uint32_t largest_spiram_block;
    uint32_t internal_free;
    uint32_t internal_minimum_free;
    uint32_t spiram_free;
    uint32_t spiram_minimum_free;
    size_t task_count;
    system_diagnostics_task_t tasks[SYSTEM_DIAGNOSTICS_MAX_TASKS];
    size_t heap_checkpoint_count;
    system_diagnostics_heap_checkpoint_t heap_checkpoints[SYSTEM_DIAGNOSTICS_MAX_HEAP_CHECKPOINTS];
} system_diagnostics_snapshot_t;

void system_diagnostics_record_heap(const char *stage);
void system_diagnostics_register_task(
    const char *name,
    TaskHandle_t handle,
    uint32_t configured_stack_bytes,
    UBaseType_t priority,
    bool external_stack);
void system_diagnostics_snapshot(system_diagnostics_snapshot_t *snapshot);
const char *system_diagnostics_reset_reason_name(esp_reset_reason_t reason);
