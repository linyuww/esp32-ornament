#include "system_diagnostics.h"

#include "esp_heap_caps.h"
#include "esp_timer.h"

#include <string.h>

typedef struct {
    char name[SYSTEM_DIAGNOSTICS_TASK_NAME_MAX];
    TaskHandle_t handle;
    uint32_t configured_stack_bytes;
    UBaseType_t priority;
    bool external_stack;
} registered_task_t;

static portMUX_TYPE diagnostics_lock = portMUX_INITIALIZER_UNLOCKED;
static registered_task_t registered_tasks[SYSTEM_DIAGNOSTICS_MAX_TASKS];
static size_t registered_task_count;
static system_diagnostics_heap_checkpoint_t heap_checkpoints[SYSTEM_DIAGNOSTICS_MAX_HEAP_CHECKPOINTS];
static size_t heap_checkpoint_count;

static int64_t uptime_ms(void)
{
    return esp_timer_get_time() / 1000;
}

void system_diagnostics_record_heap(const char *stage)
{
    system_diagnostics_heap_checkpoint_t checkpoint = {
        .uptime_ms = uptime_ms(),
        .free_heap = esp_get_free_heap_size(),
        .minimum_free_heap = esp_get_minimum_free_heap_size(),
        .largest_8bit_block = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT),
        .internal_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
        .spiram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT),
    };
    strlcpy(checkpoint.stage, stage != NULL ? stage : "unknown", sizeof(checkpoint.stage));

    taskENTER_CRITICAL(&diagnostics_lock);
    if (heap_checkpoint_count < SYSTEM_DIAGNOSTICS_MAX_HEAP_CHECKPOINTS) {
        heap_checkpoints[heap_checkpoint_count++] = checkpoint;
    } else {
        memmove(
            heap_checkpoints,
            heap_checkpoints + 1,
            sizeof(heap_checkpoints[0]) * (SYSTEM_DIAGNOSTICS_MAX_HEAP_CHECKPOINTS - 1));
        heap_checkpoints[SYSTEM_DIAGNOSTICS_MAX_HEAP_CHECKPOINTS - 1] = checkpoint;
    }
    taskEXIT_CRITICAL(&diagnostics_lock);
}

void system_diagnostics_register_task(
    const char *name,
    TaskHandle_t handle,
    uint32_t configured_stack_bytes,
    UBaseType_t priority,
    bool external_stack)
{
    if (handle == NULL || name == NULL || name[0] == '\0') {
        return;
    }

    taskENTER_CRITICAL(&diagnostics_lock);
    if (registered_task_count < SYSTEM_DIAGNOSTICS_MAX_TASKS) {
        registered_task_t *task = &registered_tasks[registered_task_count++];
        memset(task, 0, sizeof(*task));
        strlcpy(task->name, name, sizeof(task->name));
        task->handle = handle;
        task->configured_stack_bytes = configured_stack_bytes;
        task->priority = priority;
        task->external_stack = external_stack;
    }
    taskEXIT_CRITICAL(&diagnostics_lock);
}

void system_diagnostics_snapshot(system_diagnostics_snapshot_t *snapshot)
{
    if (snapshot == NULL) {
        return;
    }

    memset(snapshot, 0, sizeof(*snapshot));
    snapshot->uptime_ms = uptime_ms();
    snapshot->reset_reason = esp_reset_reason();
    snapshot->free_heap = esp_get_free_heap_size();
    snapshot->minimum_free_heap = esp_get_minimum_free_heap_size();
    snapshot->largest_8bit_block = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
    snapshot->largest_internal_block = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    snapshot->largest_spiram_block = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    snapshot->internal_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    snapshot->internal_minimum_free = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    snapshot->spiram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    snapshot->spiram_minimum_free = heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

    registered_task_t task_copy[SYSTEM_DIAGNOSTICS_MAX_TASKS];
    size_t task_count = 0;
    taskENTER_CRITICAL(&diagnostics_lock);
    task_count = registered_task_count;
    if (task_count > SYSTEM_DIAGNOSTICS_MAX_TASKS) {
        task_count = SYSTEM_DIAGNOSTICS_MAX_TASKS;
    }
    memcpy(task_copy, registered_tasks, sizeof(task_copy[0]) * task_count);
    snapshot->heap_checkpoint_count = heap_checkpoint_count;
    memcpy(
        snapshot->heap_checkpoints,
        heap_checkpoints,
        sizeof(heap_checkpoints[0]) * snapshot->heap_checkpoint_count);
    taskEXIT_CRITICAL(&diagnostics_lock);

    snapshot->task_count = task_count;
    for (size_t i = 0; i < task_count; i++) {
        system_diagnostics_task_t *task = &snapshot->tasks[i];
        strlcpy(task->name, task_copy[i].name, sizeof(task->name));
        task->configured_stack_bytes = task_copy[i].configured_stack_bytes;
        task->stack_high_water_bytes = uxTaskGetStackHighWaterMark(task_copy[i].handle) * sizeof(StackType_t);
        task->priority = task_copy[i].priority;
        task->external_stack = task_copy[i].external_stack;
        task->valid = true;
    }
}

const char *system_diagnostics_reset_reason_name(esp_reset_reason_t reason)
{
    switch (reason) {
    case ESP_RST_POWERON:
        return "poweron";
    case ESP_RST_EXT:
        return "external";
    case ESP_RST_SW:
        return "software";
    case ESP_RST_PANIC:
        return "panic";
    case ESP_RST_INT_WDT:
        return "interrupt_wdt";
    case ESP_RST_TASK_WDT:
        return "task_wdt";
    case ESP_RST_WDT:
        return "watchdog";
    case ESP_RST_DEEPSLEEP:
        return "deepsleep";
    case ESP_RST_BROWNOUT:
        return "brownout";
    case ESP_RST_SDIO:
        return "sdio";
    case ESP_RST_USB:
        return "usb";
    case ESP_RST_JTAG:
        return "jtag";
    case ESP_RST_EFUSE:
        return "efuse";
    case ESP_RST_PWR_GLITCH:
        return "power_glitch";
    case ESP_RST_CPU_LOCKUP:
        return "cpu_lockup";
    case ESP_RST_UNKNOWN:
    default:
        return "unknown";
    }
}
