#include "web_console.h"

#include "bridge_client.h"
#include "device_identity.h"
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
#include "lwip/inet.h"
#include "lwip/sockets.h"

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
static ornament_settings_t console_settings;

static void refresh_console_settings(void)
{
    if (settings_load(&console_settings) != ESP_OK) {
        memset(&console_settings, 0, sizeof(console_settings));
    }
}

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

static const char *task_title_or_default(bool has_task, const char *title, const char *fallback)
{
    return has_task && title != NULL && title[0] != '\0' ? title : fallback;
}

static bool text_equals_ignore_case(const char *left, const char *right)
{
    if (left == NULL || right == NULL) {
        return false;
    }

    while (*left != '\0' && *right != '\0') {
        char left_ch = *left;
        char right_ch = *right;
        if (left_ch >= 'a' && left_ch <= 'z') {
            left_ch = (char)(left_ch - 'a' + 'A');
        }
        if (right_ch >= 'a' && right_ch <= 'z') {
            right_ch = (char)(right_ch - 'a' + 'A');
        }
        if (left_ch != right_ch) {
            return false;
        }
        left++;
        right++;
    }
    return *left == '\0' && *right == '\0';
}

static void task_card_status_label(const ornament_state_t *state, char *out, size_t out_size)
{
    if (state == NULL || out_size == 0) {
        return;
    }

    if (state->status == ORNAMENT_STATUS_DONE && state->has_task) {
        if (text_equals_ignore_case(state->task_title, "Claude + Codex done")) {
            strlcpy(out, "claude + codex done", out_size);
            return;
        }
        if (text_equals_ignore_case(state->task_title, "Claude done")) {
            strlcpy(out, "claude done", out_size);
            return;
        }
        if (text_equals_ignore_case(state->task_title, "Codex done")) {
            strlcpy(out, "codex done", out_size);
            return;
        }
    }
    status_label(state->status, out, out_size);
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
    wifi_debug_snapshot_t wifi_debug;
    esp_err_t fetch_error = ESP_OK;
    int64_t age_ms = -1;
    char status[16];
    char task_title[ORNAMENT_TEXT_MAX * 2];
    char task_message[ORNAMENT_TEXT_MAX * 2];
    char quota_status[64];
    char wifi_ssid[80];
    char bridge_url[ORNAMENT_BRIDGE_URL_MAX * 2];

    ornament_state_init(&state);
    state_snapshot(&state, &fetch_error, &age_ms);
    wifi_debug_snapshot(&wifi_debug);
    status_label(state.status, status, sizeof(status));
    json_escape(state.task_title, task_title, sizeof(task_title));
    json_escape(state.task_message, task_message, sizeof(task_message));
    json_escape(state.quota_status, quota_status, sizeof(quota_status));
    json_escape(state.wifi_ssid, wifi_ssid, sizeof(wifi_ssid));

    json_escape(settings_bridge_url_or_default(&console_settings), bridge_url, sizeof(bridge_url));

    const size_t json_size = 4096;
    char *json = calloc(1, json_size);
    if (json == NULL) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
        return ESP_FAIL;
    }

    snprintf(
        json,
        json_size,
        "{"
        "\"uptime_ms\":%lld,"
        "\"last_state_age_ms\":%lld,"
        "\"fetch_error\":\"%s\","
        "\"hostname\":\"%s\","
        "\"mdns_url\":\"%s\","
        "\"bridge_url\":\"%s\","
        "\"wifi\":{\"connected\":%s,\"ssid\":\"%s\",\"rssi\":%d},"
        "\"network\":{\"connected\":%s,\"ip\":\"%s\",\"netmask\":\"%s\",\"gateway\":\"%s\",\"bssid\":\"%s\","
        "\"channel\":%d,\"authmode\":%d,\"retry_count\":%d,\"last_disconnect_reason\":%u,"
        "\"last_disconnect_name\":\"%s\",\"last_disconnect_rssi\":%d},"
        "\"time\":{\"synced\":%s,\"local_time\":\"%s\",\"local_date\":\"%s\"},"
        "\"quota\":{\"has\":%s,\"status\":\"%s\",\"primary\":%d,\"weekly\":%d},"
        "\"task\":{\"has\":%s,\"active_count\":%d,\"done_seq\":%d,\"status\":\"%s\",\"title\":\"%s\",\"message\":\"%s\"},"
        "\"heap\":{\"free\":%u,\"min_free\":%u,\"largest_free_block\":%u}"
        "}",
        (long long)(esp_timer_get_time() / 1000),
        (long long)age_ms,
        esp_err_to_name(fetch_error),
        device_identity_hostname(),
        device_identity_mdns_url(),
        bridge_url,
        state.wifi_connected ? "true" : "false",
        wifi_ssid,
        state.wifi_rssi,
        wifi_debug.connected ? "true" : "false",
        wifi_debug.ip,
        wifi_debug.netmask,
        wifi_debug.gateway,
        wifi_debug.bssid,
        wifi_debug.channel,
        wifi_debug.authmode,
        wifi_debug.retry_count,
        wifi_debug.last_disconnect_reason,
        wifi_debug.last_disconnect_name,
        wifi_debug.last_disconnect_rssi,
        state.time_synced ? "true" : "false",
        state.local_time,
        state.local_date,
        state.has_quota ? "true" : "false",
        quota_status,
        state.primary_remaining_percent,
        state.secondary_remaining_percent,
        state.has_task ? "true" : "false",
        state.active_task_count,
        state.done_seq,
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
    wifi_debug_snapshot_t wifi_debug;
    esp_err_t fetch_error = ESP_OK;
    int64_t age_ms = -1;
    char bridge_url[ORNAMENT_BRIDGE_URL_MAX * 2];
    char wifi_ssid[80];
    char title[ORNAMENT_TEXT_MAX * 2];
    char message[ORNAMENT_TEXT_MAX * 2];
    char codex_title[ORNAMENT_TEXT_MAX * 2];
    char codex_message[ORNAMENT_TEXT_MAX * 2];
    char claude_title[ORNAMENT_TEXT_MAX * 2];
    char claude_message[ORNAMENT_TEXT_MAX * 2];
    char status[32];
    char codex_status[32];
    char claude_status[32];

    ornament_state_init(&state);
    state_snapshot(&state, &fetch_error, &age_ms);
    wifi_debug_snapshot(&wifi_debug);
    html_escape(settings_bridge_url_or_default(&console_settings), bridge_url, sizeof(bridge_url));
    html_escape(state.wifi_ssid, wifi_ssid, sizeof(wifi_ssid));
    html_escape(state.task_title, title, sizeof(title));
    html_escape(state.task_message, message, sizeof(message));
    html_escape(state.codex_task_title, codex_title, sizeof(codex_title));
    html_escape(state.codex_task_message, codex_message, sizeof(codex_message));
    html_escape(state.claude_task_title, claude_title, sizeof(claude_title));
    html_escape(state.claude_task_message, claude_message, sizeof(claude_message));
    task_card_status_label(&state, status, sizeof(status));
    status_label(state.codex_task_status, codex_status, sizeof(codex_status));
    status_label(state.claude_task_status, claude_status, sizeof(claude_status));

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
        ".task-grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(210px,1fr));gap:12px;margin-top:12px}"
        ".task-detail{min-height:56px;overflow:hidden;text-overflow:ellipsis}"
        "button,a.btn{box-sizing:border-box;display:inline-block;margin:8px 8px 0 0;padding:10px 12px;border:0;border-radius:8px;background:#49d3c8;color:#06100f;font-weight:700;text-decoration:none}"
        "button.warn{background:#ffbf45}.danger{background:#ff5b5b}code{word-break:break-all;color:#c6f7ff}"
        "input{box-sizing:border-box;width:100%;padding:10px;border-radius:8px;border:1px solid #344a58;background:#0e171e;color:#fff}"
        "pre{white-space:pre-wrap;word-break:break-word;background:#070b0e;border-radius:8px;padding:10px;color:#b9cbd6}"
        "</style></head><body><main><h1>Codex Ornament Console</h1>");
    appendf(html, 8192, &used, "<p class=\"k\">Local URL</p><p><code>%s</code></p>", device_identity_mdns_url());
    appendf(html, 8192, &used, "<p class=\"k\">Bridge URL</p><p><code>%s</code></p>", bridge_url);
    append(html, 8192, &used, "<div class=\"grid\">");
    appendf(html, 8192, &used, "<div class=\"card\"><div class=\"k\">Wi-Fi</div><div class=\"v\">%s %ddBm</div></div>", state.wifi_connected ? wifi_ssid : "OFF", state.wifi_rssi);
    appendf(html, 8192, &used, "<div class=\"card\"><div class=\"k\">Time</div><div class=\"v\">%s %s</div></div>", state.local_time, state.local_date);
    appendf(html, 8192, &used, "<div class=\"card\"><div class=\"k\">Quota</div><div class=\"v\">%d%% / %d%%</div></div>", state.primary_remaining_percent, state.secondary_remaining_percent);
    append(html, 8192, &used, "</div>");
    append(html, 8192, &used, "<div class=\"task-grid\">");
    appendf(
        html,
        8192,
        &used,
        "<div class=\"card\"><div class=\"k\">Task</div><div class=\"v\">%s</div><div class=\"k\">active %d, done %d</div></div>",
        state.has_codex_summary ? codex_status : status,
        state.has_codex_summary ? state.codex_active_task_count : state.active_task_count,
        state.has_codex_summary ? state.codex_done_seq : state.done_seq);
    appendf(
        html,
        8192,
        &used,
        "<div class=\"card claude\"><div class=\"k\">Claude Task</div><div class=\"v\">%s</div><div class=\"k\">active %d, done %d</div></div>",
        state.has_claude_summary ? claude_status : "idle",
        state.claude_active_task_count,
        state.claude_done_seq);
    append(html, 8192, &used, "</div>");
    append(
        html,
        8192,
        &used,
        "<div class=\"task-grid\">");
    appendf(
        html,
        8192,
        &used,
        "<p class=\"task-detail\"><b>%s</b><br>%s</p>",
        task_title_or_default(state.has_codex_task, codex_title, title[0] != '\0' ? title : "No task title"),
        state.has_codex_task ? codex_message : "");
    appendf(
        html,
        8192,
        &used,
        "<p class=\"task-detail claude\"><b>%s</b><br>%s</p>",
        task_title_or_default(state.has_claude_task, claude_title, "No Claude task"),
        state.has_claude_task ? claude_message : "");
    append(html, 8192, &used, "</div>");
    appendf(html, 8192, &used, "<p class=\"k\">Last fetch: %s, age: %lld ms</p>", esp_err_to_name(fetch_error), (long long)age_ms);
    append(
        html,
        8192,
        &used,
        "<h2>Network Debug</h2><div class=\"grid\">");
    appendf(html, 8192, &used, "<div class=\"card\"><div class=\"k\">IP</div><div class=\"v\">%s</div></div>", wifi_debug.ip[0] != '\0' ? wifi_debug.ip : "0.0.0.0");
    appendf(html, 8192, &used, "<div class=\"card\"><div class=\"k\">Gateway</div><div class=\"v\">%s</div></div>", wifi_debug.gateway[0] != '\0' ? wifi_debug.gateway : "0.0.0.0");
    appendf(html, 8192, &used, "<div class=\"card\"><div class=\"k\">Channel</div><div class=\"v\">%d</div></div>", wifi_debug.channel);
    appendf(html, 8192, &used, "<div class=\"card\"><div class=\"k\">BSSID</div><div class=\"v\">%s</div></div>", wifi_debug.bssid[0] != '\0' ? wifi_debug.bssid : "--");
    appendf(html, 8192, &used, "<div class=\"card\"><div class=\"k\">Disconnect</div><div class=\"v\">%u %s</div><div class=\"k\">rssi %d retries %d</div></div>", wifi_debug.last_disconnect_reason, wifi_debug.last_disconnect_name, wifi_debug.last_disconnect_rssi, wifi_debug.retry_count);
    appendf(html, 8192, &used, "<div class=\"card\"><div class=\"k\">Bridge Fetch</div><div class=\"v\">%s</div><div class=\"k\">age %lld ms</div></div>", esp_err_to_name(fetch_error), (long long)age_ms);
    append(html, 8192, &used, "</div>");
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
        "\"><button type=\"submit\">Test</button>"
        "<button class=\"warn\" type=\"submit\" formaction=\"/save-bridge\">Save</button></form>"
        "<form id=\"autoBridge\" method=\"post\" action=\"/auto-bridge\"><input type=\"hidden\" name=\"bridge_url\" id=\"autoBridgeUrl\">"
        "<button type=\"submit\">Auto Match This PC Bridge</button></form>"
        "<p><a class=\"btn\" href=\"/status\">JSON Status</a></p>"
        "<form method=\"post\" action=\"/reboot\"><button class=\"warn\" type=\"submit\">Reboot</button></form>"
        "<form method=\"post\" action=\"/clear-config\"><button class=\"danger\" type=\"submit\">Clear Wi-Fi and Bridge Config</button></form>"
        "<script>"
        "document.getElementById('autoBridge').addEventListener('submit',async e=>{"
        "const input=document.getElementById('autoBridgeUrl');"
        "if(input.value)return;"
        "e.preventDefault();"
        "try{const r=await fetch('http://127.0.0.1:8787/discover',{cache:'no-store'});"
        "if(!r.ok)throw new Error('HTTP '+r.status);"
        "const data=await r.json();"
        "input.value=data.stateUrl||'';"
        "if(!input.value)throw new Error('missing stateUrl');"
        "e.target.submit();}"
        "catch(err){alert('Bridge auto-match failed: '+err.message);}"
        "});"
        "setTimeout(()=>location.reload(),10000)"
        "</script>"
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

