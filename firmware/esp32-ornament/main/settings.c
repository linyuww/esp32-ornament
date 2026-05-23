#include "settings.h"

#include "nvs.h"

#include <string.h>

static const char *NVS_NAMESPACE = "ornament";
static const char *KEY_SSID = "ssid";
static const char *KEY_PASSWORD = "password";
static const char *KEY_BRIDGE_URL = "bridge_url";

static void load_default_settings(ornament_settings_t *settings)
{
    memset(settings, 0, sizeof(*settings));
    strlcpy(settings->ssid, CONFIG_ORNAMENT_WIFI_SSID, sizeof(settings->ssid));
    strlcpy(settings->password, CONFIG_ORNAMENT_WIFI_PASSWORD, sizeof(settings->password));
    strlcpy(settings->bridge_url, CONFIG_ORNAMENT_BRIDGE_URL, sizeof(settings->bridge_url));
    settings->has_wifi = settings->ssid[0] != '\0';
    settings->has_bridge_url = settings->bridge_url[0] != '\0';
}

static void read_nvs_string(nvs_handle_t handle, const char *key, char *target, size_t target_size)
{
    size_t required = target_size;
    esp_err_t err = nvs_get_str(handle, key, target, &required);
    if (err != ESP_OK && target_size > 0) {
        target[0] = '\0';
    }
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
    read_nvs_string(handle, KEY_SSID, ssid, sizeof(ssid));
    read_nvs_string(handle, KEY_PASSWORD, password, sizeof(password));
    read_nvs_string(handle, KEY_BRIDGE_URL, bridge_url, sizeof(bridge_url));
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
