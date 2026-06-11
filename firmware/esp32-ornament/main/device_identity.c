#include "device_identity.h"

#include "esp_log.h"
#include "esp_mac.h"

#ifndef CONFIG_ORNAMENT_MDNS_ENABLED
#define CONFIG_ORNAMENT_MDNS_ENABLED 0
#endif

#if CONFIG_ORNAMENT_MDNS_ENABLED
#include "mdns.h"
#endif

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

static const char *TAG = "device_identity";
static char hostname[32];
static char mdns_url[64];
static bool identity_ready;
static bool mdns_started;

static void build_hostname(void)
{
    uint8_t mac[6] = {0};
    esp_err_t err = esp_read_mac(mac, ESP_MAC_WIFI_STA);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "failed to read STA MAC for hostname: %s", esp_err_to_name(err));
        strlcpy(hostname, CONFIG_ORNAMENT_HOSTNAME_PREFIX, sizeof(hostname));
        return;
    }

    snprintf(
        hostname,
        sizeof(hostname),
        "%s-%02x%02x",
        CONFIG_ORNAMENT_HOSTNAME_PREFIX,
        mac[4],
        mac[5]);
}

const char *device_identity_hostname(void)
{
    if (!identity_ready) {
        (void)device_identity_init();
    }
    return hostname;
}

const char *device_identity_mdns_url(void)
{
    if (!identity_ready) {
        (void)device_identity_init();
    }
    return mdns_url;
}

esp_err_t device_identity_init(void)
{
    if (identity_ready) {
        return ESP_OK;
    }

    build_hostname();
    snprintf(mdns_url, sizeof(mdns_url), "http://%s.local/", hostname);
    identity_ready = true;
    ESP_LOGI(TAG, "hostname=%s mdns_url=%s", hostname, mdns_url);
    return ESP_OK;
}

esp_err_t device_identity_start_mdns(void)
{
    ESP_ERROR_CHECK_WITHOUT_ABORT(device_identity_init());

    if (mdns_started) {
        return ESP_OK;
    }

#if CONFIG_ORNAMENT_MDNS_ENABLED
    esp_err_t err = mdns_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "mDNS init failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_ERROR_CHECK_WITHOUT_ABORT(mdns_hostname_set(hostname));
    ESP_ERROR_CHECK_WITHOUT_ABORT(mdns_instance_name_set("Codex Ornament"));
    ESP_ERROR_CHECK_WITHOUT_ABORT(mdns_service_add("Codex Ornament Web Console", "_http", "_tcp", 80, NULL, 0));

    mdns_started = true;
    ESP_LOGI(TAG, "mDNS started: %s", mdns_url);
#endif
    return ESP_OK;
}
