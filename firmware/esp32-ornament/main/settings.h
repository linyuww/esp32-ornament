#pragma once

#include "esp_err.h"

#include <stddef.h>
#include <stdbool.h>

#define ORNAMENT_WIFI_SSID_MAX 32
#define ORNAMENT_WIFI_PASSWORD_MAX 64
#define ORNAMENT_BRIDGE_URL_MAX 160
#define ORNAMENT_MUSIC_SERVICE_BASE_URL_MAX 192
#define ORNAMENT_XIAOZHI_WS_URL_MAX 192
#define ORNAMENT_XIAOZHI_TOKEN_MAX 160
#define ORNAMENT_MUSIC_AUTH_SECRET_MAX 160
#define ORNAMENT_WEATHER_LABEL_MAX 24
#define ORNAMENT_WEATHER_SOURCE_MAX 16
#define ORNAMENT_WEATHER_TOKEN_MAX 96
#define ORNAMENT_WEATHER_SOURCE_CAIYUN "caiyun"
#define ORNAMENT_WEATHER_SOURCE_OPEN_METEO "open-meteo"

typedef struct {
    char ssid[ORNAMENT_WIFI_SSID_MAX + 1];
    char password[ORNAMENT_WIFI_PASSWORD_MAX + 1];
    char bridge_url[ORNAMENT_BRIDGE_URL_MAX];
    char music_service_base_url[ORNAMENT_MUSIC_SERVICE_BASE_URL_MAX];
    char music_auth_secret[ORNAMENT_MUSIC_AUTH_SECRET_MAX];
    char xiaozhi_ws_url[ORNAMENT_XIAOZHI_WS_URL_MAX];
    char xiaozhi_token[ORNAMENT_XIAOZHI_TOKEN_MAX];
    char weather_label[ORNAMENT_WEATHER_LABEL_MAX + 1];
    char weather_source[ORNAMENT_WEATHER_SOURCE_MAX + 1];
    char caiyun_token[ORNAMENT_WEATHER_TOKEN_MAX + 1];
    int weather_lat_e6;
    int weather_lon_e6;
    int audio_volume_percent;
    bool has_wifi;
    bool has_bridge_url;
    bool has_music_service_base_url;
    bool has_music_auth_secret;
    bool has_xiaozhi_ws_url;
    bool has_xiaozhi_token;
    bool has_caiyun_token;
} ornament_settings_t;

esp_err_t settings_load(ornament_settings_t *settings);
esp_err_t settings_save(const ornament_settings_t *settings);
esp_err_t settings_clear(void);
esp_err_t settings_load_audio_volume_percent(int *volume_percent);
esp_err_t settings_save_audio_volume_percent(int volume_percent);
const char *settings_bridge_url_or_default(const ornament_settings_t *settings);
const char *settings_music_service_base_url_or_default(const ornament_settings_t *settings);
const char *settings_music_auth_secret_or_default(const ornament_settings_t *settings);
const char *settings_xiaozhi_ws_url_or_default(const ornament_settings_t *settings);
const char *settings_xiaozhi_token_or_default(const ornament_settings_t *settings);
int settings_audio_volume_percent_or_default(const ornament_settings_t *settings);
const char *settings_weather_label_or_default(const ornament_settings_t *settings);
const char *settings_weather_source_or_default(const ornament_settings_t *settings);
