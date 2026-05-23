#pragma once

#include "esp_err.h"
#include "ornament_state.h"

esp_err_t web_console_start(void);
void web_console_set_last_state(const ornament_state_t *state, esp_err_t fetch_error);
