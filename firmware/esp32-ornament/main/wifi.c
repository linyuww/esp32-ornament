#include "wifi.h"

#include "config_portal.h"
#include "device_identity.h"
#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_ip_addr.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "settings.h"

#include <string.h>

static const char *TAG = "wifi";
static EventGroupHandle_t wifi_event_group;
static const int WIFI_CONNECTED_BIT = BIT0;
static const int WIFI_FAIL_BIT = BIT1;
static int retry_count;
static bool wifi_runtime_ready;
static bool sta_connect_active;
static uint8_t last_disconnect_reason;
static int8_t last_disconnect_rssi;
static esp_netif_t *sta_netif;

static const char *wifi_disconnect_reason_name(uint8_t reason)
{
    switch (reason) {
    case WIFI_REASON_AUTH_EXPIRE:
        return "AUTH_EXPIRE";
    case WIFI_REASON_AUTH_LEAVE:
        return "AUTH_LEAVE";
    case WIFI_REASON_DISASSOC_DUE_TO_INACTIVITY:
        return "DISASSOC_DUE_TO_INACTIVITY";
    case WIFI_REASON_ASSOC_TOOMANY:
        return "ASSOC_TOOMANY";
    case WIFI_REASON_CLASS2_FRAME_FROM_NONAUTH_STA:
        return "CLASS2_FRAME_FROM_NONAUTH_STA";
    case WIFI_REASON_CLASS3_FRAME_FROM_NONASSOC_STA:
        return "CLASS3_FRAME_FROM_NONASSOC_STA";
    case WIFI_REASON_ASSOC_LEAVE:
        return "ASSOC_LEAVE";
    case WIFI_REASON_ASSOC_NOT_AUTHED:
        return "ASSOC_NOT_AUTHED";
    case WIFI_REASON_DISASSOC_PWRCAP_BAD:
        return "DISASSOC_PWRCAP_BAD";
    case WIFI_REASON_DISASSOC_SUPCHAN_BAD:
        return "DISASSOC_SUPCHAN_BAD";
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
        return "4WAY_HANDSHAKE_TIMEOUT";
    case WIFI_REASON_HANDSHAKE_TIMEOUT:
        return "HANDSHAKE_TIMEOUT";
    case WIFI_REASON_AUTH_FAIL:
        return "AUTH_FAIL";
    case WIFI_REASON_ASSOC_FAIL:
        return "ASSOC_FAIL";
    case WIFI_REASON_BEACON_TIMEOUT:
        return "BEACON_TIMEOUT";
    case WIFI_REASON_NO_AP_FOUND:
        return "NO_AP_FOUND";
    case WIFI_REASON_CONNECTION_FAIL:
        return "CONNECTION_FAIL";
    case WIFI_REASON_AP_INITIATED:
        return "AP_INITIATED";
    case WIFI_REASON_PEER_INITIATED:
        return "PEER_INITIATED";
    default:
        return "UNKNOWN";
    }
}

static void copy_event_ssid(const uint8_t *ssid, uint8_t ssid_len, char *target, size_t target_size)
{
    if (target_size == 0) {
        return;
    }
    size_t copy_len = ssid_len;
    if (copy_len >= target_size) {
        copy_len = target_size - 1;
    }
    memcpy(target, ssid, copy_len);
    target[copy_len] = '\0';
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        if (!sta_connect_active) {
            return;
        }
        esp_err_t err = esp_wifi_connect();
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "STA start connect failed: %s", esp_err_to_name(err));
        }
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_CONNECTED) {
        const wifi_event_sta_connected_t *event = (const wifi_event_sta_connected_t *)event_data;
        char ssid[33] = {0};
        copy_event_ssid(event->ssid, event->ssid_len, ssid, sizeof(ssid));
        ESP_LOGI(
            TAG,
            "STA connected: ssid=%s channel=%u authmode=%d bssid=%02x:%02x:%02x:%02x:%02x:%02x",
            ssid,
            event->channel,
            event->authmode,
            event->bssid[0],
            event->bssid[1],
            event->bssid[2],
            event->bssid[3],
            event->bssid[4],
            event->bssid[5]);
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *event = (const wifi_event_sta_disconnected_t *)event_data;
        if (!sta_connect_active) {
            ESP_LOGD(TAG, "STA disconnected outside normal connect: reason=%u rssi=%d", event->reason, event->rssi);
            return;
        }
        char ssid[33] = {0};
        copy_event_ssid(event->ssid, event->ssid_len, ssid, sizeof(ssid));
        last_disconnect_reason = event->reason;
        last_disconnect_rssi = event->rssi;
        ESP_LOGW(
            TAG,
            "STA disconnected: ssid=%s reason=%u(%s) rssi=%d retry=%d/8",
            ssid,
            event->reason,
            wifi_disconnect_reason_name(event->reason),
            event->rssi,
            retry_count);
        if (retry_count < 8) {
            retry_count++;
            esp_err_t err = esp_wifi_connect();
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "STA reconnect failed: %s", esp_err_to_name(err));
            }
        } else {
            xEventGroupSetBits(wifi_event_group, WIFI_FAIL_BIT);
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *event = (const ip_event_got_ip_t *)event_data;
        retry_count = 0;
        last_disconnect_reason = 0;
        last_disconnect_rssi = 0;
        if (!sta_connect_active) {
            return;
        }
        ESP_LOGI(TAG, "STA got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        xEventGroupSetBits(wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

static esp_err_t wifi_runtime_init(void)
{
    if (wifi_runtime_ready) {
        return ESP_OK;
    }

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    sta_netif = esp_netif_create_default_wifi_sta();
    if (sta_netif == NULL) {
        return ESP_ERR_NO_MEM;
    }
    char hostname[ORNAMENT_HOSTNAME_MAX];
    device_identity_hostname(hostname, sizeof(hostname));
    ESP_ERROR_CHECK(esp_netif_set_hostname(sta_netif, hostname));
    ESP_LOGI(TAG, "STA hostname: %s.local", hostname);
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t init_config = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init_config));

    esp_event_handler_instance_t any_id;
    esp_event_handler_instance_t got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, &any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, &got_ip));

    wifi_runtime_ready = true;
    return ESP_OK;
}

