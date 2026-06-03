#pragma once

#include "ornament_state.h"

#include "esp_err.h"

#include <stdbool.h>

esp_err_t weather_client_start(void);
void weather_client_apply(ornament_state_t *state);
bool weather_client_token_configured(void);
void weather_client_settings_changed(bool token_configured);
