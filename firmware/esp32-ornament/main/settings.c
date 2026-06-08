#include "settings.h"

#include "nvs.h"

#include <string.h>

#ifndef CONFIG_ORNAMENT_XIAOZHI_WS_URL
#define CONFIG_ORNAMENT_XIAOZHI_WS_URL ""
#endif

#ifndef CONFIG_ORNAMENT_XIAOZHI_TOKEN
#define CONFIG_ORNAMENT_XIAOZHI_TOKEN ""
#endif

static const char *NVS_NAMESPACE = "ornament";
static const char *KEY_SSID = "ssid";
static const char *KEY_PASSWORD = "password";
static const char *KEY_BRIDGE_URL = "bridge_url";
static const char *KEY_AUDIO_VOLUME = "audio_volume";
static const char *KEY_XIAOZHI_WS_URL = "xz_ws_url";
static const char *KEY_XIAOZHI_TOKEN = "xz_token";
static const char *KEY_WEATHER_LABEL = "weather_label";
static const char *KEY_WEATHER_SOURCE = "weather_source";
static const char *KEY_WEATHER_LAT_E6 = "weather_lat_e6";
static const char *KEY_WEATHER_LON_E6 = "weather_lon_e6";
static const char *KEY_CAIYUN_TOKEN = "caiyun_token";
static const char *LEGACY_XIAOZHI_WS_URL = "ws://123.60.62.147:8000/xiaozhi/v1/";

static void load_default_settings(ornament_settings_t *settings)
{
    memset(settings, 0, sizeof(*settings));
    strlcpy(settings->ssid, CONFIG_ORNAMENT_WIFI_SSID, sizeof(settings->ssid));
    strlcpy(settings->password, CONFIG_ORNAMENT_WIFI_PASSWORD, sizeof(settings->password));
    strlcpy(settings->bridge_url, CONFIG_ORNAMENT_BRIDGE_URL, sizeof(settings->bridge_url));
    strlcpy(settings->xiaozhi_ws_url, CONFIG_ORNAMENT_XIAOZHI_WS_URL, sizeof(settings->xiaozhi_ws_url));
    strlcpy(settings->xiaozhi_token, CONFIG_ORNAMENT_XIAOZHI_TOKEN, sizeof(settings->xiaozhi_token));
    strlcpy(settings->weather_label, CONFIG_ORNAMENT_WEATHER_LABEL, sizeof(settings->weather_label));
    strlcpy(settings->weather_source, CONFIG_ORNAMENT_WEATHER_SOURCE, sizeof(settings->weather_source));
    settings->weather_lat_e6 = CONFIG_ORNAMENT_WEATHER_LAT_E6;
    settings->weather_lon_e6 = CONFIG_ORNAMENT_WEATHER_LON_E6;
    settings->audio_volume_percent = CONFIG_ORNAMENT_AUDIO_VOLUME_PERCENT;
    settings->has_wifi = settings->ssid[0] != '\0';
    settings->has_bridge_url = settings->bridge_url[0] != '\0';
    settings->has_xiaozhi_ws_url = settings->xiaozhi_ws_url[0] != '\0';
    settings->has_xiaozhi_token = settings->xiaozhi_token[0] != '\0';
    settings->has_caiyun_token = false;
}

static void read_nvs_string(nvs_handle_t handle, const char *key, char *target, size_t target_size)
{
    size_t required = target_size;
    esp_err_t err = nvs_get_str(handle, key, target, &required);
    if (err != ESP_OK && target_size > 0) {
        target[0] = '\0';
    }
}

static bool xiaozhi_ws_url_needs_migration(const char *ws_url)
{
    return ws_url != NULL &&
           ws_url[0] != '\0' &&
           strcmp(ws_url, LEGACY_XIAOZHI_WS_URL) == 0 &&
           strcmp(CONFIG_ORNAMENT_XIAOZHI_WS_URL, LEGACY_XIAOZHI_WS_URL) != 0;
}

