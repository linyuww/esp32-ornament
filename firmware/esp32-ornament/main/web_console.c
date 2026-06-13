#include "web_console.h"

#ifndef CONFIG_ORNAMENT_WEB_CONSOLE_ENABLED
#define CONFIG_ORNAMENT_WEB_CONSOLE_ENABLED 0
#endif

#if CONFIG_ORNAMENT_WEB_CONSOLE_ENABLED

#include "bridge_client.h"
#include "device_identity.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "music_player.h"
#include "settings.h"
#include "system_diagnostics.h"
#include "task_audio.h"
#include "weather_client.h"
#include "wifi.h"
#include "xiaozhi_client.h"
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

static void log_handler_stack_headroom(const char *handler_name)
{
    UBaseType_t words = uxTaskGetStackHighWaterMark(NULL);
    ESP_LOGI(TAG, "%s stack headroom=%u bytes", handler_name, (unsigned int)(words * sizeof(StackType_t)));
}

static void refresh_console_settings(void)
{
    if (settings_load(&console_settings) != ESP_OK) {
        memset(&console_settings, 0, sizeof(console_settings));
        console_settings.audio_volume_percent = CONFIG_ORNAMENT_AUDIO_VOLUME_PERCENT;
    }
}

static void *alloc_console_buffer(size_t size)
{
    void *buffer = heap_caps_calloc(1, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (buffer != NULL) {
        return buffer;
    }

    ESP_LOGW(
        TAG,
        "PSRAM response allocation failed: size=%u largest_spiram=%u largest_internal=%u",
        (unsigned int)size,
        (unsigned int)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT),
        (unsigned int)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    return heap_caps_calloc(1, size, MALLOC_CAP_8BIT);
}

static char *alloc_response_buffer(size_t size)
{
    return (char *)alloc_console_buffer(size);
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

static void format_i64_decimal(int64_t value, char *output, size_t output_size)
{
    if (output == NULL || output_size == 0) {
        return;
    }

    char scratch[24];
    size_t cursor = sizeof(scratch);
    scratch[--cursor] = '\0';

    bool negative = value < 0;
    uint64_t magnitude = negative ? (uint64_t)(-(value + 1)) + 1U : (uint64_t)value;
    do {
        scratch[--cursor] = (char)('0' + (magnitude % 10U));
        magnitude /= 10U;
    } while (magnitude > 0 && cursor > 0);

    if (negative && cursor > 0) {
        scratch[--cursor] = '-';
    }

    strlcpy(output, &scratch[cursor], output_size);
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

static void append_diagnostics_json(
    char *json,
    size_t json_size,
    size_t *used,
    const system_diagnostics_snapshot_t *diag)
{
    char uptime_ms[24];
    format_i64_decimal(diag->uptime_ms, uptime_ms, sizeof(uptime_ms));

    appendf(
        json,
        json_size,
        used,
        "\"system\":{\"uptime_ms\":%s,\"reset_reason\":\"%s\",\"reset_reason_code\":%d,"
        "\"heap\":{\"free\":%u,\"min_free\":%u,\"largest_free_block\":%u,"
        "\"internal_free\":%u,\"internal_min_free\":%u,\"largest_internal_block\":%u,"
        "\"spiram_free\":%u,\"spiram_min_free\":%u,\"largest_spiram_block\":%u},",
        uptime_ms,
        system_diagnostics_reset_reason_name(diag->reset_reason),
        (int)diag->reset_reason,
        (unsigned int)diag->free_heap,
        (unsigned int)diag->minimum_free_heap,
        (unsigned int)diag->largest_8bit_block,
        (unsigned int)diag->internal_free,
        (unsigned int)diag->internal_minimum_free,
        (unsigned int)diag->largest_internal_block,
        (unsigned int)diag->spiram_free,
        (unsigned int)diag->spiram_minimum_free,
        (unsigned int)diag->largest_spiram_block);

    append(json, json_size, used, "\"tasks\":[");
    for (size_t i = 0; i < diag->task_count; i++) {
        const system_diagnostics_task_t *task = &diag->tasks[i];
        if (!task->valid) {
            continue;
        }
        appendf(
            json,
            json_size,
            used,
            "%s{\"name\":\"%s\",\"stack_bytes\":%u,\"stack_high_water_bytes\":%u,"
            "\"priority\":%u,\"external_stack\":%s}",
            i > 0 ? "," : "",
            task->name,
            (unsigned int)task->configured_stack_bytes,
            (unsigned int)task->stack_high_water_bytes,
            (unsigned int)task->priority,
            task->external_stack ? "true" : "false");
    }
    append(json, json_size, used, "],\"heap_checkpoints\":[");
    for (size_t i = 0; i < diag->heap_checkpoint_count; i++) {
        const system_diagnostics_heap_checkpoint_t *checkpoint = &diag->heap_checkpoints[i];
        char checkpoint_uptime_ms[24];
        format_i64_decimal(
            checkpoint->uptime_ms,
            checkpoint_uptime_ms,
            sizeof(checkpoint_uptime_ms));
        appendf(
            json,
            json_size,
            used,
            "%s{\"stage\":\"%s\",\"uptime_ms\":%s,\"free\":%u,\"min_free\":%u,"
            "\"largest_free_block\":%u,\"internal_free\":%u,\"spiram_free\":%u}",
            i > 0 ? "," : "",
            checkpoint->stage,
            checkpoint_uptime_ms,
            (unsigned int)checkpoint->free_heap,
            (unsigned int)checkpoint->minimum_free_heap,
            (unsigned int)checkpoint->largest_8bit_block,
            (unsigned int)checkpoint->internal_free,
            (unsigned int)checkpoint->spiram_free);
    }
    append(json, json_size, used, "]},");
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

static void snapshot_bridge_debug(web_console_bridge_debug_t *snapshot)
{
    if (snapshot == NULL) {
        return;
    }
    memset(snapshot, 0, sizeof(*snapshot));
    if (state_mutex != NULL && xSemaphoreTake(state_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        *snapshot = bridge_debug;
        xSemaphoreGive(state_mutex);
    }
}

static esp_err_t status_get_handler(httpd_req_t *req)
{
    log_handler_stack_headroom("/status");

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
    char standby_wallpaper_id[ORNAMENT_WALLPAPER_ID_MAX * 2];
    char standby_wallpaper_name[ORNAMENT_WALLPAPER_NAME_MAX * 2];
    char standby_wallpaper_mode[32];
    char standby_wallpaper_url[ORNAMENT_WALLPAPER_URL_MAX * 2];
    char weather_config_label[ORNAMENT_WEATHER_LABEL_MAX * 2];
    char bridge_url[ORNAMENT_BRIDGE_URL_MAX * 2];
    char xiaozhi_ws_url[ORNAMENT_XIAOZHI_WS_URL_MAX * 2];
    char xiaozhi_saved_ws_url[ORNAMENT_XIAOZHI_WS_URL_MAX * 2];
    char xiaozhi_runtime_ws_url[ORNAMENT_XIAOZHI_WS_URL_MAX * 2];
    char xiaozhi_active_ws_url[ORNAMENT_XIAOZHI_WS_URL_MAX * 2];
    char xiaozhi_client_id[XIAOZHI_CLIENT_ID_MAX * 2];
    char xiaozhi_session_id[XIAOZHI_SESSION_ID_MAX * 2];
    char xiaozhi_activation_code[XIAOZHI_ACTIVATION_CODE_MAX * 2];
    char xiaozhi_activation_message[XIAOZHI_STATUS_TEXT_MAX * 2];
    char xiaozhi_last_error[XIAOZHI_STATUS_TEXT_MAX * 2];
    char xiaozhi_last_stt[XIAOZHI_STATUS_TEXT_MAX * 2];
    char xiaozhi_last_tts[XIAOZHI_STATUS_TEXT_MAX * 2];
    char music_title[ORNAMENT_TEXT_MAX * 2];
    char music_artist[ORNAMENT_TEXT_MAX * 2];
    char music_album[ORNAMENT_TEXT_MAX * 2];
    char music_picture[ORNAMENT_BRIDGE_URL_MAX * 2];
    char music_cover_url[ORNAMENT_BRIDGE_URL_MAX * 2];
    xiaozhi_client_snapshot_t *xiaozhi = alloc_console_buffer(sizeof(*xiaozhi));
    music_player_snapshot_t *music = alloc_console_buffer(sizeof(*music));
    web_console_bridge_debug_t bridge_diag = {0};
    system_diagnostics_snapshot_t *diag = alloc_console_buffer(sizeof(*diag));

    if (xiaozhi == NULL || music == NULL || diag == NULL) {
        free(xiaozhi);
        free(music);
        free(diag);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
        return ESP_FAIL;
    }

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
    json_escape(state.standby_wallpaper_id, standby_wallpaper_id, sizeof(standby_wallpaper_id));
    json_escape(state.standby_wallpaper_name, standby_wallpaper_name, sizeof(standby_wallpaper_name));
    json_escape(state.standby_wallpaper_mode, standby_wallpaper_mode, sizeof(standby_wallpaper_mode));
    json_escape(state.standby_wallpaper_url, standby_wallpaper_url, sizeof(standby_wallpaper_url));
    json_escape(settings_weather_label_or_default(&console_settings), weather_config_label, sizeof(weather_config_label));

    json_escape(settings_bridge_url_or_default(&console_settings), bridge_url, sizeof(bridge_url));
    xiaozhi_client_status_snapshot(xiaozhi);
    system_diagnostics_snapshot(diag);
    json_escape(xiaozhi->ws_url, xiaozhi_ws_url, sizeof(xiaozhi_ws_url));
    json_escape(xiaozhi->saved_ws_url, xiaozhi_saved_ws_url, sizeof(xiaozhi_saved_ws_url));
    json_escape(xiaozhi->runtime_ws_url, xiaozhi_runtime_ws_url, sizeof(xiaozhi_runtime_ws_url));
    json_escape(xiaozhi->active_ws_url, xiaozhi_active_ws_url, sizeof(xiaozhi_active_ws_url));
    json_escape(xiaozhi->client_id, xiaozhi_client_id, sizeof(xiaozhi_client_id));
    json_escape(xiaozhi->session_id, xiaozhi_session_id, sizeof(xiaozhi_session_id));
    json_escape(xiaozhi->activation_code, xiaozhi_activation_code, sizeof(xiaozhi_activation_code));
    json_escape(xiaozhi->activation_message, xiaozhi_activation_message, sizeof(xiaozhi_activation_message));
    json_escape(xiaozhi->last_error, xiaozhi_last_error, sizeof(xiaozhi_last_error));
    json_escape(xiaozhi->last_stt, xiaozhi_last_stt, sizeof(xiaozhi_last_stt));
    json_escape(xiaozhi->last_tts, xiaozhi_last_tts, sizeof(xiaozhi_last_tts));
    music_player_status_snapshot(music);
    json_escape(music->title, music_title, sizeof(music_title));
    json_escape(music->artist_name, music_artist, sizeof(music_artist));
    json_escape(music->album, music_album, sizeof(music_album));
    json_escape(music->picture, music_picture, sizeof(music_picture));
    json_escape(music->cover_url, music_cover_url, sizeof(music_cover_url));
    snapshot_bridge_debug(&bridge_diag);

    const size_t json_size = 12288;
    char *json = alloc_response_buffer(json_size);
    if (json == NULL) {
        free(xiaozhi);
        free(music);
        free(diag);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
        return ESP_FAIL;
    }
    size_t used = 0;
    char uptime_ms[24];
    char age_ms_text[24];
    char last_success_ms[24];
    char last_failure_ms[24];
    format_i64_decimal(esp_timer_get_time() / 1000, uptime_ms, sizeof(uptime_ms));
    format_i64_decimal(age_ms, age_ms_text, sizeof(age_ms_text));
    format_i64_decimal(bridge_diag.last_success_ms, last_success_ms, sizeof(last_success_ms));
    format_i64_decimal(bridge_diag.last_failure_ms, last_failure_ms, sizeof(last_failure_ms));
    append(json, json_size, &used, "{");
    append_diagnostics_json(json, json_size, &used, diag);
    appendf(json, json_size, &used, "\"uptime_ms\":%s,", uptime_ms);
    appendf(json, json_size, &used, "\"last_state_age_ms\":%s,", age_ms_text);
    appendf(json, json_size, &used, "\"fetch_error\":\"%s\",", esp_err_to_name(fetch_error));
    appendf(json, json_size, &used, "\"bridge_offline\":%s,", state.bridge_offline ? "true" : "false");
    appendf(json, json_size, &used, "\"hostname\":\"%s\",", device_identity_hostname());
    appendf(json, json_size, &used, "\"mdns_url\":\"%s\",", device_identity_mdns_url());
    appendf(json, json_size, &used, "\"bridge_url\":\"%s\",", bridge_url);
    appendf(json, json_size, &used, "\"wifi\":{\"connected\":%s,\"ssid\":\"%s\",\"rssi\":%d},", state.wifi_connected ? "true" : "false", wifi_ssid, state.wifi_rssi);
    appendf(
        json,
        json_size,
        &used,
        "\"network\":{\"connected\":%s,\"ip\":\"%s\",\"netmask\":\"%s\",\"gateway\":\"%s\",\"bssid\":\"%s\",\"channel\":%d,\"authmode\":%d,\"retry_count\":%d,\"last_disconnect_reason\":%u,\"last_disconnect_name\":\"%s\",\"last_disconnect_rssi\":%d},",
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
        wifi_debug.last_disconnect_rssi);
    appendf(json, json_size, &used, "\"time\":{\"synced\":%s,\"local_time\":\"%s\",\"local_date\":\"%s\"},", state.time_synced ? "true" : "false", state.local_time, state.local_date);
    appendf(
        json,
        json_size,
        &used,
        "\"weather\":{\"has\":%s,\"status\":\"%s\",\"label\":\"%s\",\"source\":\"%s\",\"summary\":\"%s\",\"icon\":\"%s\",\"observed_at\":\"%s\",\"temperature_c\":%d,\"wind_kmh\":%d,\"code\":%d},",
        state.has_weather ? "true" : "false",
        weather_status,
        weather_label,
        weather_source,
        weather_summary,
        weather_icon,
        weather_observed_at,
        state.weather_temperature_c,
        state.weather_wind_kmh,
        state.weather_code);
    appendf(
        json,
        json_size,
        &used,
        "\"weather_config\":{\"caiyun_configured\":%s,\"source\":\"%s\",\"label\":\"%s\",\"lat_e6\":%d,\"lon_e6\":%d},",
        console_settings.has_caiyun_token ? "true" : "false",
        settings_weather_source_or_default(&console_settings),
        weather_config_label,
        console_settings.weather_lat_e6,
        console_settings.weather_lon_e6);
    appendf(
        json,
        json_size,
        &used,
        "\"standby_wallpaper\":{\"has\":%s,\"id\":\"%s\",\"name\":\"%s\",\"mode\":\"%s\",\"url\":\"%s\",\"index\":%d,\"total\":%d},",
        state.has_standby_wallpaper ? "true" : "false",
        standby_wallpaper_id,
        standby_wallpaper_name,
        standby_wallpaper_mode,
        standby_wallpaper_url,
        state.standby_wallpaper_index,
        state.standby_wallpaper_total);
    appendf(
        json,
        json_size,
        &used,
        "\"quota\":{\"has\":%s,\"status\":\"%s\",\"primary\":%d,\"weekly\":%d},",
        state.has_quota ? "true" : "false",
        quota_status,
        state.primary_remaining_percent,
        state.secondary_remaining_percent);
    appendf(
        json,
        json_size,
        &used,
        "\"task\":{\"has\":%s,\"active_count\":%d,\"done_seq\":%d,\"status\":\"%s\",\"title\":\"%s\",\"message\":\"%s\"},",
        state.has_task ? "true" : "false",
        state.active_task_count,
        state.done_seq,
        status,
        task_title,
        task_message);
    appendf(json, json_size, &used, "\"audio\":{\"enabled\":%s,\"volume_percent\":%d},", CONFIG_ORNAMENT_AUDIO_ENABLED ? "true" : "false", settings_audio_volume_percent_or_default(&console_settings));
    appendf(
        json,
        json_size,
        &used,
        "\"music\":{\"active\":%s,\"stop_requested\":%s,\"state\":\"%s\",\"title\":\"%s\",\"artist\":\"%s\",\"album\":\"%s\",\"picture\":\"%s\",\"cover_url\":\"%s\",\"has_cover\":%s,\"has_lyrics\":%s,\"lyrics_bytes\":%u,\"playback_ms\":%u,\"duration_ms\":%u,\"volume_percent\":%d,\"battery_percent\":%d},",
        music->active ? "true" : "false",
        music->stop_requested ? "true" : "false",
        music_player_state_name(music->state),
        music_title,
        music_artist,
        music_album,
        music_picture,
        music_cover_url,
        music->has_cover ? "true" : "false",
        music->lyrics[0] != '\0' ? "true" : "false",
        (unsigned int)strlen(music->lyrics),
        (unsigned int)music->playback_ms,
        (unsigned int)music->duration_ms,
        music->volume_percent,
        music->battery_percent);
    appendf(
        json,
        json_size,
        &used,
        "\"xiaozhi\":{\"enabled\":%s,\"configured\":%s,\"connected\":%s,\"ai_enabled\":%s,\"session_requested\":%s,\"state\":\"%s\",\"protocol_version\":%d,\"activation_pending\":%s,\"official_runtime_config\":%s,\"ws_url\":\"%s\",\"saved_ws_url\":\"%s\",\"runtime_ws_url\":\"%s\",\"active_ws_url\":\"%s\",\"client_id\":\"%s\",\"session_id\":\"%s\",\"activation_code\":\"%s\",\"activation_message\":\"%s\",\"last_error\":\"%s\",\"last_stt\":\"%s\",\"last_tts\":\"%s\",\"uplink_frames\":%u,\"downlink_frames\":%u},",
        xiaozhi->enabled ? "true" : "false",
        xiaozhi->configured ? "true" : "false",
        xiaozhi->connected ? "true" : "false",
        xiaozhi->session_requested ? "true" : "false",
        xiaozhi->session_requested ? "true" : "false",
        xiaozhi_client_state_name(xiaozhi->state),
        xiaozhi->protocol_version,
        xiaozhi->activation_pending ? "true" : "false",
        xiaozhi->official_runtime_config ? "true" : "false",
        xiaozhi_ws_url,
        xiaozhi_saved_ws_url,
        xiaozhi_runtime_ws_url,
        xiaozhi_active_ws_url,
        xiaozhi_client_id,
        xiaozhi_session_id,
        xiaozhi_activation_code,
        xiaozhi_activation_message,
        xiaozhi_last_error,
        xiaozhi_last_stt,
        xiaozhi_last_tts,
        (unsigned int)xiaozhi->uplink_frames,
        (unsigned int)xiaozhi->downlink_frames);
    appendf(
        json,
        json_size,
        &used,
        "\"bridge_debug\":{\"last_fetch_error\":\"%s\",\"consecutive_fetch_failures\":%d,\"last_success_ms\":%s,\"last_failure_ms\":%s,\"last_auto_match_ok\":%s,\"last_auto_match_error\":\"%s\",\"last_auto_match_reason\":\"%s\"},",
        esp_err_to_name(bridge_diag.last_fetch_error),
        bridge_diag.consecutive_fetch_failures,
        last_success_ms,
        last_failure_ms,
        bridge_diag.last_auto_match_ok ? "true" : "false",
        esp_err_to_name(bridge_diag.last_auto_match_error),
        bridge_diag.last_auto_match_reason);
    appendf(
        json,
        json_size,
        &used,
        "\"heap\":{\"free\":%u,\"min_free\":%u,\"largest_free_block\":%u,\"largest_internal_block\":%u}}",
        (unsigned int)diag->free_heap,
        (unsigned int)diag->minimum_free_heap,
        (unsigned int)diag->largest_8bit_block,
        (unsigned int)diag->largest_internal_block);

    httpd_resp_set_type(req, "application/json");
    esp_err_t err = httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
    free(json);
    free(xiaozhi);
    free(music);
    free(diag);
    return err;
}

static esp_err_t root_get_handler(httpd_req_t *req)
{
    log_handler_stack_headroom("/");

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
    char xiaozhi_ws_url[ORNAMENT_XIAOZHI_WS_URL_MAX * 2];
    char xiaozhi_saved_ws_url[ORNAMENT_XIAOZHI_WS_URL_MAX * 2];
    char xiaozhi_runtime_ws_url[ORNAMENT_XIAOZHI_WS_URL_MAX * 2];
    char xiaozhi_active_ws_url[ORNAMENT_XIAOZHI_WS_URL_MAX * 2];
    char xiaozhi_client_id[XIAOZHI_CLIENT_ID_MAX * 2];
    char xiaozhi_activation_code[XIAOZHI_ACTIVATION_CODE_MAX * 2];
    char xiaozhi_activation_message[XIAOZHI_STATUS_TEXT_MAX * 2];
    char xiaozhi_last_error[XIAOZHI_STATUS_TEXT_MAX * 2];
    char xiaozhi_last_stt[XIAOZHI_STATUS_TEXT_MAX * 2];
    char xiaozhi_last_tts[XIAOZHI_STATUS_TEXT_MAX * 2];
    xiaozhi_client_snapshot_t *xiaozhi = alloc_console_buffer(sizeof(*xiaozhi));
    char music_title[ORNAMENT_TEXT_MAX * 2];
    char music_artist[ORNAMENT_TEXT_MAX * 2];
    char music_album[ORNAMENT_TEXT_MAX * 2];
    char music_error[ORNAMENT_TEXT_MAX * 2];
    music_player_snapshot_t *music = alloc_console_buffer(sizeof(*music));
    char settings_weather_label[ORNAMENT_WEATHER_LABEL_MAX * 2];
    const char *settings_weather_source = settings_weather_source_or_default(&console_settings);
    char weather_lat_text[24];
    char weather_lon_text[24];
    web_console_bridge_debug_t bridge_diag = {0};
    system_diagnostics_snapshot_t *diag = alloc_console_buffer(sizeof(*diag));
    int audio_volume_percent = settings_audio_volume_percent_or_default(&console_settings);

    if (xiaozhi == NULL || music == NULL || diag == NULL) {
        free(xiaozhi);
        free(music);
        free(diag);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
        return ESP_FAIL;
    }

    ornament_state_init(&state);
    state_snapshot(&state, &fetch_error, &age_ms);
    wifi_debug_snapshot(&wifi_debug);
    int codex_active_count = state.has_codex_summary ? state.codex_active_task_count : state.active_task_count;
    int codex_done_seq = state.has_codex_summary ? state.codex_done_seq : state.done_seq;
    html_escape(settings_bridge_url_or_default(&console_settings), bridge_url, sizeof(bridge_url));
    xiaozhi_client_status_snapshot(xiaozhi);
    system_diagnostics_snapshot(diag);
    html_escape(settings_xiaozhi_ws_url_or_default(&console_settings), xiaozhi_ws_url, sizeof(xiaozhi_ws_url));
    html_escape(xiaozhi->saved_ws_url, xiaozhi_saved_ws_url, sizeof(xiaozhi_saved_ws_url));
    html_escape(xiaozhi->runtime_ws_url, xiaozhi_runtime_ws_url, sizeof(xiaozhi_runtime_ws_url));
    html_escape(xiaozhi->active_ws_url, xiaozhi_active_ws_url, sizeof(xiaozhi_active_ws_url));
    html_escape(xiaozhi->client_id, xiaozhi_client_id, sizeof(xiaozhi_client_id));
    html_escape(xiaozhi->activation_code, xiaozhi_activation_code, sizeof(xiaozhi_activation_code));
    html_escape(xiaozhi->activation_message, xiaozhi_activation_message, sizeof(xiaozhi_activation_message));
    html_escape(xiaozhi->last_error, xiaozhi_last_error, sizeof(xiaozhi_last_error));
    html_escape(xiaozhi->last_stt, xiaozhi_last_stt, sizeof(xiaozhi_last_stt));
    html_escape(xiaozhi->last_tts, xiaozhi_last_tts, sizeof(xiaozhi_last_tts));
    music_player_status_snapshot(music);
    html_escape(music->title[0] != '\0' ? music->title : music->song_name, music_title, sizeof(music_title));
    html_escape(music->artist_name, music_artist, sizeof(music_artist));
    html_escape(music->album, music_album, sizeof(music_album));
    html_escape(music->last_error, music_error, sizeof(music_error));
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
    snapshot_bridge_debug(&bridge_diag);
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

    const size_t html_size = 20480;
    char *html = alloc_response_buffer(html_size);
    if (html == NULL) {
        free(xiaozhi);
        free(music);
        free(diag);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
        return ESP_FAIL;
    }
    size_t used = 0;
    char age_ms_text[24];
    char last_success_ms[24];
    char last_failure_ms[24];
    format_i64_decimal(age_ms, age_ms_text, sizeof(age_ms_text));
    format_i64_decimal(bridge_diag.last_success_ms, last_success_ms, sizeof(last_success_ms));
    format_i64_decimal(bridge_diag.last_failure_ms, last_failure_ms, sizeof(last_failure_ms));
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
        "<div class=\"badges\"><span class=\"badge\">Bridge %s</span><span class=\"badge\">%s</span><span class=\"badge\">age %s ms</span></div></header>",
        device_identity_hostname(),
        esp_err_to_name(fetch_error),
        state.bridge_offline ? "Bridge offline" : "Bridge online",
        age_ms_text);
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
    appendf(html, html_size, &used, "<div class=\"card\"><div class=\"k\">Bridge Fetch</div><div class=\"v\">%s</div><div class=\"k\">age %s ms</div></div>", esp_err_to_name(fetch_error), age_ms_text);
    append(html, html_size, &used, "</section>");
    append(html, html_size, &used, "<h2>Bridge Debug</h2><section class=\"grid\">");
    appendf(html, html_size, &used, "<div class=\"card\"><div class=\"k\">Last Fetch</div><div class=\"v\">%s</div><div class=\"k\">failures %d</div></div>", esp_err_to_name(bridge_diag.last_fetch_error), bridge_diag.consecutive_fetch_failures);
    appendf(html, html_size, &used, "<div class=\"card\"><div class=\"k\">Last Success</div><div class=\"v\">%s ms</div><div class=\"k\">since boot</div></div>", last_success_ms);
    appendf(html, html_size, &used, "<div class=\"card\"><div class=\"k\">Last Failure</div><div class=\"v\">%s ms</div><div class=\"k\">since boot</div></div>", last_failure_ms);
    appendf(html, html_size, &used, "<div class=\"card\"><div class=\"k\">Auto Match</div><div class=\"v\">%s</div><div class=\"k\">%s / %s</div></div>", bridge_diag.last_auto_match_ok ? "ok" : "failed", esp_err_to_name(bridge_diag.last_auto_match_error), bridge_diag.last_auto_match_reason[0] != '\0' ? bridge_diag.last_auto_match_reason : "--");
    append(html, html_size, &used, "</section>");
    append(html, html_size, &used, "<h2>System</h2><section class=\"grid\">");
    appendf(html, html_size, &used, "<div class=\"card\"><div class=\"k\">Reset</div><div class=\"v\">%s</div><div class=\"k\">code %d</div></div>", system_diagnostics_reset_reason_name(diag->reset_reason), (int)diag->reset_reason);
    appendf(html, html_size, &used, "<div class=\"card\"><div class=\"k\">Heap Free</div><div class=\"v\">%u KB</div><div class=\"k\">min %u KB</div></div>", (unsigned int)(diag->free_heap / 1024), (unsigned int)(diag->minimum_free_heap / 1024));
    appendf(html, html_size, &used, "<div class=\"card\"><div class=\"k\">Internal RAM</div><div class=\"v\">%u KB</div><div class=\"k\">largest block %u KB</div></div>", (unsigned int)(diag->internal_free / 1024), (unsigned int)(diag->largest_internal_block / 1024));
    appendf(html, html_size, &used, "<div class=\"card\"><div class=\"k\">PSRAM</div><div class=\"v\">%u KB</div><div class=\"k\">largest block %u KB</div></div>", (unsigned int)(diag->spiram_free / 1024), (unsigned int)(diag->largest_spiram_block / 1024));
    append(html, html_size, &used, "</section><section class=\"grid\">");
    for (size_t i = 0; i < diag->task_count; i++) {
        const system_diagnostics_task_t *task = &diag->tasks[i];
        if (!task->valid) {
            continue;
        }
        appendf(
            html,
            html_size,
            &used,
            "<div class=\"card\"><div class=\"k\">%s</div><div class=\"v\">%u B</div>"
            "<div class=\"k\">stack low water / %u B %s</div></div>",
            task->name,
            (unsigned int)task->stack_high_water_bytes,
            (unsigned int)task->configured_stack_bytes,
            task->external_stack ? "psram" : "internal");
    }
    append(html, html_size, &used, "</section>");
    append(html, html_size, &used, "<h2>Xiaozhi AI</h2><section class=\"grid\">");
    appendf(
        html,
        html_size,
        &used,
        "<div class=\"card\"><div class=\"k\">State</div><div class=\"v\">%s</div><div class=\"k\">configured %s</div></div>",
        xiaozhi_client_state_name(xiaozhi->state),
        xiaozhi->configured ? "yes" : "no");
    appendf(
        html,
        html_size,
        &used,
        "<div class=\"card\"><div class=\"k\">AI Enabled</div><div class=\"v\">%s</div><div class=\"k\">stop ai disables wake</div></div>",
        xiaozhi->session_requested ? "yes" : "no");
    appendf(
        html,
        html_size,
        &used,
        "<div class=\"card\"><div class=\"k\">Client ID</div><div class=\"v\">%s</div><div class=\"k\">proto v%d</div></div>",
        xiaozhi_client_id[0] != '\0' ? xiaozhi_client_id : "--",
        xiaozhi->protocol_version);
    appendf(
        html,
        html_size,
        &used,
        "<div class=\"card\"><div class=\"k\">Activation Code</div><div class=\"v\">%s</div><div class=\"k\">%s</div></div>",
        xiaozhi_activation_code[0] != '\0' ? xiaozhi_activation_code : "--",
        xiaozhi->activation_pending ?
            (xiaozhi_activation_message[0] != '\0' ? xiaozhi_activation_message : "pending bind on xiaozhi.me") :
            "not pending");
    appendf(
        html,
        html_size,
        &used,
        "<div class=\"card\"><div class=\"k\">Frames</div><div class=\"v\">%u / %u</div><div class=\"k\">uplink / downlink</div></div>",
        (unsigned int)xiaozhi->uplink_frames,
        (unsigned int)xiaozhi->downlink_frames);
    appendf(
        html,
        html_size,
        &used,
        "<div class=\"card\"><div class=\"k\">Last STT</div><div class=\"v\">%s</div></div>",
        xiaozhi_last_stt[0] != '\0' ? xiaozhi_last_stt : "--");
    appendf(
        html,
        html_size,
        &used,
        "<div class=\"card\"><div class=\"k\">Last TTS</div><div class=\"v\">%s</div><div class=\"k\">%s</div></div>",
        xiaozhi_last_tts[0] != '\0' ? xiaozhi_last_tts : "--",
        xiaozhi_last_error[0] != '\0' ? xiaozhi_last_error : "no error");
    appendf(
        html,
        html_size,
        &used,
        "<div class=\"card\"><div class=\"k\">Saved WS</div><div class=\"v\" style=\"font-size:12px;word-break:break-all\">%s</div></div>",
        xiaozhi_saved_ws_url[0] != '\0' ? xiaozhi_saved_ws_url : "--");
    appendf(
        html,
        html_size,
        &used,
        "<div class=\"card\"><div class=\"k\">Runtime WS</div><div class=\"v\" style=\"font-size:12px;word-break:break-all\">%s</div><div class=\"k\">official %s</div></div>",
        xiaozhi_runtime_ws_url[0] != '\0' ? xiaozhi_runtime_ws_url : "--",
        xiaozhi->official_runtime_config ? "yes" : "no");
    appendf(
        html,
        html_size,
        &used,
        "<div class=\"card\"><div class=\"k\">Active WS</div><div class=\"v\" style=\"font-size:12px;word-break:break-all\">%s</div><div class=\"k\">connected %s</div></div>",
        xiaozhi_active_ws_url[0] != '\0' ? xiaozhi_active_ws_url : "--",
        xiaozhi->connected ? "yes" : "no");
    append(html, html_size, &used, "</section>");
    append(
        html,
        html_size,
        &used,
        "<section class=\"ops\"><form method=\"post\" action=\"/save-xiaozhi\"><label class=\"k\">Xiaozhi WebSocket URL</label>"
        "<input name=\"xiaozhi_ws_url\" maxlength=\"191\" value=\"");
    append(html, html_size, &used, xiaozhi_ws_url);
    append(
        html,
        html_size,
        &used,
        "\"><label class=\"k\">Xiaozhi Token</label><input name=\"xiaozhi_token\" maxlength=\"159\" type=\"password\" placeholder=\"Leave blank to keep current token\">"
        "<div class=\"actions\"><button type=\"submit\">Save Xiaozhi</button>"
        "<button type=\"submit\" formaction=\"/test-xiaozhi\">Test AI</button>"
        "<button class=\"warn\" type=\"submit\" formaction=\"/xiaozhi-start\">Start AI</button>"
        "<button class=\"danger\" type=\"submit\" formaction=\"/xiaozhi-stop\">Stop AI</button></div>"
        "<footer>Start AI keeps Xiaozhi listening in the background. When idle, the screen returns to the quota or standby page. When wake-word or dialog activity appears, the screen switches back to Xiaozhi. Stop AI fully disables wake listening.</footer>"
        "</form></section>");
    append(html, html_size, &used, "<h2>Music</h2><section class=\"grid\">");
    appendf(
        html,
        html_size,
        &used,
        "<div class=\"card\"><div class=\"k\">State</div><div class=\"v\">%s</div><div class=\"k\">active %s stop %s</div></div>",
        music_player_state_name(music->state),
        music->active ? "yes" : "no",
        music->stop_requested ? "yes" : "no");
    appendf(
        html,
        html_size,
        &used,
        "<div class=\"card\"><div class=\"k\">Track</div><div class=\"v\">%s</div><div class=\"k\">%s</div></div>",
        music_title[0] != '\0' ? music_title : "--",
        music_artist[0] != '\0' ? music_artist : "--");
    appendf(
        html,
        html_size,
        &used,
        "<div class=\"card\"><div class=\"k\">Album</div><div class=\"v\">%s</div><div class=\"k\">cover %s lyrics %u B</div></div>",
        music_album[0] != '\0' ? music_album : "--",
        music->has_cover ? "yes" : "no",
        (unsigned int)strlen(music->lyrics));
    appendf(
        html,
        html_size,
        &used,
        "<div class=\"card\"><div class=\"k\">Playback</div><div class=\"v\">%u / %u ms</div><div class=\"k\">volume %d%% %s</div></div>",
        (unsigned int)music->playback_ms,
        (unsigned int)music->duration_ms,
        music->volume_percent,
        music_error[0] != '\0' ? music_error : "no error");
    append(
        html,
        html_size,
        &used,
        "</section><section class=\"ops\"><form method=\"post\" action=\"/play-music\">"
        "<label class=\"k\">Song</label><input name=\"song_name\" maxlength=\"63\" value=\"bad guy\">"
        "<label class=\"k\">Artist</label><input name=\"artist_name\" maxlength=\"63\" value=\"billie eilish\">"
        "<div class=\"actions\"><button type=\"submit\">Play Music</button>"
        "<button class=\"danger\" type=\"submit\" formaction=\"/stop-music\">Stop Music</button></div>"
        "<footer>Music commands are LAN-only web console tooling for validating bridge-resolved audio, lyrics, and cover art.</footer>"
        "</form></section>");
    appendf(
        html,
        html_size,
        &used,
        "<section class=\"ops\"><form method=\"post\" action=\"/save-audio\"><label class=\"k\">AI Assistant Volume</label>"
        "<input type=\"range\" name=\"audio_volume\" min=\"0\" max=\"100\" step=\"5\" value=\"%d\" oninput=\"audioVol.value=this.value\">"
        "<div class=\"v\"><output id=\"audioVol\">%d</output>%%</div>"
        "<div class=\"actions\"><button type=\"submit\">Save Volume</button>"
        "<button class=\"warn\" type=\"submit\" formaction=\"/test-audio\">Test Volume</button>"
        "<button class=\"warn\" type=\"submit\" formaction=\"/test-mic\">Test Mic</button></div></form></section>",
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
        "<button class=\"warn\" type=\"submit\" formaction=\"/save-bridge\">Save</button>"
        "<button class=\"warn\" type=\"submit\" formaction=\"/restart-bridge\">Restart Bridge</button></div></form>"
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
    free(xiaozhi);
    free(music);
    free(diag);
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

    esp_err_t err = settings_save_audio_volume_percent(volume_percent);
    if (err == ESP_OK) {
        console_settings.audio_volume_percent = volume_percent;
        task_audio_set_volume_percent(volume_percent);
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
        "</head><body><h1>AI Assistant Volume</h1><p>Volume: %d%%</p><p>%s</p><p><a href=\"/\">Back</a></p></body></html>",
        volume_percent,
        played ? "Test audio queued with this volume." : "Saved and applied to AI assistant speech.");

    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, html, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t send_simple_page(httpd_req_t *req, const char *title, const char *detail)
{
    char escaped_title[80];
    char escaped_detail[240];
    html_escape(title != NULL ? title : "Codex Ornament", escaped_title, sizeof(escaped_title));
    html_escape(detail != NULL ? detail : "", escaped_detail, sizeof(escaped_detail));

    char html[768];
    snprintf(
        html,
        sizeof(html),
        "<!doctype html><html><head><meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
        "<style>body{font-family:system-ui;margin:24px;background:#0b1116;color:#edf7fb}a{color:#49d3c8}</style>"
        "</head><body><h1>%s</h1><p>%s</p><p><a href=\"/\">Back</a></p></body></html>",
        escaped_title,
        escaped_detail);

    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, html, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t save_xiaozhi_settings(const char *ws_url, const char *token)
{
    ornament_settings_t settings;
    esp_err_t err = settings_load(&settings);
    if (err != ESP_OK) {
        return err;
    }

    if (ws_url != NULL) {
        strlcpy(settings.xiaozhi_ws_url, ws_url, sizeof(settings.xiaozhi_ws_url));
        settings.has_xiaozhi_ws_url = settings.xiaozhi_ws_url[0] != '\0';
    }
    if (token != NULL && token[0] != '\0') {
        strlcpy(settings.xiaozhi_token, token, sizeof(settings.xiaozhi_token));
        settings.has_xiaozhi_token = true;
    }

    err = settings_save(&settings);
    if (err == ESP_OK) {
        console_settings = settings;
        (void)xiaozhi_client_init();
        err = xiaozhi_client_reconnect_session(false);
        if (err == ESP_ERR_INVALID_STATE || err == ESP_ERR_NOT_SUPPORTED) {
            err = ESP_OK;
        }
    }
    return err;
}

static esp_err_t send_xiaozhi_page(httpd_req_t *req, const char *title, const char *detail)
{
    return send_simple_page(req, title != NULL ? title : "Xiaozhi AI", detail);
}

static esp_err_t send_xiaozhi_test_page(httpd_req_t *req, const xiaozhi_probe_result_t *result)
{
    if (result == NULL) {
        return send_xiaozhi_page(req, "Xiaozhi Test", "No test result.");
    }

    char escaped_detail[XIAOZHI_STATUS_TEXT_MAX * 6 + 1];
    char escaped_session_id[XIAOZHI_SESSION_ID_MAX * 6 + 1];
    html_escape(result->detail, escaped_detail, sizeof(escaped_detail));
    html_escape(result->session_id, escaped_session_id, sizeof(escaped_session_id));

    char html[3072];
    size_t used = 0;
    append(
        html,
        sizeof(html),
        &used,
        "<!doctype html><html><head><meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
        "<style>body{font-family:system-ui;margin:24px;background:#0b1116;color:#edf7fb}a{color:#49d3c8}"
        ".grid{display:grid;gap:12px;grid-template-columns:repeat(auto-fit,minmax(180px,1fr))}"
        ".card{padding:14px;border:1px solid #22303c;border-radius:8px;background:#111923}"
        ".k{font-size:12px;color:#8ea5b5;text-transform:uppercase}.v{font-size:20px;margin-top:6px}</style>"
        "</head><body><h1>Xiaozhi Test</h1><section class=\"grid\">");
    appendf(
        html,
        sizeof(html),
        &used,
        "<div class=\"card\"><div class=\"k\">Result</div><div class=\"v\">%s</div></div>"
        "<div class=\"card\"><div class=\"k\">WebSocket</div><div class=\"v\">%s</div></div>"
        "<div class=\"card\"><div class=\"k\">Hello</div><div class=\"v\">%s</div></div>"
        "<div class=\"card\"><div class=\"k\">HTTP Status</div><div class=\"v\">%d</div></div>",
        result->err == ESP_OK ? "ok" : esp_err_to_name(result->err),
        result->websocket_connected ? "connected" : "not connected",
        result->hello_received ? "received" : "not received",
        result->http_status);
    append(html, sizeof(html), &used, "</section><section class=\"grid\" style=\"margin-top:12px\">");
    appendf(
        html,
        sizeof(html),
        &used,
        "<div class=\"card\"><div class=\"k\">Session ID</div><div class=\"v\">%s</div></div>"
        "<div class=\"card\"><div class=\"k\">Detail</div><div class=\"v\">%s</div></div>",
        escaped_session_id[0] != '\0' ? escaped_session_id : "--",
        escaped_detail[0] != '\0' ? escaped_detail : "--");
    append(html, sizeof(html), &used, "</section><p><a href=\"/\">Back</a></p></body></html>");

    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, html, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t send_weather_saved_page(httpd_req_t *req, const char *title, const char *detail)
{
    return send_simple_page(req, title != NULL ? title : "Weather", detail);
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
    if (req->content_len > 0) {
        if (read_form_body(req, body, sizeof(body)) != ESP_OK) {
            return ESP_FAIL;
        }
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

static esp_err_t test_mic_post_handler(httpd_req_t *req)
{
    if (req->content_len > 0) {
        char body[128] = {0};
        (void)read_form_body(req, body, sizeof(body));
    }

    task_audio_mic_probe_result_t direct;
    task_audio_mic_probe_result_t with_tx_clock;
    (void)task_audio_input_probe(&direct, 500);
    (void)task_audio_input_probe_with_tx_clock(&with_tx_clock, 500);

    char detail[640];
    if (direct.start_err == ESP_ERR_TIMEOUT && with_tx_clock.start_err == ESP_ERR_TIMEOUT) {
        snprintf(
            detail,
            sizeof(detail),
            "Mic busy: the Xiaozhi AI session is already using the microphone. Stop AI first, then run Test Mic again. "
            "Last probe direct=%s/%s with_tx_clock=%s/%s",
            esp_err_to_name(direct.start_err),
            esp_err_to_name(direct.read_err),
            esp_err_to_name(with_tx_clock.start_err),
            esp_err_to_name(with_tx_clock.read_err));
    } else {
        snprintf(
            detail,
            sizeof(detail),
            "direct: start=%s read=%s frames=%u/%u nonzero=%u pos=%u neg=%u sat=%u zero_x=%u min=%d max=%d mean=%ld mean_abs=%u | "
            "with_tx_clock: start=%s read=%s frames=%u/%u nonzero=%u pos=%u neg=%u sat=%u zero_x=%u min=%d max=%d mean=%ld mean_abs=%u",
            esp_err_to_name(direct.start_err),
            esp_err_to_name(direct.read_err),
            (unsigned int)direct.frames_captured,
            (unsigned int)direct.frames_requested,
            (unsigned int)direct.nonzero_samples,
            (unsigned int)direct.positive_samples,
            (unsigned int)direct.negative_samples,
            (unsigned int)direct.saturated_samples,
            (unsigned int)direct.zero_crossings,
            direct.min_sample,
            direct.max_sample,
            (long)direct.mean_sample,
            (unsigned int)direct.mean_abs_sample,
            esp_err_to_name(with_tx_clock.start_err),
            esp_err_to_name(with_tx_clock.read_err),
            (unsigned int)with_tx_clock.frames_captured,
            (unsigned int)with_tx_clock.frames_requested,
            (unsigned int)with_tx_clock.nonzero_samples,
            (unsigned int)with_tx_clock.positive_samples,
            (unsigned int)with_tx_clock.negative_samples,
            (unsigned int)with_tx_clock.saturated_samples,
            (unsigned int)with_tx_clock.zero_crossings,
            with_tx_clock.min_sample,
            with_tx_clock.max_sample,
            (long)with_tx_clock.mean_sample,
            (unsigned int)with_tx_clock.mean_abs_sample);
     }

    return send_simple_page(req, "Mic Test", detail);
}

static esp_err_t save_xiaozhi_post_handler(httpd_req_t *req)
{
    char body[1024] = {0};
    char ws_url[ORNAMENT_XIAOZHI_WS_URL_MAX] = {0};
    char token[ORNAMENT_XIAOZHI_TOKEN_MAX] = {0};
    if (read_form_body(req, body, sizeof(body)) != ESP_OK) {
        return ESP_FAIL;
    }
    form_value(body, "xiaozhi_ws_url", ws_url, sizeof(ws_url));
    form_value(body, "xiaozhi_token", token, sizeof(token));

    esp_err_t err = save_xiaozhi_settings(ws_url, token);
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, esp_err_to_name(err));
        return ESP_FAIL;
    }
    return send_xiaozhi_page(req, "Xiaozhi Saved", "Saved settings applied. Running AI sessions were reconnected with the new WebSocket address.");
}

static esp_err_t xiaozhi_start_post_handler(httpd_req_t *req)
{
    if (req->content_len > 0) {
        char body[1024] = {0};
        char ws_url[ORNAMENT_XIAOZHI_WS_URL_MAX] = {0};
        char token[ORNAMENT_XIAOZHI_TOKEN_MAX] = {0};
        if (read_form_body(req, body, sizeof(body)) != ESP_OK) {
            return ESP_FAIL;
        }
        form_value(body, "xiaozhi_ws_url", ws_url, sizeof(ws_url));
        form_value(body, "xiaozhi_token", token, sizeof(token));
        esp_err_t save_err = save_xiaozhi_settings(ws_url, token);
        if (save_err != ESP_OK) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, esp_err_to_name(save_err));
            return ESP_FAIL;
        }
    }

    esp_err_t err = xiaozhi_client_reconnect_session(true);
    if (err != ESP_OK) {
        char detail[96];
        snprintf(detail, sizeof(detail), "Start failed: %s", esp_err_to_name(err));
        return send_xiaozhi_page(req, "Xiaozhi Start", detail);
    }
    return send_xiaozhi_page(req, "Xiaozhi Start", "AI session is starting with the latest saved WebSocket settings.");
}

static esp_err_t xiaozhi_stop_post_handler(httpd_req_t *req)
{
    if (req->content_len > 0) {
        char body[1024] = {0};
        (void)read_form_body(req, body, sizeof(body));
    }

    esp_err_t err = xiaozhi_client_stop_session();
    if (err != ESP_OK) {
        char detail[96];
        snprintf(detail, sizeof(detail), "Stop returned: %s", esp_err_to_name(err));
        return send_xiaozhi_page(req, "Xiaozhi Stop", detail);
    }
    return send_xiaozhi_page(req, "Xiaozhi Stop", "AI session stop requested.");
}

static esp_err_t xiaozhi_test_post_handler(httpd_req_t *req)
{
    char body[1024] = {0};
    char ws_url[ORNAMENT_XIAOZHI_WS_URL_MAX] = {0};
    char token[ORNAMENT_XIAOZHI_TOKEN_MAX] = {0};
    if (req->content_len > 0) {
        if (read_form_body(req, body, sizeof(body)) != ESP_OK) {
            return ESP_FAIL;
        }
        form_value(body, "xiaozhi_ws_url", ws_url, sizeof(ws_url));
        form_value(body, "xiaozhi_token", token, sizeof(token));
    }

    xiaozhi_probe_result_t result;
    esp_err_t err = xiaozhi_client_probe(
        ws_url[0] != '\0' ? ws_url : NULL,
        token[0] != '\0' ? token : NULL,
        &result);
    if (err != ESP_OK && result.detail[0] == '\0') {
        strlcpy(result.detail, esp_err_to_name(err), sizeof(result.detail));
    }
    return send_xiaozhi_test_page(req, &result);
}

static esp_err_t play_music_post_handler(httpd_req_t *req)
{
    char body[256] = {0};
    char song_name[ORNAMENT_TEXT_MAX] = {0};
    char artist_name[ORNAMENT_TEXT_MAX] = {0};
    if (read_form_body(req, body, sizeof(body)) != ESP_OK) {
        return ESP_FAIL;
    }
    form_value(body, "song_name", song_name, sizeof(song_name));
    form_value(body, "artist_name", artist_name, sizeof(artist_name));
    if (song_name[0] == '\0') {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Song is required");
        return ESP_FAIL;
    }

    esp_err_t err = music_player_play_song_with_settings(
        song_name,
        artist_name[0] != '\0' ? artist_name : NULL,
        1,
        &console_settings);
    char detail[160];
    snprintf(
        detail,
        sizeof(detail),
        err == ESP_OK ? "Music start queued: %s" : "Music start failed: %s",
        err == ESP_OK ? song_name : esp_err_to_name(err));
    return send_simple_page(req, "Music", detail);
}

static esp_err_t stop_music_post_handler(httpd_req_t *req)
{
    if (req->content_len > 0) {
        char body[256] = {0};
        (void)read_form_body(req, body, sizeof(body));
    }
    music_player_request_stop();
    return send_simple_page(req, "Music", "Music stop requested.");
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

static esp_err_t restart_bridge_post_handler(httpd_req_t *req)
{
    char body[256] = {0};
    char url[ORNAMENT_BRIDGE_URL_MAX] = {0};
    if (req->content_len > 0) {
        if (read_form_body(req, body, sizeof(body)) != ESP_OK) {
            return ESP_FAIL;
        }
        form_value(body, "bridge_url", url, sizeof(url));
        if (url[0] != '\0') {
            esp_err_t save_err = save_bridge_url(url);
            if (save_err != ESP_OK) {
                httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, esp_err_to_name(save_err));
                return ESP_FAIL;
            }
            refresh_console_settings();
        }
    }

    const char *active_url = settings_bridge_url_or_default(&console_settings);
    esp_err_t err = bridge_client_restart();
    if (err != ESP_OK) {
        char detail[160];
        snprintf(
            detail,
            sizeof(detail),
            "Bridge restart failed: %s",
            esp_err_to_name(err));
        return send_bridge_saved_page(req, active_url, detail);
    }

    return send_bridge_saved_page(
        req,
        active_url,
        "Bridge restart requested on this PC. The bridge should come back automatically in a moment.");
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
    config.max_uri_handlers = 22;
    config.stack_size = 32768;

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
    const httpd_uri_t test_mic = {
        .uri = "/test-mic",
        .method = HTTP_POST,
        .handler = test_mic_post_handler,
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
    const httpd_uri_t restart_bridge = {
        .uri = "/restart-bridge",
        .method = HTTP_POST,
        .handler = restart_bridge_post_handler,
    };
    const httpd_uri_t save_xiaozhi = {
        .uri = "/save-xiaozhi",
        .method = HTTP_POST,
        .handler = save_xiaozhi_post_handler,
    };
    const httpd_uri_t xiaozhi_start = {
        .uri = "/xiaozhi-start",
        .method = HTTP_POST,
        .handler = xiaozhi_start_post_handler,
    };
    const httpd_uri_t xiaozhi_stop = {
        .uri = "/xiaozhi-stop",
        .method = HTTP_POST,
        .handler = xiaozhi_stop_post_handler,
    };
    const httpd_uri_t xiaozhi_test = {
        .uri = "/test-xiaozhi",
        .method = HTTP_POST,
        .handler = xiaozhi_test_post_handler,
    };
    const httpd_uri_t play_music = {
        .uri = "/play-music",
        .method = HTTP_POST,
        .handler = play_music_post_handler,
    };
    const httpd_uri_t stop_music = {
        .uri = "/stop-music",
        .method = HTTP_POST,
        .handler = stop_music_post_handler,
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
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &test_mic));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &save_weather));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &clear_weather_token));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &auto_bridge));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &restart_bridge));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &save_xiaozhi));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &xiaozhi_start));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &xiaozhi_stop));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &xiaozhi_test));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &play_music));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &stop_music));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &reboot));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &clear_config));
    ESP_LOGI(TAG, "web console started on http://<device-ip>/");
    return ESP_OK;
}

#else

esp_err_t web_console_start(void)
{
    return ESP_OK;
}

void web_console_set_last_state(const ornament_state_t *state, esp_err_t fetch_error)
{
    (void)state;
    (void)fetch_error;
}

void web_console_set_bridge_debug(const web_console_bridge_debug_t *debug)
{
    (void)debug;
}

#endif
