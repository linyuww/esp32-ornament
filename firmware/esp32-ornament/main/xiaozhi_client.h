#pragma once

#include "esp_err.h"
#include "settings.h"

#include <stdbool.h>
#include <stdint.h>

#define XIAOZHI_STATUS_TEXT_MAX 96
#define XIAOZHI_SESSION_ID_MAX 64

typedef enum {
    XIAOZHI_CLIENT_STATE_DISABLED = 0,
    XIAOZHI_CLIENT_STATE_IDLE,
    XIAOZHI_CLIENT_STATE_CONFIG_MISSING,
    XIAOZHI_CLIENT_STATE_CONNECTING,
    XIAOZHI_CLIENT_STATE_LISTENING,
    XIAOZHI_CLIENT_STATE_SPEAKING,
    XIAOZHI_CLIENT_STATE_ERROR,
} xiaozhi_client_state_t;

typedef struct {
    bool enabled;
    bool configured;
    bool connected;
    xiaozhi_client_state_t state;
    char ws_url[ORNAMENT_XIAOZHI_WS_URL_MAX];
    char session_id[XIAOZHI_SESSION_ID_MAX];
    char last_error[XIAOZHI_STATUS_TEXT_MAX];
    char last_stt[XIAOZHI_STATUS_TEXT_MAX];
    char last_tts[XIAOZHI_STATUS_TEXT_MAX];
    uint32_t uplink_frames;
    uint32_t downlink_frames;
} xiaozhi_client_snapshot_t;

esp_err_t xiaozhi_client_init(void);
esp_err_t xiaozhi_client_start_session(void);
esp_err_t xiaozhi_client_stop_session(void);
void xiaozhi_client_status_snapshot(xiaozhi_client_snapshot_t *snapshot);
const char *xiaozhi_client_state_name(xiaozhi_client_state_t state);
