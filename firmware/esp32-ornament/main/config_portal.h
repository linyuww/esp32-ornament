#pragma once

#include "esp_err.h"
#include "sdkconfig.h"

#ifndef CONFIG_ORNAMENT_CONFIG_PORTAL_ENABLED
#define CONFIG_ORNAMENT_CONFIG_PORTAL_ENABLED 0
#endif

#if CONFIG_ORNAMENT_CONFIG_PORTAL_ENABLED

esp_err_t config_portal_start(void);
const char *config_portal_ssid(void);

#else

static inline esp_err_t config_portal_start(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

static inline const char *config_portal_ssid(void)
{
    return CONFIG_ORNAMENT_PROV_AP_PREFIX;
}

#endif
