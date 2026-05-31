#include "ornament_state.h"

#include <assert.h>
#include <stdbool.h>

static void expect_timing(
    ornament_status_t status,
    int active_count,
    int codex_active_count,
    int claude_active_count,
    uint32_t now_ms,
    uint32_t last_done_ms,
    bool has_done_event,
    bool expected_flash_active,
    bool expected_flash_on,
    ornament_status_t expected_panel_status)
{
    ornament_state_t state;
    ornament_state_init(&state);
    state.status = status;
    state.active_task_count = active_count;
    state.codex_active_task_count = codex_active_count;
    state.claude_active_task_count = claude_active_count;

    ornament_state_update_display_timing(&state, now_ms, last_done_ms, has_done_event);

    assert(state.done_flash_active == expected_flash_active);
    assert(state.done_flash_on == expected_flash_on);
    assert(ornament_state_panel_status(&state) == expected_panel_status);
}

static void running_tasks_do_not_flash_without_done_event(void)
{
    expect_timing(ORNAMENT_STATUS_RUNNING, 1, 1, 0, 1000, 0, false, false, false, ORNAMENT_STATUS_RUNNING);
    expect_timing(ORNAMENT_STATUS_RUNNING, 2, 1, 1, 1000, 0, false, false, false, ORNAMENT_STATUS_RUNNING);
}

static void done_event_with_remaining_work_flashes_then_returns_to_running(void)
{
    expect_timing(ORNAMENT_STATUS_DONE, 1, 1, 0, 1000, 1000, true, true, true, ORNAMENT_STATUS_DONE);
    expect_timing(ORNAMENT_STATUS_DONE, 1, 1, 0, 1399, 1000, true, true, true, ORNAMENT_STATUS_DONE);
    expect_timing(ORNAMENT_STATUS_DONE, 1, 1, 0, 1400, 1000, true, true, false, ORNAMENT_STATUS_DONE);
    expect_timing(ORNAMENT_STATUS_DONE, 1, 1, 0, 5999, 1000, true, true, true, ORNAMENT_STATUS_DONE);
    expect_timing(ORNAMENT_STATUS_DONE, 1, 1, 0, 6000, 1000, true, false, false, ORNAMENT_STATUS_RUNNING);
}

static void concurrent_source_tasks_share_the_same_esp_panel_status(void)
{
    expect_timing(ORNAMENT_STATUS_DONE, 2, 1, 1, 2000, 2000, true, true, true, ORNAMENT_STATUS_DONE);
    expect_timing(ORNAMENT_STATUS_DONE, 2, 1, 1, 6999, 2000, true, true, true, ORNAMENT_STATUS_DONE);
    expect_timing(ORNAMENT_STATUS_DONE, 2, 1, 1, 7000, 2000, true, false, false, ORNAMENT_STATUS_RUNNING);
}

static void done_without_remaining_work_stays_done_after_flash(void)
{
    expect_timing(ORNAMENT_STATUS_DONE, 0, 0, 0, 3000, 3000, true, true, true, ORNAMENT_STATUS_DONE);
    expect_timing(ORNAMENT_STATUS_DONE, 0, 0, 0, 8000, 3000, true, false, false, ORNAMENT_STATUS_DONE);
}

static void error_status_suppresses_done_flash(void)
{
    expect_timing(ORNAMENT_STATUS_ERROR, 2, 1, 1, 1000, 1000, true, false, false, ORNAMENT_STATUS_ERROR);
}

static void non_done_statuses_keep_original_panel_status(void)
{
    expect_timing(ORNAMENT_STATUS_EVENT, 2, 1, 1, 1000, 0, false, false, false, ORNAMENT_STATUS_EVENT);
    expect_timing(ORNAMENT_STATUS_IDLE, 2, 1, 1, 1000, 0, false, false, false, ORNAMENT_STATUS_IDLE);
}

int main(void)
{
    running_tasks_do_not_flash_without_done_event();
    done_event_with_remaining_work_flashes_then_returns_to_running();
    concurrent_source_tasks_share_the_same_esp_panel_status();
    done_without_remaining_work_stays_done_after_flash();
    error_status_suppresses_done_flash();
    non_done_statuses_keep_original_panel_status();
    return 0;
}
