#include "config_portal.h"

#include "bridge_client.h"
#include "esp_check.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "settings.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "config_portal";
static httpd_handle_t server;
static char ap_ssid[33];
static bool sta_event_handlers_registered;
static EventGroupHandle_t sta_probe_events;
static const int STA_PROBE_CONNECTED_BIT = BIT0;
static const int STA_PROBE_DISCONNECTED_BIT = BIT1;

const char *config_portal_ssid(void)
{
    return ap_ssid[0] != '\0' ? ap_ssid : CONFIG_ORNAMENT_PROV_AP_PREFIX;
}

static void append(char *html, size_t html_size, size_t *used, const char *text)
{
    if (*used >= html_size) {
        return;
    }
    int written = snprintf(html + *used, html_size - *used, "%s", text);
    if (written > 0) {
        *used += (size_t)written;
        if (*used >= html_size) {
            *used = html_size - 1;
        }
    }
}

static void appendf(char *html, size_t html_size, size_t *used, const char *format, ...)
{
    if (*used >= html_size) {
        return;
    }
    va_list args;
    va_start(args, format);
    int written = vsnprintf(html + *used, html_size - *used, format, args);
    va_end(args);
    if (written > 0) {
        *used += (size_t)written;
        if (*used >= html_size) {
            *used = html_size - 1;
        }
    }
}

static void html_escape(const char *input, char *output, size_t output_size)
{
    size_t used = 0;
    for (const char *cursor = input; *cursor != '\0' && used + 1 < output_size; cursor++) {
        const char *escaped = NULL;
        switch (*cursor) {
        case '&':
            escaped = "&amp;";
            break;
        case '<':
            escaped = "&lt;";
            break;
        case '>':
            escaped = "&gt;";
            break;
        case '"':
            escaped = "&quot;";
            break;
        default:
            break;
        }

        if (escaped != NULL) {
            size_t len = strlen(escaped);
            if (used + len >= output_size) {
                break;
            }
            memcpy(output + used, escaped, len);
            used += len;
        } else {
            output[used++] = *cursor;
        }
    }
    output[used] = '\0';
}

static uint16_t scan_wifi_networks(wifi_ap_record_t *records, uint16_t max_records)
{
    wifi_scan_config_t scan_config = {
        .show_hidden = false,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
    };
    esp_err_t err = esp_wifi_scan_start(&scan_config, true);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Wi-Fi scan failed: %s", esp_err_to_name(err));
        return 0;
    }

    uint16_t count = max_records;
    err = esp_wifi_scan_get_ap_records(&count, records);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "failed to read scan results: %s", esp_err_to_name(err));
        return 0;
    }
    return count;
}

static void sta_probe_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    (void)arg;
    (void)event_data;
    if (sta_probe_events == NULL) {
        return;
    }
    if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        xEventGroupSetBits(sta_probe_events, STA_PROBE_CONNECTED_BIT);
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupSetBits(sta_probe_events, STA_PROBE_DISCONNECTED_BIT);
    }
}

