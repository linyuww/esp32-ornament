#pragma once

#include "esp_err.h"
#include "settings.h"

#include <stdbool.h>
#include <stdint.h>

#define XIAOZHI_STATUS_TEXT_MAX 256
#define XIAOZHI_SESSION_ID_MAX 64
#define XIAOZHI_CLIENT_ID_MAX 40
#define XIAOZHI_ACTIVATION_CODE_MAX 16

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
    int protocol_version;
    bool activation_pending;
    char ws_url[ORNAMENT_XIAOZHI_WS_URL_MAX];
    char client_id[XIAOZHI_CLIENT_ID_MAX];
    char session_id[XIAOZHI_SESSION_ID_MAX];
    char activation_code[XIAOZHI_ACTIVATION_CODE_MAX];
    char activation_message[XIAOZHI_STATUS_TEXT_MAX];
    char last_error[XIAOZHI_STATUS_TEXT_MAX];
    char last_stt[XIAOZHI_STATUS_TEXT_MAX];
    char last_tts[XIAOZHI_STATUS_TEXT_MAX];
    uint32_t uplink_frames;
    uint32_t downlink_frames;
} xiaozhi_client_snapshot_t;

typedef struct {
    bool configured;
    bool websocket_connected;
    bool hello_received;
    esp_err_t err;
    int http_status;
    char session_id[XIAOZHI_SESSION_ID_MAX];
    char detail[XIAOZHI_STATUS_TEXT_MAX];
} xiaozhi_probe_result_t;

esp_err_t xiaozhi_client_init(void);
esp_err_t xiaozhi_client_probe(const char *ws_url_override, const char *token_override, xiaozhi_probe_result_t *result);
esp_err_t xiaozhi_client_start_session(void);
esp_err_t xiaozhi_client_reconnect_session(bool start_if_idle);
esp_err_t xiaozhi_client_stop_session(void);
void xiaozhi_client_status_snapshot(xiaozhi_client_snapshot_t *snapshot);
const char *xiaozhi_client_state_name(xiaozhi_client_state_t state);
