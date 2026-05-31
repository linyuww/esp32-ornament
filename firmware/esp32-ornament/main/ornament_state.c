#include "ornament_state.h"

#include <string.h>

#ifndef CONFIG_ORNAMENT_UI_FRAME_MS
#define CONFIG_ORNAMENT_UI_FRAME_MS 250
#endif

#ifndef CONFIG_ORNAMENT_DONE_FLASH_MS
#define CONFIG_ORNAMENT_DONE_FLASH_MS 5000
#endif

#define ORNAMENT_DONE_FLASH_STEP_MS 400

void ornament_state_init(ornament_state_t *state)
{
    memset(state, 0, sizeof(*state));
    state->status = ORNAMENT_STATUS_IDLE;
    state->codex_task_status = ORNAMENT_STATUS_IDLE;
    state->claude_task_status = ORNAMENT_STATUS_IDLE;
    state->primary_remaining_percent = -1;
    state->secondary_remaining_percent = -1;
    state->active_task_count = 0;
}

ornament_status_t ornament_status_from_text(const char *value)
{
    if (value == NULL) {
        return ORNAMENT_STATUS_IDLE;
    }
    if (strcmp(value, "running") == 0) {
        return ORNAMENT_STATUS_RUNNING;
    }
    if (strcmp(value, "done") == 0) {
        return ORNAMENT_STATUS_DONE;
    }
    if (strcmp(value, "error") == 0) {
        return ORNAMENT_STATUS_ERROR;
    }
    if (strcmp(value, "event") == 0) {
        return ORNAMENT_STATUS_EVENT;
    }
    return ORNAMENT_STATUS_IDLE;
}

ornament_status_t ornament_state_panel_status(const ornament_state_t *state)
{
    if (state == NULL) {
        return ORNAMENT_STATUS_IDLE;
    }
    if (state->status == ORNAMENT_STATUS_ERROR) {
        return ORNAMENT_STATUS_ERROR;
    }
    if (state->done_flash_active) {
        return ORNAMENT_STATUS_DONE;
    }
    if (state->status == ORNAMENT_STATUS_DONE && state->active_task_count > 0) {
        return ORNAMENT_STATUS_RUNNING;
    }
    return state->status;
}

void ornament_state_update_display_timing(
    ornament_state_t *state,
    uint32_t now_ms,
    uint32_t last_done_ms,
    bool has_done_event)
{
    if (state == NULL) {
        return;
    }

    uint32_t frame_ms = CONFIG_ORNAMENT_UI_FRAME_MS > 0 ? CONFIG_ORNAMENT_UI_FRAME_MS : 1;
    state->active_dot_phase = (uint8_t)((now_ms / frame_ms) & 0x03);
    state->done_flash_active = false;
    state->done_flash_on = false;

    if (!has_done_event || state->status == ORNAMENT_STATUS_ERROR || CONFIG_ORNAMENT_DONE_FLASH_MS <= 0) {
        return;
    }

    uint32_t elapsed_ms = now_ms - last_done_ms;
    if (elapsed_ms >= (uint32_t)CONFIG_ORNAMENT_DONE_FLASH_MS) {
        return;
    }

    state->done_flash_active = true;
    state->done_flash_on = ((elapsed_ms / ORNAMENT_DONE_FLASH_STEP_MS) % 2U) == 0U;
}
