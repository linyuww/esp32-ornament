#pragma once

#include "esp_err.h"

#include "cJSON.h"

esp_err_t xiaozhi_mcp_init(void);
esp_err_t xiaozhi_mcp_handle_request(const cJSON *payload, char **response_json);