static esp_err_t enter_config_portal(const char *reason)
{
    ESP_LOGW(TAG, "starting config portal: %s", reason);
    sta_connect_active = false;
    esp_err_t err = config_portal_start();
    if (err == ESP_OK) {
        ESP_LOGW(TAG, "connect phone to SSID=%s and open http://192.168.4.1", config_portal_ssid());
    }
    return err == ESP_OK ? ESP_ERR_WIFI_NOT_CONNECT : err;
}

esp_err_t wifi_connect(void)
{
    ESP_RETURN_ON_ERROR(wifi_runtime_init(), TAG, "wifi runtime init failed");

    if (wifi_event_group == NULL) {
        wifi_event_group = xEventGroupCreate();
        if (wifi_event_group == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }

    ornament_settings_t settings;
    ESP_RETURN_ON_ERROR(settings_load(&settings), TAG, "settings load failed");
    if (!settings.has_wifi) {
        return enter_config_portal("missing Wi-Fi settings");
    }

    xEventGroupClearBits(wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);
    retry_count = 0;
    last_disconnect_reason = 0;
    last_disconnect_rssi = 0;
    sta_connect_active = true;

    wifi_config_t wifi_config = {0};
    strlcpy((char *)wifi_config.sta.ssid, settings.ssid, sizeof(wifi_config.sta.ssid));
    strlcpy((char *)wifi_config.sta.password, settings.password, sizeof(wifi_config.sta.password));
    wifi_config.sta.threshold.authmode = WIFI_AUTH_OPEN;
    wifi_config.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
    ESP_LOGI(
        TAG,
        "connecting to Wi-Fi SSID=%s password_len=%u timeout_ms=%d",
        settings.ssid,
        (unsigned)strlen(settings.password),
        CONFIG_ORNAMENT_CONNECT_TIMEOUT_MS);

    EventBits_t bits = xEventGroupWaitBits(
        wifi_event_group,
        WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
        pdFALSE,
        pdFALSE,
        pdMS_TO_TICKS(CONFIG_ORNAMENT_CONNECT_TIMEOUT_MS));

    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "connected to Wi-Fi SSID=%s", settings.ssid);
        return ESP_OK;
    }

    ESP_LOGW(
        TAG,
        "Wi-Fi connect did not get IP: bits=0x%lx retries=%d last_reason=%u(%s) last_rssi=%d",
        (unsigned long)bits,
        retry_count,
        last_disconnect_reason,
        wifi_disconnect_reason_name(last_disconnect_reason),
        last_disconnect_rssi);
    sta_connect_active = false;
    return enter_config_portal("Wi-Fi connection failed");
}

void wifi_status_snapshot(bool *connected, char *ssid, size_t ssid_size, int *rssi)
{
    wifi_ap_record_t ap_info = {0};
    bool is_connected = esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK;

    if (connected != NULL) {
        *connected = is_connected;
    }
    if (ssid != NULL && ssid_size > 0) {
        if (is_connected) {
            strlcpy(ssid, (const char *)ap_info.ssid, ssid_size);
        } else {
            ssid[0] = '\0';
        }
    }
    if (rssi != NULL) {
        *rssi = is_connected ? ap_info.rssi : 0;
    }
}
