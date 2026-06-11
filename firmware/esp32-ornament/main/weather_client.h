#pragma once

#include "ornament_state.h"

#include "esp_err.h"
#include "sdkconfig.h"

#include <stdbool.h>

#ifndef CONFIG_ORNAMENT_LOCAL_WEATHER_ENABLED
#define CONFIG_ORNAMENT_LOCAL_WEATHER_ENABLED 0
#endif

#if CONFIG_ORNAMENT_LOCAL_WEATHER_ENABLED

esp_err_t weather_client_start(void);
void weather_client_apply(ornament_state_t *state);
bool weather_client_token_configured(void);
void weather_client_settings_changed(void);

#else

static inline esp_err_t weather_client_start(void)
{
    return ESP_OK;
}

static inline void weather_client_apply(ornament_state_t *state)
{
    (void)state;
}

static inline bool weather_client_token_configured(void)
{
    return false;
}

static inline void weather_client_settings_changed(void)
{
}

#endif