static char *render_config_form(void)
{
    const size_t html_size = 8192;
    char *html = calloc(1, html_size);
    if (html == NULL) {
        return NULL;
    }

    wifi_ap_record_t records[CONFIG_ORNAMENT_PROV_SCAN_MAX] = {0};
    uint16_t count = scan_wifi_networks(records, CONFIG_ORNAMENT_PROV_SCAN_MAX);
    size_t used = 0;

    append(
        html,
        html_size,
        &used,
        "<!doctype html>"
        "<html><head><meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
        "<title>Codex Ornament Setup</title>"
        "<style>"
        "body{font-family:system-ui,-apple-system,Segoe UI,sans-serif;margin:24px;background:#101418;color:#eef3f8}"
        "main{max-width:480px;margin:auto}"
        "label{display:block;margin:14px 0 6px;color:#aeb9c5}"
        "input,select{box-sizing:border-box;width:100%;padding:12px;border-radius:8px;border:1px solid #3b4752;background:#18212a;color:#fff}"
        "button,.link{margin-top:18px;width:100%;padding:12px;border:0;border-radius:8px;background:#4fb477;color:#07110b;font-weight:700;text-align:center;display:block;text-decoration:none}"
        "p{line-height:1.5;color:#c8d2dc}"
        ".muted{font-size:13px;color:#8d9aa7}"
        "</style></head>"
        "<body><main>"
        "<h1>Codex Ornament Setup</h1>"
        "<p>Choose your Wi-Fi, enter its password, and set the PC bridge state URL. Use Test to connect once and verify the bridge before saving.</p>"
        "<form method=\"post\" action=\"/save\">");

    if (count > 0) {
        append(html, html_size, &used, "<label>Wi-Fi SSID</label><select name=\"ssid\" required>");
        for (uint16_t i = 0; i < count; i++) {
            char ssid[65] = {0};
            char escaped[160] = {0};
            strlcpy(ssid, (const char *)records[i].ssid, sizeof(ssid));
            html_escape(ssid, escaped, sizeof(escaped));
            appendf(
                html,
                html_size,
                &used,
                "<option value=\"%s\">%s (%d dBm)</option>",
                escaped,
                escaped,
                records[i].rssi);
        }
        append(html, html_size, &used, "</select>");
    } else {
        append(
            html,
            html_size,
            &used,
            "<label>Wi-Fi SSID</label><input name=\"ssid\" maxlength=\"32\" required>"
            "<p class=\"muted\">No Wi-Fi networks were found. Enter SSID manually or refresh.</p>");
    }

    append(
        html,
        html_size,
        &used,
        "<label>Wi-Fi Password</label><input name=\"password\" maxlength=\"64\" type=\"password\">"
        "<label>Bridge State URL</label><input name=\"bridge_url\" maxlength=\"159\" value=\"http://192.168.1.100:8787/state\" required>"
        "<button type=\"submit\">Save and restart</button>"
        "<button type=\"submit\" formaction=\"/test-bridge\">Test Wi-Fi and Bridge URL</button>"
        "</form>"
        "<a class=\"link\" href=\"/\">Rescan Wi-Fi</a>"
        "<p class=\"muted\">After saving, reconnect your phone to the normal Wi-Fi. The ornament will reboot.</p>"
        "</main></body></html>");

    return html;
}

static int hex_value(char value)
{
    if (value >= '0' && value <= '9') {
        return value - '0';
    }
    if (value >= 'a' && value <= 'f') {
        return value - 'a' + 10;
    }
    if (value >= 'A' && value <= 'F') {
        return value - 'A' + 10;
    }
    return -1;
}

static void url_decode(char *value)
{
    char *read = value;
    char *write = value;
    while (*read != '\0') {
        if (*read == '+') {
            *write++ = ' ';
            read++;
        } else if (*read == '%' && isxdigit((unsigned char)read[1]) && isxdigit((unsigned char)read[2])) {
            *write++ = (char)((hex_value(read[1]) << 4) | hex_value(read[2]));
            read += 3;
        } else {
            *write++ = *read++;
        }
    }
    *write = '\0';
}

static void form_value(const char *body, const char *name, char *target, size_t target_size)
{
    if (target_size == 0) {
        return;
    }
    target[0] = '\0';
    size_t name_len = strlen(name);
    const char *cursor = body;
    while (cursor != NULL && *cursor != '\0') {
        const char *next = strchr(cursor, '&');
        size_t pair_len = next == NULL ? strlen(cursor) : (size_t)(next - cursor);
        const char *equals = memchr(cursor, '=', pair_len);
        if (equals != NULL && (size_t)(equals - cursor) == name_len && strncmp(cursor, name, name_len) == 0) {
            size_t value_len = pair_len - name_len - 1;
            if (value_len >= target_size) {
                value_len = target_size - 1;
            }
            memcpy(target, equals + 1, value_len);
            target[value_len] = '\0';
            url_decode(target);
            return;
        }
        cursor = next == NULL ? NULL : next + 1;
    }
}

