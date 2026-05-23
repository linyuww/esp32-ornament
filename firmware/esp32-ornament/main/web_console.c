#include "web_console.h"

#include "bridge_client.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "settings.h"
#include "wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "web_console";
static httpd_handle_t server;
static SemaphoreHandle_t state_mutex;
static ornament_state_t last_state;
static esp_err_t last_fetch_error = ESP_ERR_INVALID_STATE;
static int64_t last_state_us;

static void append(char *text, size_t text_size, size_t *used, const char *chunk)
{
    if (*used >= text_size) {
        return;
    }
    int written = snprintf(text + *used, text_size - *used, "%s", chunk);
    if (written > 0) {
        *used += (size_t)written;
        if (*used >= text_size) {
            *used = text_size - 1;
        }
    }
}

static void appendf(char *text, size_t text_size, size_t *used, const char *format, ...)
{
    if (*used >= text_size) {
        return;
    }
    va_list args;
    va_start(args, format);
    int written = vsnprintf(text + *used, text_size - *used, format, args);
    va_end(args);
    if (written > 0) {
        *used += (size_t)written;
        if (*used >= text_size) {
            *used = text_size - 1;
        }
    }
}

static void json_escape(const char *input, char *output, size_t output_size)
{
    size_t used = 0;
    if (output_size == 0) {
        return;
    }

    for (const char *cursor = input; cursor != NULL && *cursor != '\0' && used + 1 < output_size; cursor++) {
        if ((*cursor == '"' || *cursor == '\\') && used + 2 < output_size) {
            output[used++] = '\\';
            output[used++] = *cursor;
        } else if ((unsigned char)*cursor >= 0x20) {
            output[used++] = *cursor;
        }
    }
    output[used] = '\0';
}