static esp_err_t save_bridge_url(const char *url)
{
    if (url == NULL || url[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    ornament_settings_t settings;
    esp_err_t err = settings_load(&settings);
    if (err != ESP_OK) {
        return err;
    }

    strlcpy(settings.bridge_url, url, sizeof(settings.bridge_url));
    settings.has_bridge_url = true;
    err = settings_save(&settings);
    if (err == ESP_OK) {
        console_settings = settings;
    }
    return err;
}

static esp_err_t send_bridge_saved_page(httpd_req_t *req, const char *url, const char *detail)
{
    char escaped_url[ORNAMENT_BRIDGE_URL_MAX * 2];
    char escaped_detail[160];
    html_escape(url, escaped_url, sizeof(escaped_url));
    html_escape(detail != NULL ? detail : "The next poll will use this URL.", escaped_detail, sizeof(escaped_detail));

    char html[1024];
    snprintf(
        html,
        sizeof(html),
        "<!doctype html><html><head><meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
        "<style>body{font-family:system-ui;margin:24px;background:#0b1116;color:#edf7fb}a{color:#49d3c8}code{word-break:break-all}</style>"
        "</head><body><h1>Bridge Saved</h1><p><code>%s</code></p><p>%s</p><p><a href=\"/\">Back</a></p></body></html>",
        escaped_url,
        escaped_detail);

    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, html, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t client_ip_from_request(httpd_req_t *req, char *ip, size_t ip_size)
{
    if (req == NULL || ip == NULL || ip_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    int sockfd = httpd_req_to_sockfd(req);
    if (sockfd < 0) {
        return ESP_FAIL;
    }

    struct sockaddr_storage addr = {0};
    socklen_t addr_len = sizeof(addr);
    if (getpeername(sockfd, (struct sockaddr *)&addr, &addr_len) != 0) {
        return ESP_FAIL;
    }

    if (addr.ss_family == AF_INET) {
        const struct sockaddr_in *peer = (const struct sockaddr_in *)&addr;
        inet_ntoa_r(peer->sin_addr, ip, ip_size);
        return ip[0] != '\0' ? ESP_OK : ESP_FAIL;
    }

    return ESP_ERR_NOT_SUPPORTED;
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

static esp_err_t save_bridge_post_handler(httpd_req_t *req)
{
    char body[256] = {0};
    char url[ORNAMENT_BRIDGE_URL_MAX] = {0};
    if (read_form_body(req, body, sizeof(body)) != ESP_OK) {
        return ESP_FAIL;
    }
    form_value(body, "bridge_url", url, sizeof(url));
    if (url[0] == '\0') {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bridge URL is required");
        return ESP_FAIL;
    }

    esp_err_t err = save_bridge_url(url);
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, esp_err_to_name(err));
        return ESP_FAIL;
    }

    return send_bridge_saved_page(req, url, "The next poll will use this URL.");
}

static esp_err_t auto_bridge_post_handler(httpd_req_t *req)
{
    char url[ORNAMENT_BRIDGE_URL_MAX];
    char body[256] = {0};
    if (req->content_len > 0) {
        if (read_form_body(req, body, sizeof(body)) != ESP_OK) {
            return ESP_FAIL;
        }
        form_value(body, "bridge_url", url, sizeof(url));
    } else {
        url[0] = '\0';
    }

    char client_ip[16] = {0};
    if (url[0] == '\0') {
        esp_err_t ip_err = client_ip_from_request(req, client_ip, sizeof(client_ip));
        if (ip_err != ESP_OK) {
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Could not auto-detect bridge URL");
            return ESP_FAIL;
        }
        snprintf(url, sizeof(url), "http://%s:8787/state", client_ip);
    }

    bridge_probe_result_t result;
    esp_err_t err = bridge_client_probe_url(url, &result);
    if (err != ESP_OK) {
        char message[256];
        snprintf(
            message,
            sizeof(message),
            "Bridge test failed for %.159s: %.32s. Start the bridge on this PC first.",
            url,
            esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, message);
        return ESP_FAIL;
    }

    err = save_bridge_url(url);
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, esp_err_to_name(err));
        return ESP_FAIL;
    }

    return send_bridge_saved_page(req, url, "Auto-detected from this browser and verified.");
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
        memset(&console_settings, 0, sizeof(console_settings));
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
    refresh_console_settings();

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;
    config.lru_purge_enable = true;
    config.max_uri_handlers = 10;
    config.stack_size = 16384;

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
    const httpd_uri_t save_bridge = {
        .uri = "/save-bridge",
        .method = HTTP_POST,
        .handler = save_bridge_post_handler,
    };
    const httpd_uri_t auto_bridge = {
        .uri = "/auto-bridge",
        .method = HTTP_POST,
        .handler = auto_bridge_post_handler,
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
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &save_bridge));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &auto_bridge));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &reboot));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &clear_config));
    ESP_LOGI(TAG, "web console started on http://<device-ip>/");
    return ESP_OK;
}