static esp_err_t read_form_body(httpd_req_t *req, char *body, size_t body_size)
{
    if (req->content_len <= 0 || req->content_len >= body_size) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid form size");
        return ESP_FAIL;
    }

    int received = 0;
    while (received < req->content_len) {
        int ret = httpd_req_recv(req, body + received, req->content_len - received);
        if (ret <= 0) {
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to read request");
            return ESP_FAIL;
        }
        received += ret;
    }
    body[received] = '\0';
    return ESP_OK;
}

static esp_err_t root_get_handler(httpd_req_t *req)
{
    char *html = render_config_form();
    if (html == NULL) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "text/html; charset=utf-8");
    esp_err_t err = httpd_resp_send(req, html, HTTPD_RESP_USE_STRLEN);
    free(html);
    return err;
}

static esp_err_t save_post_handler(httpd_req_t *req)
{
    char body[513] = {0};
    if (read_form_body(req, body, sizeof(body)) != ESP_OK) {
        return ESP_FAIL;
    }

    ornament_settings_t settings = {0};
    form_value(body, "ssid", settings.ssid, sizeof(settings.ssid));
    form_value(body, "password", settings.password, sizeof(settings.password));
    form_value(body, "bridge_url", settings.bridge_url, sizeof(settings.bridge_url));
    settings.has_wifi = settings.ssid[0] != '\0';
    settings.has_bridge_url = settings.bridge_url[0] != '\0';

    if (!settings.has_wifi || !settings.has_bridge_url) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "SSID and bridge URL are required");
        return ESP_FAIL;
    }

    esp_err_t err = settings_save(&settings);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "failed to save settings: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to save settings");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_sendstr(req, "<html><body><h1>Saved</h1><p>Device will restart now.</p></body></html>");
    vTaskDelay(pdMS_TO_TICKS(700));
    esp_restart();
    return ESP_OK;
}

static esp_err_t connect_sta_for_probe(const char *ssid, const char *password, int timeout_ms)
{
    if (ssid == NULL || ssid[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    if (sta_probe_events == NULL) {
        sta_probe_events = xEventGroupCreate();
        if (sta_probe_events == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }
    if (!sta_event_handlers_registered) {
        ESP_ERROR_CHECK(esp_event_handler_instance_register(
            IP_EVENT, IP_EVENT_STA_GOT_IP, &sta_probe_event_handler, NULL, NULL));
        ESP_ERROR_CHECK(esp_event_handler_instance_register(
            WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, &sta_probe_event_handler, NULL, NULL));
        sta_event_handlers_registered = true;
    }

    (void)esp_wifi_disconnect();
    vTaskDelay(pdMS_TO_TICKS(250));
    xEventGroupClearBits(sta_probe_events, STA_PROBE_CONNECTED_BIT | STA_PROBE_DISCONNECTED_BIT);

    wifi_config_t wifi_config = {0};
    strlcpy((char *)wifi_config.sta.ssid, ssid, sizeof(wifi_config.sta.ssid));
    strlcpy((char *)wifi_config.sta.password, password != NULL ? password : "", sizeof(wifi_config.sta.password));
    wifi_config.sta.threshold.authmode = WIFI_AUTH_OPEN;
    wifi_config.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;

    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &wifi_config), TAG, "set STA config failed");
    ESP_RETURN_ON_ERROR(esp_wifi_connect(), TAG, "STA connect failed");

    EventBits_t bits = xEventGroupWaitBits(
        sta_probe_events,
        STA_PROBE_CONNECTED_BIT | STA_PROBE_DISCONNECTED_BIT,
        pdFALSE,
        pdFALSE,
        pdMS_TO_TICKS(timeout_ms));
    if (bits & STA_PROBE_CONNECTED_BIT) {
        return ESP_OK;
    }
    if (bits & STA_PROBE_DISCONNECTED_BIT) {
        return ESP_FAIL;
    }
    return ESP_ERR_TIMEOUT;
}

static esp_err_t test_bridge_post_handler(httpd_req_t *req)
{
    char body[512] = {0};
    char ssid[ORNAMENT_WIFI_SSID_MAX + 1] = {0};
    char password[ORNAMENT_WIFI_PASSWORD_MAX + 1] = {0};
    char url[ORNAMENT_BRIDGE_URL_MAX] = {0};
    if (read_form_body(req, body, sizeof(body)) != ESP_OK) {
        return ESP_FAIL;
    }
    form_value(body, "ssid", ssid, sizeof(ssid));
    form_value(body, "password", password, sizeof(password));
    form_value(body, "bridge_url", url, sizeof(url));

    esp_err_t wifi_err = connect_sta_for_probe(ssid, password, CONFIG_ORNAMENT_CONNECT_TIMEOUT_MS);
    bridge_probe_result_t result = {0};
    esp_err_t bridge_err = wifi_err == ESP_OK ? bridge_client_probe_url(url, &result) : wifi_err;

    char escaped[ORNAMENT_BRIDGE_URL_MAX * 2] = {0};
    html_escape(url, escaped, sizeof(escaped));
    char html[1024];
    snprintf(
        html,
        sizeof(html),
        "<!doctype html><html><head><meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
        "<style>body{font-family:system-ui;margin:24px;background:#101418;color:#eef3f8}a{color:#4fb477}code{word-break:break-all}</style>"
        "</head><body><h1>Bridge Test</h1><p><code>%s</code></p>"
        "<p>Wi-Fi: %s</p><p>Bridge: %s</p><p>HTTP: %d, bytes: %d, JSON: %s, status: %s</p>"
        "<p>If Wi-Fi is ok but Bridge fails, check that the URL uses the PC LAN IP and Windows firewall allows port 8787.</p>"
        "<p><a href=\"/\">Back</a></p></body></html>",
        escaped,
        esp_err_to_name(wifi_err),
        esp_err_to_name(bridge_err),
        result.http_status,
        result.response_bytes,
        result.json_ok ? "ok" : "invalid",
        result.status_text);

    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, html, HTTPD_RESP_USE_STRLEN);
}