static void html_escape(const char *input, char *output, size_t output_size)
{
    size_t used = 0;
    if (output_size == 0) {
        return;
    }

    for (const char *cursor = input; cursor != NULL && *cursor != '\0' && used + 1 < output_size; cursor++) {
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

static void status_label(ornament_status_t status, char *out, size_t out_size)
{
    const char *label = "idle";
    switch (status) {
    case ORNAMENT_STATUS_RUNNING:
        label = "running";
        break;
    case ORNAMENT_STATUS_DONE:
        label = "done";
        break;
    case ORNAMENT_STATUS_EVENT:
        label = "event";
        break;
    case ORNAMENT_STATUS_ERROR:
        label = "error";
        break;
    case ORNAMENT_STATUS_IDLE:
    default:
        label = "idle";
        break;
    }
    strlcpy(out, label, out_size);
}

static void state_snapshot(ornament_state_t *state, esp_err_t *fetch_error, int64_t *age_ms)
{
    int64_t updated_us = 0;
    if (state_mutex != NULL && xSemaphoreTake(state_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        *state = last_state;
        *fetch_error = last_fetch_error;
        updated_us = last_state_us;
        xSemaphoreGive(state_mutex);
    } else {
        *fetch_error = ESP_ERR_TIMEOUT;
    }

    if (updated_us > 0) {
        *age_ms = (esp_timer_get_time() - updated_us) / 1000;
    } else {
        *age_ms = -1;
    }
}

void web_console_set_last_state(const ornament_state_t *state, esp_err_t fetch_error)
{
    if (state == NULL) {
        return;
    }
    if (state_mutex != NULL && xSemaphoreTake(state_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        last_state = *state;
        last_fetch_error = fetch_error;
        last_state_us = esp_timer_get_time();
        xSemaphoreGive(state_mutex);
    }
}

static esp_err_t status_get_handler(httpd_req_t *req)
{
    ornament_state_t state;
    esp_err_t fetch_error = ESP_OK;
    int64_t age_ms = -1;
    char status[16];
    char task_title[ORNAMENT_TEXT_MAX * 2];
    char task_message[ORNAMENT_TEXT_MAX * 2];
    char quota_status[64];
    char wifi_ssid[80];
    char bridge_url[ORNAMENT_BRIDGE_URL_MAX * 2];
    ornament_settings_t settings;

    ornament_state_init(&state);
    state_snapshot(&state, &fetch_error, &age_ms);
    status_label(state.status, status, sizeof(status));
    json_escape(state.task_title, task_title, sizeof(task_title));
    json_escape(state.task_message, task_message, sizeof(task_message));
    json_escape(state.quota_status, quota_status, sizeof(quota_status));
    json_escape(state.wifi_ssid, wifi_ssid, sizeof(wifi_ssid));

    if (settings_load(&settings) != ESP_OK) {
        memset(&settings, 0, sizeof(settings));
    }
    json_escape(settings_bridge_url_or_default(&settings), bridge_url, sizeof(bridge_url));

    char *json = calloc(1, 2048);
    if (json == NULL) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
        return ESP_FAIL;
    }

    snprintf(
        json,
        2048,
        "{"
        "\"uptime_ms\":%lld,"
        "\"last_state_age_ms\":%lld,"
        "\"fetch_error\":\"%s\","
        "\"bridge_url\":\"%s\","
        "\"wifi\":{\"connected\":%s,\"ssid\":\"%s\",\"rssi\":%d},"
        "\"time\":{\"synced\":%s,\"local_time\":\"%s\",\"local_date\":\"%s\"},"
        "\"quota\":{\"has\":%s,\"status\":\"%s\",\"primary\":%d,\"weekly\":%d},"
        "\"task\":{\"has\":%s,\"status\":\"%s\",\"title\":\"%s\",\"message\":\"%s\"},"
        "\"heap\":{\"free\":%u,\"min_free\":%u,\"largest_free_block\":%u}"
        "}",
        (long long)(esp_timer_get_time() / 1000),
        (long long)age_ms,
        esp_err_to_name(fetch_error),
        bridge_url,
        state.wifi_connected ? "true" : "false",
        wifi_ssid,
        state.wifi_rssi,
        state.time_synced ? "true" : "false",
        state.local_time,
        state.local_date,
        state.has_quota ? "true" : "false",
        quota_status,
        state.primary_remaining_percent,
        state.secondary_remaining_percent,
        state.has_task ? "true" : "false",
        status,
        task_title,
        task_message,
        (unsigned int)esp_get_free_heap_size(),
        (unsigned int)esp_get_minimum_free_heap_size(),
        (unsigned int)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));

    httpd_resp_set_type(req, "application/json");
    esp_err_t err = httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
    free(json);
    return err;
}

static esp_err_t root_get_handler(httpd_req_t *req)
{
    ornament_state_t state;
    esp_err_t fetch_error = ESP_OK;
    int64_t age_ms = -1;
    ornament_settings_t settings;
    char bridge_url[ORNAMENT_BRIDGE_URL_MAX * 2];
    char wifi_ssid[80];
    char title[ORNAMENT_TEXT_MAX * 2];
    char message[ORNAMENT_TEXT_MAX * 2];
    char status[16];

    ornament_state_init(&state);
    state_snapshot(&state, &fetch_error, &age_ms);
    if (settings_load(&settings) != ESP_OK) {
        memset(&settings, 0, sizeof(settings));
    }
    html_escape(settings_bridge_url_or_default(&settings), bridge_url, sizeof(bridge_url));
    html_escape(state.wifi_ssid, wifi_ssid, sizeof(wifi_ssid));
    html_escape(state.task_title, title, sizeof(title));
    html_escape(state.task_message, message, sizeof(message));
    status_label(state.status, status, sizeof(status));

    char *html = calloc(1, 8192);
    if (html == NULL) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
        return ESP_FAIL;
    }
    size_t used = 0;
    append(
        html,
        8192,
        &used,
        "<!doctype html><html><head><meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
        "<title>Codex Ornament Console</title>"
        "<style>"
        "body{font-family:system-ui,-apple-system,Segoe UI,sans-serif;margin:22px;background:#0b1116;color:#edf7fb}"
        "main{max-width:760px;margin:auto}.grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(210px,1fr));gap:12px}"
        ".card{border:1px solid #263744;border-radius:8px;padding:14px;background:#111a21}.k{color:#8fa3b1;font-size:13px}.v{font-size:22px;margin-top:5px}"
        "button,a.btn{box-sizing:border-box;display:inline-block;margin:8px 8px 0 0;padding:10px 12px;border:0;border-radius:8px;background:#49d3c8;color:#06100f;font-weight:700;text-decoration:none}"
        "button.warn{background:#ffbf45}.danger{background:#ff5b5b}code{word-break:break-all;color:#c6f7ff}"
        "input{box-sizing:border-box;width:100%;padding:10px;border-radius:8px;border:1px solid #344a58;background:#0e171e;color:#fff}"
        "pre{white-space:pre-wrap;word-break:break-word;background:#070b0e;border-radius:8px;padding:10px;color:#b9cbd6}"
        "</style></head><body><main><h1>Codex Ornament Console</h1>");
    appendf(html, 8192, &used, "<p class=\"k\">Bridge URL</p><p><code>%s</code></p>", bridge_url);
    append(html, 8192, &used, "<div class=\"grid\">");
    appendf(html, 8192, &used, "<div class=\"card\"><div class=\"k\">Wi-Fi</div><div class=\"v\">%s %ddBm</div></div>", state.wifi_connected ? wifi_ssid : "OFF", state.wifi_rssi);
    appendf(html, 8192, &used, "<div class=\"card\"><div class=\"k\">Time</div><div class=\"v\">%s %s</div></div>", state.local_time, state.local_date);
    appendf(html, 8192, &used, "<div class=\"card\"><div class=\"k\">Quota</div><div class=\"v\">%d%% / %d%%</div></div>", state.primary_remaining_percent, state.secondary_remaining_percent);
    appendf(html, 8192, &used, "<div class=\"card\"><div class=\"k\">Task</div><div class=\"v\">%s</div></div>", status);
    append(html, 8192, &used, "</div>");
    appendf(html, 8192, &used, "<p><b>%s</b><br>%s</p>", title[0] != '\0' ? title : "No task title", message[0] != '\0' ? message : "");
    appendf(html, 8192, &used, "<p class=\"k\">Last fetch: %s, age: %lld ms</p>", esp_err_to_name(fetch_error), (long long)age_ms);
    append(
        html,
        8192,
        &used,
        "<form method=\"post\" action=\"/test-bridge\"><label class=\"k\">Test Bridge URL</label>"
        "<input name=\"bridge_url\" maxlength=\"159\" value=\"");
    append(html, 8192, &used, bridge_url);
    append(
        html,
        8192,
        &used,
        "\"><button type=\"submit\">Test</button></form>"
        "<p><a class=\"btn\" href=\"/status\">JSON Status</a></p>"
        "<form method=\"post\" action=\"/reboot\"><button class=\"warn\" type=\"submit\">Reboot</button></form>"
        "<form method=\"post\" action=\"/clear-config\"><button class=\"danger\" type=\"submit\">Clear Wi-Fi and Bridge Config</button></form>"
        "<script>setTimeout(()=>location.reload(),10000)</script>"
        "</main></body></html>");

    httpd_resp_set_type(req, "text/html; charset=utf-8");
    esp_err_t err = httpd_resp_send(req, html, HTTPD_RESP_USE_STRLEN);
    free(html);
    return err;
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
        } else if (*read == '%' && hex_value(read[1]) >= 0 && hex_value(read[2]) >= 0) {
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

static esp_err_t test_bridge_post_handler(httpd_req_t *req)
{
    char body[256] = {0};
    char url[ORNAMENT_BRIDGE_URL_MAX] = {0};
    if (read_form_body(req, body, sizeof(body)) != ESP_OK) {
        return ESP_FAIL;
    }
    form_value(body, "bridge_url", url, sizeof(url));

    bridge_probe_result_t result;
    esp_err_t err = bridge_client_probe_url(url, &result);

    char escaped_url[ORNAMENT_BRIDGE_URL_MAX * 2];
    html_escape(url, escaped_url, sizeof(escaped_url));
    char html[1024];
    snprintf(
        html,
        sizeof(html),
        "<!doctype html><html><head><meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
        "<style>body{font-family:system-ui;margin:24px;background:#0b1116;color:#edf7fb}a{color:#49d3c8}</style>"
        "</head><body><h1>Bridge Test</h1><p><code>%s</code></p>"
        "<p>Result: %s</p><p>HTTP: %d, bytes: %d, JSON: %s, status: %s</p><p><a href=\"/\">Back</a></p></body></html>",
        escaped_url,
        esp_err_to_name(err),
        result.http_status,
        result.response_bytes,
        result.json_ok ? "ok" : "invalid",
        result.status_text);

    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, html, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t reboot_post_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_sendstr(req, "rebooting");
    vTaskDelay(pdMS_TO_TICKS(300));
    esp_restart();
    return ESP_OK;
}

static esp_err_t clear_config_post_handler(httpd_req_t *req)
{
    esp_err_t err = settings_clear();
    httpd_resp_set_type(req, "text/plain");
    if (err == ESP_OK) {
        httpd_resp_sendstr(req, "config cleared; rebooting");
        vTaskDelay(pdMS_TO_TICKS(300));
        esp_restart();
        return ESP_OK;
    }
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, esp_err_to_name(err));
    return ESP_FAIL;
}

esp_err_t web_console_start(void)
{
    if (server != NULL) {
        return ESP_OK;
    }
    if (state_mutex == NULL) {
        state_mutex = xSemaphoreCreateMutex();
        if (state_mutex == NULL) {
            return ESP_ERR_NO_MEM;
        }
        ornament_state_init(&last_state);
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;
    config.lru_purge_enable = true;
    config.max_uri_handlers = 8;

    esp_err_t err = httpd_start(&server, &config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "failed to start web console: %s", esp_err_to_name(err));
        return err;
    }

    const httpd_uri_t root = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = root_get_handler,
    };
    const httpd_uri_t status = {
        .uri = "/status",
        .method = HTTP_GET,
        .handler = status_get_handler,
    };
    const httpd_uri_t test_bridge = {
        .uri = "/test-bridge",
        .method = HTTP_POST,
        .handler = test_bridge_post_handler,
    };
    const httpd_uri_t reboot = {
        .uri = "/reboot",
        .method = HTTP_POST,
        .handler = reboot_post_handler,
    };
    const httpd_uri_t clear_config = {
        .uri = "/clear-config",
        .method = HTTP_POST,
        .handler = clear_config_post_handler,
    };

    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &root));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &status));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &test_bridge));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &reboot));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &clear_config));
    ESP_LOGI(TAG, "web console started on http://<device-ip>/");
    return ESP_OK;
}
