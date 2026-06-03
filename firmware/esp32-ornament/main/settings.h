#pragma once

#include "esp_err.h"

#include <stdbool.h>

#define ORNAMENT_WIFI_SSID_MAX 32
#define ORNAMENT_WIFI_PASSWORD_MAX 64
#define ORNAMENT_BRIDGE_URL_MAX 160
#define ORNAMENT_WEATHER_LABEL_MAX 24
#define ORNAMENT_WEATHER_TOKEN_MAX 96

typedef struct {
    char ssid[ORNAMENT_WIFI_SSID_MAX + 1];
    char password[ORNAMENT_WIFI_PASSWORD_MAX + 1];
    char bridge_url[ORNAMENT_BRIDGE_URL_MAX];
    char weather_label[ORNAMENT_WEATHER_LABEL_MAX + 1];
    char caiyun_token[ORNAMENT_WEATHER_TOKEN_MAX + 1];
    int weather_lat_e6;
    int weather_lon_e6;
    int audio_volume_percent;
    bool has_wifi;
    bool has_bridge_url;
    bool has_caiyun_token;
} ornament_settings_t;

esp_err_t settings_load(ornament_settings_t *settings);
esp_err_t settings_save(const ornament_settings_t *settings);
esp_err_t settings_clear(void);
const char *settings_bridge_url_or_default(const ornament_settings_t *settings);
int settings_audio_volume_percent_or_default(const ornament_settings_t *settings);
const char *settings_weather_label_or_default(const ornament_settings_t *settings);