esp_err_t settings_load(ornament_settings_t *settings)
{
    load_default_settings(settings);

    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    if (err != ESP_OK) {
        return err;
    }

    char ssid[sizeof(settings->ssid)] = {0};
    char password[sizeof(settings->password)] = {0};
    char bridge_url[sizeof(settings->bridge_url)] = {0};
    char xiaozhi_ws_url[sizeof(settings->xiaozhi_ws_url)] = {0};
    char xiaozhi_token[sizeof(settings->xiaozhi_token)] = {0};
    char weather_label[sizeof(settings->weather_label)] = {0};
    char weather_source[sizeof(settings->weather_source)] = {0};
    char caiyun_token[sizeof(settings->caiyun_token)] = {0};
    int32_t audio_volume_percent = settings->audio_volume_percent;
    int32_t weather_lat_e6 = settings->weather_lat_e6;
    int32_t weather_lon_e6 = settings->weather_lon_e6;
    read_nvs_string(handle, KEY_SSID, ssid, sizeof(ssid));
    read_nvs_string(handle, KEY_PASSWORD, password, sizeof(password));
    read_nvs_string(handle, KEY_BRIDGE_URL, bridge_url, sizeof(bridge_url));
    read_nvs_string(handle, KEY_XIAOZHI_WS_URL, xiaozhi_ws_url, sizeof(xiaozhi_ws_url));
    read_nvs_string(handle, KEY_XIAOZHI_TOKEN, xiaozhi_token, sizeof(xiaozhi_token));
    read_nvs_string(handle, KEY_WEATHER_LABEL, weather_label, sizeof(weather_label));
    read_nvs_string(handle, KEY_WEATHER_SOURCE, weather_source, sizeof(weather_source));
    read_nvs_string(handle, KEY_CAIYUN_TOKEN, caiyun_token, sizeof(caiyun_token));
    (void)nvs_get_i32(handle, KEY_AUDIO_VOLUME, &audio_volume_percent);
    (void)nvs_get_i32(handle, KEY_WEATHER_LAT_E6, &weather_lat_e6);
    (void)nvs_get_i32(handle, KEY_WEATHER_LON_E6, &weather_lon_e6);
    nvs_close(handle);

    if (ssid[0] != '\0') {
        strlcpy(settings->ssid, ssid, sizeof(settings->ssid));
        strlcpy(settings->password, password, sizeof(settings->password));
        settings->has_wifi = true;
    }
    if (bridge_url[0] != '\0') {
        strlcpy(settings->bridge_url, bridge_url, sizeof(settings->bridge_url));
        settings->has_bridge_url = true;
    }
    if (xiaozhi_ws_url[0] != '\0') {
        if (xiaozhi_ws_url_needs_migration(xiaozhi_ws_url)) {
            strlcpy(settings->xiaozhi_ws_url, CONFIG_ORNAMENT_XIAOZHI_WS_URL, sizeof(settings->xiaozhi_ws_url));
        } else {
            strlcpy(settings->xiaozhi_ws_url, xiaozhi_ws_url, sizeof(settings->xiaozhi_ws_url));
        }
        settings->has_xiaozhi_ws_url = true;
    }
    if (xiaozhi_token[0] != '\0') {
        strlcpy(settings->xiaozhi_token, xiaozhi_token, sizeof(settings->xiaozhi_token));
        settings->has_xiaozhi_token = true;
    }
    if (audio_volume_percent >= 0 && audio_volume_percent <= 100) {
        settings->audio_volume_percent = (int)audio_volume_percent;
    }
    if (weather_label[0] != '\0') {
        strlcpy(settings->weather_label, weather_label, sizeof(settings->weather_label));
    }
    if (weather_source[0] != '\0') {
        strlcpy(settings->weather_source, weather_source, sizeof(settings->weather_source));
    }
    if (weather_lat_e6 >= -90000000 && weather_lat_e6 <= 90000000) {
        settings->weather_lat_e6 = (int)weather_lat_e6;
    }
    if (weather_lon_e6 >= -180000000 && weather_lon_e6 <= 180000000) {
        settings->weather_lon_e6 = (int)weather_lon_e6;
    }
    if (caiyun_token[0] != '\0') {
        strlcpy(settings->caiyun_token, caiyun_token, sizeof(settings->caiyun_token));
        settings->has_caiyun_token = true;
    }

    return ESP_OK;
}

