#pragma once

#include <stdbool.h>
#include <stdint.h>

#define ORNAMENT_TEXT_MAX 192
#define ORNAMENT_TIME_MAX 48

typedef enum {
    ORNAMENT_STATUS_IDLE,
    ORNAMENT_STATUS_RUNNING,
    ORNAMENT_STATUS_DONE,
    ORNAMENT_STATUS_EVENT,
    ORNAMENT_STATUS_ERROR,
} ornament_status_t;

typedef struct {
    ornament_status_t status;
    char task_title[ORNAMENT_TEXT_MAX];
    char task_message[ORNAMENT_TEXT_MAX];
    char task_received_at[ORNAMENT_TIME_MAX];
    char codex_task_title[ORNAMENT_TEXT_MAX];
    char codex_task_message[ORNAMENT_TEXT_MAX];
    char claude_task_title[ORNAMENT_TEXT_MAX];
    char claude_task_message[ORNAMENT_TEXT_MAX];
    ornament_status_t codex_task_status;
    ornament_status_t claude_task_status;
    char quota_status[32];
    int primary_remaining_percent;
    int secondary_remaining_percent;
    int active_task_count;
    int done_seq;
    int codex_active_task_count;
    int claude_active_task_count;
    int codex_done_seq;
    int claude_done_seq;
    char primary_resets_at[ORNAMENT_TIME_MAX];
    char secondary_resets_at[ORNAMENT_TIME_MAX];
    char bridge_observed_at[ORNAMENT_TIME_MAX];
    char local_time[8];
    char local_date[8];
    char wifi_ssid[33];
    int wifi_rssi;
    bool has_task;
    bool has_codex_summary;
    bool has_claude_summary;
    bool has_codex_task;
    bool has_claude_task;
    bool has_quota;
    bool time_synced;
    bool wifi_connected;
    bool done_flash_active;
    bool done_flash_on;
    uint8_t active_dot_phase;
} ornament_state_t;

void ornament_state_init(ornament_state_t *state);
ornament_status_t ornament_status_from_text(const char *value);