static void build_ap_ssid(void)
{
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    snprintf(ap_ssid, sizeof(ap_ssid), "%s-%02X%02X", CONFIG_ORNAMENT_PROV_AP_PREFIX, mac[4], mac[5]);
}

static esp_err_t start_http_server(void)
{
    if (server != NULL) {
        return ESP_OK;
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;
    config.lru_purge_enable = true;

    esp_err_t err = httpd_start(&server, &config);
    if (err != ESP_OK) {
        return err;
    }

    const httpd_uri_t root = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = root_get_handler,
    };
    const httpd_uri_t save = {
        .uri = "/save",
        .method = HTTP_POST,
        .handler = save_post_handler,
    };
    const httpd_uri_t test_bridge = {
        .uri = "/test-bridge",
        .method = HTTP_POST,
        .handler = test_bridge_post_handler,
    };
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &root));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &save));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &test_bridge));
    return ESP_OK;
}

esp_err_t config_portal_start(void)
{
    build_ap_ssid();
    wifi_config_t wifi_config = {0};
    strlcpy((char *)wifi_config.ap.ssid, ap_ssid, sizeof(wifi_config.ap.ssid));
    wifi_config.ap.ssid_len = strlen(ap_ssid);
    wifi_config.ap.channel = CONFIG_ORNAMENT_PROV_AP_CHANNEL;
    wifi_config.ap.max_connection = 4;
    wifi_config.ap.pmf_cfg.required = false;

    const char *password = CONFIG_ORNAMENT_PROV_AP_PASSWORD;
    if (password[0] != '\0') {
        strlcpy((char *)wifi_config.ap.password, password, sizeof(wifi_config.ap.password));
        wifi_config.ap.authmode = WIFI_AUTH_WPA_WPA2_PSK;
    } else {
        wifi_config.ap.authmode = WIFI_AUTH_OPEN;
    }

    esp_err_t stop_err = esp_wifi_stop();
    if (stop_err != ESP_OK && stop_err != ESP_ERR_WIFI_NOT_STARTED) {
        return stop_err;
    }
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGW(TAG, "provisioning portal started. SSID=%s URL=http://192.168.4.1", ap_ssid);
    return start_http_server();
}