esp_err_t settings_save(const ornament_settings_t *settings)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_set_str(handle, KEY_SSID, settings->ssid);
    if (err == ESP_OK) {
        err = nvs_set_str(handle, KEY_PASSWORD, settings->password);
    }
    if (err == ESP_OK) {
        err = nvs_set_str(handle, KEY_BRIDGE_URL, settings_bridge_url_or_default(settings));
    }
    if (err == ESP_OK) {
        err = nvs_set_str(handle, KEY_XIAOZHI_WS_URL, settings_xiaozhi_ws_url_or_default(settings));
    }
    if (err == ESP_OK) {
        err = nvs_set_str(handle, KEY_XIAOZHI_TOKEN, settings_xiaozhi_token_or_default(settings));
    }
    if (err == ESP_OK) {
        err = nvs_set_i32(handle, KEY_AUDIO_VOLUME, settings_audio_volume_percent_or_default(settings));
    }
    if (err == ESP_OK) {
        err = nvs_set_str(handle, KEY_WEATHER_LABEL, settings_weather_label_or_default(settings));
    }
    if (err == ESP_OK) {
        err = nvs_set_str(handle, KEY_WEATHER_SOURCE, settings_weather_source_or_default(settings));
    }
    if (err == ESP_OK) {
        err = nvs_set_i32(handle, KEY_WEATHER_LAT_E6, settings->weather_lat_e6);
    }
    if (err == ESP_OK) {
        err = nvs_set_i32(handle, KEY_WEATHER_LON_E6, settings->weather_lon_e6);
    }
    if (err == ESP_OK) {
        if (settings->has_caiyun_token && settings->caiyun_token[0] != '\0') {
            err = nvs_set_str(handle, KEY_CAIYUN_TOKEN, settings->caiyun_token);
        } else {
            esp_err_t erase_err = nvs_erase_key(handle, KEY_CAIYUN_TOKEN);
            if (erase_err != ESP_OK && erase_err != ESP_ERR_NVS_NOT_FOUND) {
                err = erase_err;
            }
        }
    }
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }

    nvs_close(handle);
    return err;
}

esp_err_t settings_clear(void)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_erase_all(handle);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}

const char *settings_bridge_url_or_default(const ornament_settings_t *settings)
{
    if (settings != NULL && settings->bridge_url[0] != '\0') {
        return settings->bridge_url;
    }
    return CONFIG_ORNAMENT_BRIDGE_URL;
}

const char *settings_xiaozhi_ws_url_or_default(const ornament_settings_t *settings)
{
    if (settings != NULL && settings->xiaozhi_ws_url[0] != '\0') {
        return settings->xiaozhi_ws_url;
    }
    return CONFIG_ORNAMENT_XIAOZHI_WS_URL;
}

const char *settings_xiaozhi_token_or_default(const ornament_settings_t *settings)
{
    if (settings != NULL && settings->xiaozhi_token[0] != '\0') {
        return settings->xiaozhi_token;
    }
    return CONFIG_ORNAMENT_XIAOZHI_TOKEN;
}

int settings_audio_volume_percent_or_default(const ornament_settings_t *settings)
{
    if (settings != NULL && settings->audio_volume_percent >= 0 && settings->audio_volume_percent <= 100) {
        return settings->audio_volume_percent;
    }
    return CONFIG_ORNAMENT_AUDIO_VOLUME_PERCENT;
}

const char *settings_weather_label_or_default(const ornament_settings_t *settings)
{
    if (settings != NULL && settings->weather_label[0] != '\0') {
        return settings->weather_label;
    }
    return CONFIG_ORNAMENT_WEATHER_LABEL;
}

const char *settings_weather_source_or_default(const ornament_settings_t *settings)
{
    const char *source = settings != NULL && settings->weather_source[0] != '\0' ?
        settings->weather_source : CONFIG_ORNAMENT_WEATHER_SOURCE;
    if (strcmp(source, ORNAMENT_WEATHER_SOURCE_OPEN_METEO) == 0) {
        return ORNAMENT_WEATHER_SOURCE_OPEN_METEO;
    }
    return ORNAMENT_WEATHER_SOURCE_CAIYUN;
}
