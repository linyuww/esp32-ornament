#pragma once

#include "esp_err.h"
#include "ornament_state.h"
#include "settings.h"

#include <stdbool.h>

typedef struct {
    esp_err_t error;
    int http_status;
    int response_bytes;
    bool json_ok;
    char error_name[32];
    char status_text[32];
} bridge_probe_result_t;

typedef struct {
    esp_err_t last_error;
    int tested_count;
    bool current_ok;
    bool saved;
    char bridge_url[ORNAMENT_BRIDGE_URL_MAX];
    char source[24];
} bridge_auto_match_result_t;

esp_err_t bridge_client_fetch_state(ornament_state_t *state);
esp_err_t bridge_client_probe_url(const char *url, bridge_probe_result_t *result);
esp_err_t bridge_client_auto_match(bool verify_current, bridge_auto_match_result_t *result);
