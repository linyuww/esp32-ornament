#pragma once

#include "esp_err.h"

#include <stdbool.h>

#define ORNAMENT_WIFI_SSID_MAX 32
#define ORNAMENT_WIFI_PASSWORD_MAX 64
#define ORNAMENT_BRIDGE_URL_MAX 160

typedef struct {
    char ssid[ORNAMENT_WIFI_SSID_MAX + 1];
    char password[ORNAMENT_WIFI_PASSWORD_MAX + 1];
    char bridge_url[ORNAMENT_BRIDGE_URL_MAX];
    bool has_wifi;
    bool has_bridge_url;
} ornament_settings_t;

esp_err_t settings_load(ornament_settings_t *settings);
esp_err_t settings_save(const ornament_settings_t *settings);
esp_err_t settings_clear(void);
const char *settings_bridge_url_or_default(const ornament_settings_t *settings);
