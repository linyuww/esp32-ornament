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
#include "task_audio.h"
#include "weather_client.h"
#include "wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define COORD_E6_SCALE 1000000

static const char *TAG = "web_console";
static httpd_handle_t server;
static SemaphoreHandle_t state_mutex;
static ornament_state_t last_state;
static esp_err_t last_fetch_error = ESP_ERR_INVALID_STATE;
static int64_t last_state_us;
static ornament_settings_t console_settings;
static web_console_bridge_debug_t bridge_debug;

static void refresh_console_settings(void)
{
    if (settings_load(&console_settings) != ESP_OK) {
        memset(&console_settings, 0, sizeof(console_settings));
        console_settings.audio_volume_percent = CONFIG_ORNAMENT_AUDIO_VOLUME_PERCENT;
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

static void task_panel_status_label(ornament_status_t status, int active_count, char *out, size_t out_size)
{
    if (out_size == 0) {
        return;
    }
    if (active_count > 0 || status == ORNAMENT_STATUS_RUNNING) {
        strlcpy(out, "running", out_size);
    } else {
        strlcpy(out, "done", out_size);
    }
}

static const char *display_id_or_dash(const char *value)
{
    return value != NULL && value[0] != '\0' ? value : "--";
}

static void coord_e6_to_text(int value, char *out, size_t out_size)
{
    if (out_size == 0) {
        return;
    }
    int whole = value / COORD_E6_SCALE;
    int fraction = value % COORD_E6_SCALE;
    if (fraction < 0) {
        fraction = -fraction;
    }
    if (value < 0 && whole == 0) {
        snprintf(out, out_size, "-0.%06d", fraction);
    } else {
        snprintf(out, out_size, "%d.%06d", whole, fraction);
    }
}

static void append_task_card(
    char *html,
    size_t html_size,
    size_t *used,
    const char *label,
    const char *status,
    int active_count,
    int done_seq)
{
    appendf(
        html,
        html_size,
        used,
        "<div class=\"card task-card\"><div class=\"card-head\"><div><div class=\"k\">%s</div>"
        "<div class=\"v\">%s</div></div><div class=\"counts\">active <b>%d</b><br>done <b>%d</b></div></div></div>",
        label,
        status,
        active_count,
        done_seq);
}

static void append_task_detail(
    char *html,
    size_t html_size,
    size_t *used,
    const char *label,
    const char *status,
    const char *session_id,
    const char *turn_id)
{
    appendf(
        html,
        html_size,
        used,
        "<div class=\"task-detail\"><div class=\"k\">%s</div><b>%s</b>"
        "<div class=\"detail-lines\"><div class=\"detail-row\"><span>session_id</span><code>%s</code></div>"
        "<div class=\"detail-row\"><span>turn_id</span><code>%s</code></div></div></div>",
        label,
        status,
        display_id_or_dash(session_id),
        display_id_or_dash(turn_id));
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

void web_console_set_bridge_debug(const web_console_bridge_debug_t *debug)
{
    if (debug == NULL) {
        return;
    }
    if (state_mutex != NULL && xSemaphoreTake(state_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        bridge_debug = *debug;
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
    char weather_status[48];
    char weather_label[48];
    char weather_source[40];
    char weather_summary[80];
    char weather_icon[40];
    char weather_observed_at[ORNAMENT_TIME_MAX * 2];
    char weather_config_label[ORNAMENT_WEATHER_LABEL_MAX * 2];
    char bridge_url[ORNAMENT_BRIDGE_URL_MAX * 2];
    web_console_bridge_debug_t bridge_diag = {0};

    ornament_state_init(&state);
    state_snapshot(&state, &fetch_error, &age_ms);
    wifi_debug_snapshot(&wifi_debug);
    status_label(state.status, status, sizeof(status));
    json_escape(state.task_title, task_title, sizeof(task_title));
    json_escape(state.task_message, task_message, sizeof(task_message));
    json_escape(state.quota_status, quota_status, sizeof(quota_status));
    json_escape(state.wifi_ssid, wifi_ssid, sizeof(wifi_ssid));
    json_escape(state.weather_status, weather_status, sizeof(weather_status));
    json_escape(state.weather_label, weather_label, sizeof(weather_label));
    json_escape(state.weather_source, weather_source, sizeof(weather_source));
    json_escape(state.weather_summary, weather_summary, sizeof(weather_summary));
    json_escape(state.weather_icon, weather_icon, sizeof(weather_icon));
    json_escape(state.weather_observed_at, weather_observed_at, sizeof(weather_observed_at));
    json_escape(settings_weather_label_or_default(&console_settings), weather_config_label, sizeof(weather_config_label));

    json_escape(settings_bridge_url_or_default(&console_settings), bridge_url, sizeof(bridge_url));
    if (state_mutex != NULL && xSemaphoreTake(state_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        bridge_diag = bridge_debug;
        xSemaphoreGive(state_mutex);
    }

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
        "\"bridge_offline\":%s,"
        "\"hostname\":\"%s\","
        "\"mdns_url\":\"%s\","
        "\"bridge_url\":\"%s\","
        "\"wifi\":{\"connected\":%s,\"ssid\":\"%s\",\"rssi\":%d},"
        "\"network\":{\"connected\":%s,\"ip\":\"%s\",\"netmask\":\"%s\",\"gateway\":\"%s\",\"bssid\":\"%s\","
        "\"channel\":%d,\"authmode\":%d,\"retry_count\":%d,\"last_disconnect_reason\":%u,"
        "\"last_disconnect_name\":\"%s\",\"last_disconnect_rssi\":%d},"
        "\"time\":{\"synced\":%s,\"local_time\":\"%s\",\"local_date\":\"%s\"},"
        "\"weather\":{\"has\":%s,\"status\":\"%s\",\"label\":\"%s\",\"source\":\"%s\",\"summary\":\"%s\",\"icon\":\"%s\","
        "\"observed_at\":\"%s\",\"temperature_c\":%d,\"wind_kmh\":%d,\"code\":%d},"
        "\"weather_config\":{\"caiyun_configured\":%s,\"source\":\"%s\",\"label\":\"%s\",\"lat_e6\":%d,\"lon_e6\":%d},"
        "\"quota\":{\"has\":%s,\"status\":\"%s\",\"primary\":%d,\"weekly\":%d},"
        "\"task\":{\"has\":%s,\"active_count\":%d,\"done_seq\":%d,\"status\":\"%s\",\"title\":\"%s\",\"message\":\"%s\"},"
        "\"audio\":{\"enabled\":%s,\"volume_percent\":%d},"
        "\"bridge_debug\":{\"last_fetch_error\":\"%s\",\"consecutive_fetch_failures\":%d,"
        "\"last_success_ms\":%lld,\"last_failure_ms\":%lld,"
        "\"last_auto_match_ok\":%s,\"last_auto_match_error\":\"%s\",\"last_auto_match_reason\":\"%s\"},"
        "\"heap\":{\"free\":%u,\"min_free\":%u,\"largest_free_block\":%u}"
        "}",
        (long long)(esp_timer_get_time() / 1000),
        (long long)age_ms,
        esp_err_to_name(fetch_error),
        state.bridge_offline ? "true" : "false",
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
        state.has_weather ? "true" : "false",
        weather_status,
        weather_label,
        weather_source,
        weather_summary,
        weather_icon,
        weather_observed_at,
        state.weather_temperature_c,
        state.weather_wind_kmh,
        state.weather_code,
        console_settings.has_caiyun_token ? "true" : "false",
        settings_weather_source_or_default(&console_settings),
        weather_config_label,
        console_settings.weather_lat_e6,
        console_settings.weather_lon_e6,
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
        CONFIG_ORNAMENT_AUDIO_ENABLED ? "true" : "false",
        settings_audio_volume_percent_or_default(&console_settings),
        esp_err_to_name(bridge_diag.last_fetch_error),
        bridge_diag.consecutive_fetch_failures,
        (long long)bridge_diag.last_success_ms,
        (long long)bridge_diag.last_failure_ms,
        bridge_diag.last_auto_match_ok ? "true" : "false",
        esp_err_to_name(bridge_diag.last_auto_match_error),
        bridge_diag.last_auto_match_reason,
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
    char codex_session_id[ORNAMENT_TIME_MAX * 2];
    char codex_turn_id[ORNAMENT_TIME_MAX * 2];
    char claude_session_id[ORNAMENT_TIME_MAX * 2];
    char claude_turn_id[ORNAMENT_TIME_MAX * 2];
    char codex_status[32];
    char claude_status[32];
    char weather_label[48];
    char weather_source[40];
    char weather_summary[80];
    char weather_icon[40];
    char weather_value[96];
    char weather_detail[192];
    char settings_weather_label[ORNAMENT_WEATHER_LABEL_MAX * 2];
    const char *settings_weather_source = settings_weather_source_or_default(&console_settings);
    char weather_lat_text[24];
    char weather_lon_text[24];
    web_console_bridge_debug_t bridge_diag = {0};
    int audio_volume_percent = settings_audio_volume_percent_or_default(&console_settings);

    ornament_state_init(&state);
    state_snapshot(&state, &fetch_error, &age_ms);
    wifi_debug_snapshot(&wifi_debug);
    int codex_active_count = state.has_codex_summary ? state.codex_active_task_count : state.active_task_count;
    int codex_done_seq = state.has_codex_summary ? state.codex_done_seq : state.done_seq;
    html_escape(settings_bridge_url_or_default(&console_settings), bridge_url, sizeof(bridge_url));
    html_escape(state.wifi_ssid, wifi_ssid, sizeof(wifi_ssid));
    html_escape(state.weather_label, weather_label, sizeof(weather_label));
    html_escape(state.weather_source, weather_source, sizeof(weather_source));
    html_escape(state.weather_summary, weather_summary, sizeof(weather_summary));
    html_escape(state.weather_icon, weather_icon, sizeof(weather_icon));
    html_escape(settings_weather_label_or_default(&console_settings), settings_weather_label, sizeof(settings_weather_label));
    coord_e6_to_text(console_settings.weather_lat_e6, weather_lat_text, sizeof(weather_lat_text));
    coord_e6_to_text(console_settings.weather_lon_e6, weather_lon_text, sizeof(weather_lon_text));
    html_escape(
        state.has_codex_task ? state.codex_task_session_id : state.task_session_id,
        codex_session_id,
        sizeof(codex_session_id));
    html_escape(
        state.has_codex_task ? state.codex_task_turn_id : state.task_turn_id,
        codex_turn_id,
        sizeof(codex_turn_id));
    html_escape(state.claude_task_session_id, claude_session_id, sizeof(claude_session_id));
    html_escape(state.claude_task_turn_id, claude_turn_id, sizeof(claude_turn_id));
    task_panel_status_label(state.has_codex_summary ? state.codex_task_status : state.status, codex_active_count, codex_status, sizeof(codex_status));
    task_panel_status_label(state.claude_task_status, state.claude_active_task_count, claude_status, sizeof(claude_status));
    if (state_mutex != NULL && xSemaphoreTake(state_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        bridge_diag = bridge_debug;
        xSemaphoreGive(state_mutex);
    }
    if (state.has_weather && state.weather_temperature_c != INT32_MIN) {
        snprintf(weather_value, sizeof(weather_value), "%dC %s", state.weather_temperature_c, weather_summary);
    } else {
        snprintf(weather_value, sizeof(weather_value), "%s", state.has_weather ? weather_summary : "--");
    }
    if (state.has_weather && state.weather_wind_kmh >= 0) {
        snprintf(
            weather_detail,
            sizeof(weather_detail),
            "%s %s %s wind %d",
            weather_label[0] != '\0' ? weather_label : "WEATHER",
            weather_source[0] != '\0' ? weather_source : "--",
            weather_icon[0] != '\0' ? weather_icon : "unknown",
            state.weather_wind_kmh);
    } else {
        snprintf(weather_detail, sizeof(weather_detail), "%s", weather_label[0] != '\0' ? weather_label : "--");
    }

    const size_t html_size = 14336;
    char *html = calloc(1, html_size);
    if (html == NULL) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
        return ESP_FAIL;
    }
    size_t used = 0;
    append(
        html,
        html_size,
        &used,
        "<!doctype html><html><head><meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
        "<title>Codex Ornament Console</title>"
        "<style>"
        ":root{color-scheme:dark;--bg:#101214;--panel:#171b20;--panel2:#1d2329;--line:#303841;--text:#eef2f6;--muted:#98a4ae;--accent:#45d3c8;--warn:#f2b84b;--danger:#ff6b6b}"
        "*{box-sizing:border-box}body{font-family:system-ui,-apple-system,Segoe UI,sans-serif;margin:0;background:var(--bg);color:var(--text)}"
        "main{max-width:960px;margin:0 auto;padding:24px}h1{margin:0;font-size:28px;line-height:1.15;letter-spacing:0}h2{margin:22px 0 10px;font-size:16px;letter-spacing:0;color:#dbe3ea}"
        ".top{display:flex;justify-content:space-between;gap:16px;align-items:flex-start;margin-bottom:18px}.sub{margin:6px 0 0;color:var(--muted);font-size:13px}"
        ".badges{display:flex;flex-wrap:wrap;gap:8px;justify-content:flex-end}.badge{border:1px solid var(--line);border-radius:8px;padding:7px 9px;background:var(--panel);color:#d9e2e8;font-size:13px}"
        ".urls{display:grid;grid-template-columns:repeat(auto-fit,minmax(260px,1fr));gap:10px;margin-bottom:12px}.urlbox{border:1px solid var(--line);border-radius:8px;background:#13171b;padding:10px 12px}"
        ".grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(210px,1fr));gap:12px}.task-grid{display:grid;grid-template-columns:repeat(2,minmax(0,1fr));gap:12px;margin-top:12px}"
        ".card{border:1px solid var(--line);border-radius:8px;padding:14px;background:var(--panel);box-shadow:0 8px 22px rgba(0,0,0,.18)}"
        ".card-head{display:flex;justify-content:space-between;gap:12px;align-items:flex-start}.k{color:var(--muted);font-size:13px}.v{font-size:22px;line-height:1.2;margin-top:5px;color:var(--text)}"
        ".counts{text-align:right;color:var(--muted);font-size:13px;line-height:1.55}.counts b{color:var(--text);font-size:15px}.task-card{min-height:78px}.task-detail{min-height:118px;border:1px solid var(--line);border-radius:8px;background:#13171b;padding:14px;overflow:hidden}"
        ".task-detail b{display:block;margin:6px 0 12px;color:#f4f7fa;font-size:20px}.detail-lines{display:grid;gap:7px}.detail-row{display:grid;grid-template-columns:76px minmax(0,1fr);gap:9px;align-items:baseline}.detail-row span{color:var(--muted);font-size:13px}.ops{border:1px solid var(--line);border-radius:8px;background:var(--panel);padding:14px;margin-top:16px}"
        ".actions{display:flex;flex-wrap:wrap;gap:8px;margin-top:10px}button,a.btn{display:inline-block;padding:10px 12px;border:0;border-radius:8px;background:var(--accent);color:#06100f;font-weight:700;text-decoration:none;cursor:pointer}"
        "button.warn{background:var(--warn)}.danger{background:var(--danger);color:#180506}code{word-break:break-all;color:#a8f4ec}label{display:block;margin-bottom:7px}"
        "input,select{width:100%;padding:11px;border-radius:8px;border:1px solid #3a444d;background:#0f1317;color:#fff}input[type=range]{padding:0;accent-color:var(--accent)}form{margin:0}footer{margin:14px 0 0;color:var(--muted);font-size:13px}"
        "@media(max-width:620px){main{padding:18px}.top{display:block}.badges{justify-content:flex-start;margin-top:12px}.task-grid{grid-template-columns:1fr}.counts{text-align:left}.card-head{display:block}.detail-row{grid-template-columns:1fr;gap:2px}}"
        "</style></head><body><main>");
    appendf(
        html,
        html_size,
        &used,
        "<header class=\"top\"><div><h1>Codex Ornament</h1><p class=\"sub\">%s</p></div>"
        "<div class=\"badges\"><span class=\"badge\">Bridge %s</span><span class=\"badge\">%s</span><span class=\"badge\">age %lld ms</span></div></header>",
        device_identity_hostname(),
        esp_err_to_name(fetch_error),
        state.bridge_offline ? "Bridge offline" : "Bridge online",
        (long long)age_ms);
    append(html, html_size, &used, "<section class=\"urls\">");
    appendf(html, html_size, &used, "<div class=\"urlbox\"><div class=\"k\">Local URL</div><code>%s</code></div>", device_identity_mdns_url());
    appendf(html, html_size, &used, "<div class=\"urlbox\"><div class=\"k\">Bridge URL</div><code>%s</code></div>", bridge_url);
    append(html, html_size, &used, "</section>");
    append(html, html_size, &used, "<section class=\"grid\">");
    appendf(html, html_size, &used, "<div class=\"card\"><div class=\"k\">Wi-Fi</div><div class=\"v\">%s</div><div class=\"k\">%ddBm</div></div>", state.wifi_connected ? wifi_ssid : "OFF", state.wifi_rssi);
    appendf(html, html_size, &used, "<div class=\"card\"><div class=\"k\">Time</div><div class=\"v\">%s</div><div class=\"k\">%s</div></div>", state.local_time, state.local_date);
    appendf(html, html_size, &used, "<div class=\"card\"><div class=\"k\">Weather</div><div class=\"v\">%s</div><div class=\"k\">%s</div></div>", weather_value, weather_detail);
    appendf(html, html_size, &used, "<div class=\"card\"><div class=\"k\">Quota</div><div class=\"v\">%d%% / %d%%</div><div class=\"k\">primary / weekly</div></div>", state.primary_remaining_percent, state.secondary_remaining_percent);
    append(html, html_size, &used, "</section>");
    append(html, html_size, &used, "<h2>Tasks</h2><section class=\"task-grid\">");
    append_task_card(html, html_size, &used, "Codex Task", codex_status, codex_active_count, codex_done_seq);
    append_task_card(html, html_size, &used, "Claude Task", claude_status, state.claude_active_task_count, state.claude_done_seq);
    append(html, html_size, &used, "</section>");
    append(
        html,
        html_size,
        &used,
        "<section class=\"task-grid\">");
    append_task_detail(html, html_size, &used, "Codex detail", codex_status, codex_session_id, codex_turn_id);
    append_task_detail(html, html_size, &used, "Claude detail", claude_status, claude_session_id, claude_turn_id);
    append(html, html_size, &used, "</section>");
    append(
        html,
        html_size,
        &used,
        "<h2>Network</h2><section class=\"grid\">");
    appendf(html, html_size, &used, "<div class=\"card\"><div class=\"k\">IP</div><div class=\"v\">%s</div></div>", wifi_debug.ip[0] != '\0' ? wifi_debug.ip : "0.0.0.0");
    appendf(html, html_size, &used, "<div class=\"card\"><div class=\"k\">Gateway</div><div class=\"v\">%s</div></div>", wifi_debug.gateway[0] != '\0' ? wifi_debug.gateway : "0.0.0.0");
    appendf(html, html_size, &used, "<div class=\"card\"><div class=\"k\">Channel</div><div class=\"v\">%d</div></div>", wifi_debug.channel);
    appendf(html, html_size, &used, "<div class=\"card\"><div class=\"k\">BSSID</div><div class=\"v\">%s</div></div>", wifi_debug.bssid[0] != '\0' ? wifi_debug.bssid : "--");
    appendf(html, html_size, &used, "<div class=\"card\"><div class=\"k\">Disconnect</div><div class=\"v\">%u %s</div><div class=\"k\">rssi %d retries %d</div></div>", wifi_debug.last_disconnect_reason, wifi_debug.last_disconnect_name, wifi_debug.last_disconnect_rssi, wifi_debug.retry_count);
    appendf(html, html_size, &used, "<div class=\"card\"><div class=\"k\">Bridge Fetch</div><div class=\"v\">%s</div><div class=\"k\">age %lld ms</div></div>", esp_err_to_name(fetch_error), (long long)age_ms);
    append(html, html_size, &used, "</section>");
    append(html, html_size, &used, "<h2>Bridge Debug</h2><section class=\"grid\">");
    appendf(html, html_size, &used, "<div class=\"card\"><div class=\"k\">Last Fetch</div><div class=\"v\">%s</div><div class=\"k\">failures %d</div></div>", esp_err_to_name(bridge_diag.last_fetch_error), bridge_diag.consecutive_fetch_failures);
    appendf(html, html_size, &used, "<div class=\"card\"><div class=\"k\">Last Success</div><div class=\"v\">%lld ms</div><div class=\"k\">since boot</div></div>", (long long)bridge_diag.last_success_ms);
    appendf(html, html_size, &used, "<div class=\"card\"><div class=\"k\">Last Failure</div><div class=\"v\">%lld ms</div><div class=\"k\">since boot</div></div>", (long long)bridge_diag.last_failure_ms);
    appendf(html, html_size, &used, "<div class=\"card\"><div class=\"k\">Auto Match</div><div class=\"v\">%s</div><div class=\"k\">%s / %s</div></div>", bridge_diag.last_auto_match_ok ? "ok" : "failed", esp_err_to_name(bridge_diag.last_auto_match_error), bridge_diag.last_auto_match_reason[0] != '\0' ? bridge_diag.last_auto_match_reason : "--");
    append(html, html_size, &used, "</section>");
    appendf(
        html,
        html_size,
        &used,
        "<section class=\"ops\"><form method=\"post\" action=\"/save-audio\"><label class=\"k\">Voice Volume</label>"
        "<input type=\"range\" name=\"audio_volume\" min=\"0\" max=\"100\" step=\"5\" value=\"%d\" oninput=\"audioVol.value=this.value\">"
        "<div class=\"v\"><output id=\"audioVol\">%d</output>%%</div>"
        "<div class=\"actions\"><button type=\"submit\">Save Volume</button>"
        "<button class=\"warn\" type=\"submit\" formaction=\"/test-audio\">Test Voice</button></div></form></section>",
        audio_volume_percent,
        audio_volume_percent);
    appendf(
        html,
        html_size,
        &used,
        "<section class=\"ops\"><form method=\"post\" action=\"/save-weather\">"
        "<label class=\"k\">Weather Source</label><select name=\"weather_source\">"
        "<option value=\"caiyun\"%s>Caiyun</option><option value=\"open-meteo\"%s>Open-Meteo</option></select>"
        "<label class=\"k\">Weather Label</label><input name=\"weather_label\" maxlength=\"24\" value=\"%s\">"
        "<label class=\"k\">Latitude</label><input name=\"weather_lat\" inputmode=\"decimal\" value=\"%s\">"
        "<label class=\"k\">Longitude</label><input name=\"weather_lon\" inputmode=\"decimal\" value=\"%s\">"
        "<label class=\"k\">Caiyun Token (%s)</label><input name=\"caiyun_token\" type=\"password\" maxlength=\"96\" value=\"\" placeholder=\"%s\">"
        "<div class=\"actions\"><button type=\"submit\">Save Weather</button>"
        "<button class=\"danger\" type=\"submit\" formaction=\"/clear-weather-token\">Clear Token</button></div></form></section>",
        strcmp(settings_weather_source, ORNAMENT_WEATHER_SOURCE_CAIYUN) == 0 ? " selected" : "",
        strcmp(settings_weather_source, ORNAMENT_WEATHER_SOURCE_OPEN_METEO) == 0 ? " selected" : "",
        settings_weather_label,
        weather_lat_text,
        weather_lon_text,
        console_settings.has_caiyun_token ? "configured" : "not configured",
        console_settings.has_caiyun_token ? "configured" : "paste token");
    append(
        html,
        html_size,
        &used,
        "<section class=\"ops\"><form method=\"post\" action=\"/test-bridge\"><label class=\"k\">Test Bridge URL</label>"
        "<input name=\"bridge_url\" maxlength=\"159\" value=\"");
    append(html, html_size, &used, bridge_url);
    append(
        html,
        html_size,
        &used,
        "\"><div class=\"actions\"><button type=\"submit\">Test</button>"
        "<button class=\"warn\" type=\"submit\" formaction=\"/save-bridge\">Save</button></div></form>"
        "<form method=\"post\" action=\"/auto-bridge\">"
        "<div class=\"actions\"><button type=\"submit\">Auto Match This PC Bridge</button><a class=\"btn\" href=\"/status\">JSON Status</a></div></form>"
        "<div class=\"actions\"><form method=\"post\" action=\"/reboot\"><button class=\"warn\" type=\"submit\">Reboot</button></form>"
        "<form method=\"post\" action=\"/clear-config\"><button class=\"danger\" type=\"submit\">Clear Wi-Fi and Bridge Config</button></form></div></section>"
        "<footer>Refreshes every 10 seconds</footer>"
        "<script>"
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

static esp_err_t save_audio_volume(int volume_percent)
{
    if (volume_percent < 0 || volume_percent > 100) {
        return ESP_ERR_INVALID_ARG;
    }

    ornament_settings_t settings;
    esp_err_t err = settings_load(&settings);
    if (err != ESP_OK) {
        return err;
    }

    settings.audio_volume_percent = volume_percent;
    err = settings_save(&settings);
    if (err == ESP_OK) {
        console_settings = settings;
    }
    return err;
}

static esp_err_t parse_audio_volume_percent(const char *value, int *volume_percent)
{
    if (value == NULL || volume_percent == NULL || value[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    int parsed = 0;
    for (const char *cursor = value; *cursor != '\0'; cursor++) {
        if (*cursor < '0' || *cursor > '9') {
            return ESP_ERR_INVALID_ARG;
        }
        parsed = parsed * 10 + (*cursor - '0');
        if (parsed > 100) {
            return ESP_ERR_INVALID_ARG;
        }
    }

    *volume_percent = parsed;
    return ESP_OK;
}

static esp_err_t parse_coord_e6(const char *value, int min_e6, int max_e6, int *out)
{
    if (value == NULL || out == NULL || value[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    const char *cursor = value;
    bool negative = false;
    if (*cursor == '-' || *cursor == '+') {
        negative = *cursor == '-';
        cursor++;
    }
    if (*cursor == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    int64_t whole = 0;
    int fraction = 0;
    int fraction_digits = 0;
    bool saw_digit = false;
    while (*cursor >= '0' && *cursor <= '9') {
        saw_digit = true;
        whole = whole * 10 + (*cursor - '0');
        if (whole > 180) {
            return ESP_ERR_INVALID_ARG;
        }
        cursor++;
    }
    if (*cursor == '.') {
        cursor++;
        while (*cursor >= '0' && *cursor <= '9') {
            if (fraction_digits < 6) {
                fraction = fraction * 10 + (*cursor - '0');
                fraction_digits++;
            }
            cursor++;
        }
    }
    if (*cursor != '\0' || !saw_digit) {
        return ESP_ERR_INVALID_ARG;
    }
    while (fraction_digits < 6) {
        fraction *= 10;
        fraction_digits++;
    }

    int64_t scaled = whole * COORD_E6_SCALE + fraction;
    if (negative) {
        scaled = -scaled;
    }
    if (scaled < min_e6 || scaled > max_e6) {
        return ESP_ERR_INVALID_ARG;
    }
    *out = (int)scaled;
    return ESP_OK;
}

static esp_err_t save_weather_settings(
    const char *label,
    const char *source,
    const char *lat_text,
    const char *lon_text,
    const char *token)
{
    if (label == NULL || label[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    if (source == NULL ||
        (strcmp(source, ORNAMENT_WEATHER_SOURCE_CAIYUN) != 0 &&
         strcmp(source, ORNAMENT_WEATHER_SOURCE_OPEN_METEO) != 0)) {
        return ESP_ERR_INVALID_ARG;
    }

    int lat_e6 = 0;
    int lon_e6 = 0;
    esp_err_t err = parse_coord_e6(lat_text, -90 * COORD_E6_SCALE, 90 * COORD_E6_SCALE, &lat_e6);
    if (err != ESP_OK) {
        return err;
    }
    err = parse_coord_e6(lon_text, -180 * COORD_E6_SCALE, 180 * COORD_E6_SCALE, &lon_e6);
    if (err != ESP_OK) {
        return err;
    }

    ornament_settings_t settings;
    err = settings_load(&settings);
    if (err != ESP_OK) {
        return err;
    }

    strlcpy(settings.weather_label, label, sizeof(settings.weather_label));
    strlcpy(settings.weather_source, source, sizeof(settings.weather_source));
    settings.weather_lat_e6 = lat_e6;
    settings.weather_lon_e6 = lon_e6;
    if (token != NULL && token[0] != '\0') {
        strlcpy(settings.caiyun_token, token, sizeof(settings.caiyun_token));
        settings.has_caiyun_token = true;
    }

    err = settings_save(&settings);
    if (err == ESP_OK) {
        console_settings = settings;
        weather_client_settings_changed();
    }
    return err;
}

static esp_err_t clear_weather_token(void)
{
    ornament_settings_t settings;
    esp_err_t err = settings_load(&settings);
    if (err != ESP_OK) {
        return err;
    }

    settings.caiyun_token[0] = '\0';
    settings.has_caiyun_token = false;
    err = settings_save(&settings);
    if (err == ESP_OK) {
        console_settings = settings;
        weather_client_settings_changed();
    }
    return err;
}

static esp_err_t send_audio_saved_page(httpd_req_t *req, int volume_percent, bool played)
{
    char html[768];
    snprintf(
        html,
        sizeof(html),
        "<!doctype html><html><head><meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
        "<style>body{font-family:system-ui;margin:24px;background:#0b1116;color:#edf7fb}a{color:#49d3c8}</style>"
        "</head><body><h1>Voice Volume</h1><p>Volume: %d%%</p><p>%s</p><p><a href=\"/\">Back</a></p></body></html>",
        volume_percent,
        played ? "Test voice queued." : "Saved for the next voice reminder.");

    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, html, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t send_weather_saved_page(httpd_req_t *req, const char *title, const char *detail)
{
    char escaped_detail[160];
    html_escape(detail != NULL ? detail : "", escaped_detail, sizeof(escaped_detail));

    char html[768];
    snprintf(
        html,
        sizeof(html),
        "<!doctype html><html><head><meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
        "<style>body{font-family:system-ui;margin:24px;background:#0b1116;color:#edf7fb}a{color:#49d3c8}</style>"
        "</head><body><h1>%s</h1><p>%s</p><p><a href=\"/\">Back</a></p></body></html>",
        title != NULL ? title : "Weather",
        escaped_detail);

    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, html, HTTPD_RESP_USE_STRLEN);
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

static esp_err_t save_audio_post_handler(httpd_req_t *req)
{
    char body[128] = {0};
    char value[8] = {0};
    if (read_form_body(req, body, sizeof(body)) != ESP_OK) {
        return ESP_FAIL;
    }
    form_value(body, "audio_volume", value, sizeof(value));
    int volume_percent = 0;
    if (parse_audio_volume_percent(value, &volume_percent) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Audio volume must be 0..100");
        return ESP_FAIL;
    }

    esp_err_t err = save_audio_volume(volume_percent);
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, esp_err_to_name(err));
        return ESP_FAIL;
    }

    return send_audio_saved_page(req, volume_percent, false);
}

static esp_err_t save_weather_post_handler(httpd_req_t *req)
{
    char body[512] = {0};
    char label[ORNAMENT_WEATHER_LABEL_MAX + 1] = {0};
    char source[ORNAMENT_WEATHER_SOURCE_MAX + 1] = {0};
    char lat_text[24] = {0};
    char lon_text[24] = {0};
    char token[ORNAMENT_WEATHER_TOKEN_MAX + 1] = {0};
    if (read_form_body(req, body, sizeof(body)) != ESP_OK) {
        return ESP_FAIL;
    }

    form_value(body, "weather_label", label, sizeof(label));
    form_value(body, "weather_source", source, sizeof(source));
    form_value(body, "weather_lat", lat_text, sizeof(lat_text));
    form_value(body, "weather_lon", lon_text, sizeof(lon_text));
    form_value(body, "caiyun_token", token, sizeof(token));

    esp_err_t err = save_weather_settings(label, source, lat_text, lon_text, token);
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Weather source, label, latitude, longitude, or token is invalid");
        return ESP_FAIL;
    }

    return send_weather_saved_page(req, "Weather Saved", "Weather settings were saved. A blank token field keeps the existing token.");
}

static esp_err_t clear_weather_token_post_handler(httpd_req_t *req)
{
    if (req->content_len > 0) {
        char body[512] = {0};
        (void)read_form_body(req, body, sizeof(body));
    }

    esp_err_t err = clear_weather_token();
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, esp_err_to_name(err));
        return ESP_FAIL;
    }

    return send_weather_saved_page(req, "Weather Token Cleared", "Local Caiyun weather is disabled until a token is saved again.");
}

static esp_err_t test_audio_post_handler(httpd_req_t *req)
{
    char body[128] = {0};
    char value[8] = {0};
    int volume_percent = settings_audio_volume_percent_or_default(&console_settings);
    if (req->content_len > 0 && read_form_body(req, body, sizeof(body)) == ESP_OK) {
        form_value(body, "audio_volume", value, sizeof(value));
        if (value[0] != '\0' && parse_audio_volume_percent(value, &volume_percent) != ESP_OK) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Audio volume must be 0..100");
            return ESP_FAIL;
        }
    }

    esp_err_t err = save_audio_volume(volume_percent);
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, esp_err_to_name(err));
        return ESP_FAIL;
    }

    task_audio_play_done();
    return send_audio_saved_page(req, volume_percent, true);
}

static esp_err_t auto_bridge_post_handler(httpd_req_t *req)
{
    if (req->content_len > 0) {
        char body[16] = {0};
        (void)read_form_body(req, body, sizeof(body));
    }

    bridge_auto_match_result_t result;
    esp_err_t err = bridge_client_auto_match(true, &result);
    if (err != ESP_OK) {
        char message[256];
        snprintf(
            message,
            sizeof(message),
            "Bridge auto-match failed after %d probes: %.32s. Start the bridge on this PC first.",
            result.tested_count,
            esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, message);
        return ESP_FAIL;
    }

    refresh_console_settings();
    char detail[160];
    snprintf(
        detail,
        sizeof(detail),
        "Auto-matched by the ESP over %s after %d probe(s).",
        result.source,
        result.tested_count);
    return send_bridge_saved_page(req, result.bridge_url, detail);
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
        memset(&bridge_debug, 0, sizeof(bridge_debug));
        bridge_debug.last_fetch_error = ESP_ERR_INVALID_STATE;
        bridge_debug.last_auto_match_error = ESP_ERR_INVALID_STATE;
    }
    refresh_console_settings();

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;
    config.lru_purge_enable = true;
    config.max_uri_handlers = 14;
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
    const httpd_uri_t save_audio = {
        .uri = "/save-audio",
        .method = HTTP_POST,
        .handler = save_audio_post_handler,
    };
    const httpd_uri_t test_audio = {
        .uri = "/test-audio",
        .method = HTTP_POST,
        .handler = test_audio_post_handler,
    };
    const httpd_uri_t save_weather = {
        .uri = "/save-weather",
        .method = HTTP_POST,
        .handler = save_weather_post_handler,
    };
    const httpd_uri_t clear_weather_token = {
        .uri = "/clear-weather-token",
        .method = HTTP_POST,
        .handler = clear_weather_token_post_handler,
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
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &save_audio));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &test_audio));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &save_weather));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &clear_weather_token));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &auto_bridge));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &reboot));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &clear_config));
    ESP_LOGI(TAG, "web console started on http://<device-ip>/");
    return ESP_OK;
}
