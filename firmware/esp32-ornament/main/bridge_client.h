#pragma once

#include "esp_err.h"
#include "ornament_state.h"

#include <stdbool.h>

typedef struct {
    esp_err_t error;
    int http_status;
    int response_bytes;
    bool json_ok;
    char error_name[32];
    char status_text[32];
} bridge_probe_result_t;

esp_err_t bridge_client_fetch_state(ornament_state_t *state);
esp_err_t bridge_client_probe_url(const char *url, bridge_probe_result_t *result);
