#include "bridge_client.h"

#include "cJSON.h"
#include "esp_check.h"
#include "esp_log.h"
#include "ornament_http_client.h"
#include "settings.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"

#include <limits.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

static const char *TAG = "bridge_client";
static const int MAX_RESPONSE_BYTES = 8192;
static const int BRIDGE_FETCH_TIMEOUT_MS = 10000;
static const int DISCOVERY_UDP_PORT = 8787;
static const char *DISCOVERY_MAGIC = "codex-ornament-discover-v1";
static const int DISCOVERY_TIMEOUT_MS = 1200;
static const int BRIDGE_PROBE_TIMEOUT_MS = 900;
static const int BRIDGE_MDNS_PROBE_TIMEOUT_MS = 3000;
static const int SUBNET_PROBE_RADIUS = 8;

static void copy_json_string(cJSON *parent, const char *name, char *target, size_t target_size)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(parent, name);
    if (!cJSON_IsString(item) || item->valuestring == NULL || target_size == 0) {
        return;
    }
    strlcpy(target, item->valuestring, target_size);
}

static int json_percent(cJSON *parent, const char *name)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(parent, name);
    if (cJSON_IsNumber(item)) {
        return (int)item->valuedouble;
    }
    return -1;
}

static int json_nonnegative_int(cJSON *parent, const char *name)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(parent, name);
    if (cJSON_IsNumber(item) && item->valuedouble > 0 && item->valuedouble <= INT_MAX) {
        return (int)item->valuedouble;
    }
    return 0;
}

static int json_optional_int(cJSON *parent, const char *name, int fallback)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(parent, name);
    if (cJSON_IsNumber(item) && item->valuedouble >= INT_MIN && item->valuedouble <= INT_MAX) {
        return (int)item->valuedouble;
    }
    return fallback;
}

static void result_set_error(bridge_auto_match_result_t *result, esp_err_t err)
{
    if (result != NULL) {
        result->last_error = err;
    }
}

static void result_set_success(
    bridge_auto_match_result_t *result,
    const char *url,
    const char *source,
    bool current_ok,
    bool saved)
{
    if (result == NULL) {
        return;
    }

    result->last_error = ESP_OK;
    result->current_ok = current_ok;
    result->saved = saved;
    strlcpy(result->bridge_url, url, sizeof(result->bridge_url));
    strlcpy(result->source, source, sizeof(result->source));
}

static void parse_source_task(
    cJSON *source_tasks,
    const char *name,
    bool *has_summary,
    bool *has_task,
    ornament_status_t *status,
    int *active_count,
    int *done_seq,
    char *title,
    size_t title_size,
    char *message,
    size_t message_size,
    char *session_id,
    size_t session_id_size,
    char *turn_id,
    size_t turn_id_size)
{
    cJSON *summary = cJSON_GetObjectItemCaseSensitive(source_tasks, name);
    if (!cJSON_IsObject(summary)) {
        return;
    }

    *has_summary = true;
    char status_text[16] = {0};
    copy_json_string(summary, "status", status_text, sizeof(status_text));
    *status = ornament_status_from_text(status_text);
    *active_count = json_nonnegative_int(summary, "activeCount");
    *done_seq = json_nonnegative_int(summary, "doneSeq");

    cJSON *task = cJSON_GetObjectItemCaseSensitive(summary, "task");
    if (cJSON_IsObject(task)) {
        *has_task = true;
        copy_json_string(task, "title", title, title_size);
        copy_json_string(task, "message", message, message_size);
        copy_json_string(task, "sessionId", session_id, session_id_size);
        copy_json_string(task, "turnId", turn_id, turn_id_size);
    }
}

static esp_err_t parse_state_json(const char *json_text, ornament_state_t *state)
{
    cJSON *root = cJSON_Parse(json_text);
    if (root == NULL) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    ornament_state_init(state);
    copy_json_string(root, "status", state->quota_status, sizeof(state->quota_status));
    state->status = ornament_status_from_text(state->quota_status);
    state->active_task_count = json_nonnegative_int(root, "activeTaskCount");
    state->done_seq = json_nonnegative_int(root, "doneSeq");

    cJSON *task = cJSON_GetObjectItemCaseSensitive(root, "task");
    if (cJSON_IsObject(task)) {
        state->has_task = true;
        copy_json_string(task, "title", state->task_title, sizeof(state->task_title));
        copy_json_string(task, "message", state->task_message, sizeof(state->task_message));
        copy_json_string(task, "receivedAt", state->task_received_at, sizeof(state->task_received_at));
        copy_json_string(task, "sessionId", state->task_session_id, sizeof(state->task_session_id));
        copy_json_string(task, "turnId", state->task_turn_id, sizeof(state->task_turn_id));
    }

    cJSON *source_tasks = cJSON_GetObjectItemCaseSensitive(root, "sourceTasks");
    if (cJSON_IsObject(source_tasks)) {
        parse_source_task(
            source_tasks,
            "codex",
            &state->has_codex_summary,
            &state->has_codex_task,
            &state->codex_task_status,
            &state->codex_active_task_count,
            &state->codex_done_seq,
            state->codex_task_title,
            sizeof(state->codex_task_title),
            state->codex_task_message,
            sizeof(state->codex_task_message),
            state->codex_task_session_id,
            sizeof(state->codex_task_session_id),
            state->codex_task_turn_id,
            sizeof(state->codex_task_turn_id));
        parse_source_task(
            source_tasks,
            "claude",
            &state->has_claude_summary,
            &state->has_claude_task,
            &state->claude_task_status,
            &state->claude_active_task_count,
            &state->claude_done_seq,
            state->claude_task_title,
            sizeof(state->claude_task_title),
            state->claude_task_message,
            sizeof(state->claude_task_message),
            state->claude_task_session_id,
            sizeof(state->claude_task_session_id),
            state->claude_task_turn_id,
            sizeof(state->claude_task_turn_id));
    }

    cJSON *quota = cJSON_GetObjectItemCaseSensitive(root, "quota");
    if (cJSON_IsObject(quota)) {
        state->has_quota = true;
        copy_json_string(quota, "status", state->quota_status, sizeof(state->quota_status));
        state->primary_remaining_percent = json_percent(quota, "primaryRemainingPercent");
        state->secondary_remaining_percent = json_percent(quota, "secondaryRemainingPercent");
        copy_json_string(quota, "primaryResetsAt", state->primary_resets_at, sizeof(state->primary_resets_at));
        copy_json_string(quota, "secondaryResetsAt", state->secondary_resets_at, sizeof(state->secondary_resets_at));
    }

    cJSON *bridge = cJSON_GetObjectItemCaseSensitive(root, "bridge");
    if (cJSON_IsObject(bridge)) {
        copy_json_string(bridge, "observedAt", state->bridge_observed_at, sizeof(state->bridge_observed_at));
    }

    cJSON *weather = cJSON_GetObjectItemCaseSensitive(root, "weather");
    if (cJSON_IsObject(weather)) {
        state->has_weather = true;
        copy_json_string(weather, "status", state->weather_status, sizeof(state->weather_status));
        copy_json_string(weather, "label", state->weather_label, sizeof(state->weather_label));
        strlcpy(state->weather_source, "bridge", sizeof(state->weather_source));
        copy_json_string(weather, "summary", state->weather_summary, sizeof(state->weather_summary));
        copy_json_string(weather, "icon", state->weather_icon, sizeof(state->weather_icon));
        copy_json_string(weather, "observedAt", state->weather_observed_at, sizeof(state->weather_observed_at));
        state->weather_temperature_c = json_optional_int(weather, "temperatureC", INT32_MIN);
        state->weather_wind_kmh = json_optional_int(weather, "windKmh", -1);
        state->weather_code = json_optional_int(weather, "weatherCode", -1);
    }

    cJSON *standby_wallpaper = cJSON_GetObjectItemCaseSensitive(root, "standbyWallpaper");
    if (cJSON_IsObject(standby_wallpaper)) {
        state->has_standby_wallpaper = true;
        copy_json_string(standby_wallpaper, "id", state->standby_wallpaper_id, sizeof(state->standby_wallpaper_id));
        copy_json_string(standby_wallpaper, "name", state->standby_wallpaper_name, sizeof(state->standby_wallpaper_name));
        copy_json_string(standby_wallpaper, "mode", state->standby_wallpaper_mode, sizeof(state->standby_wallpaper_mode));
        copy_json_string(standby_wallpaper, "url", state->standby_wallpaper_url, sizeof(state->standby_wallpaper_url));
        state->standby_wallpaper_index = json_optional_int(standby_wallpaper, "index", -1);
        state->standby_wallpaper_total = json_optional_int(standby_wallpaper, "total", 0);
    }

    cJSON_Delete(root);
    return ESP_OK;
}

static esp_err_t fetch_url_raw(const char *url, char *response, int response_capacity, int timeout_ms, int *status_code, int *response_len)
{
    if (url == NULL || url[0] == '\0' || response == NULL || response_capacity <= 0) {
        return ESP_ERR_INVALID_ARG;
    }

    size_t received = 0;
    ornament_http_request_t request = {
        .method = "GET",
        .url = url,
        .response = response,
        .response_capacity = (size_t)response_capacity,
        .response_len = &received,
        .status_code = status_code,
        .timeout_ms = timeout_ms,
    };

    esp_err_t err = ornament_http_request(&request);
    if (response_len != NULL) {
        *response_len = (int)received;
    }
    return err;
}

static bool url_uses_mdns_host(const char *url)
{
    if (url == NULL) {
        return false;
    }

    const char *host = strstr(url, "://");
    if (host == NULL) {
        host = url;
    } else {
        host += 3;
    }

    size_t host_len = strcspn(host, ":/?#");
    return host_len > strlen(".local") &&
           strncmp(host + host_len - strlen(".local"), ".local", strlen(".local")) == 0;
}

static esp_err_t save_bridge_url(const char *url)
{
    if (url == NULL || url[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    ornament_settings_t settings;
    ESP_RETURN_ON_ERROR(settings_load(&settings), TAG, "settings load failed before bridge save");
    strlcpy(settings.bridge_url, url, sizeof(settings.bridge_url));
    settings.has_bridge_url = true;
    return settings_save(&settings);
}

static bool bridge_url_is_config_default(const char *url)
{
    return url != NULL && strcmp(url, CONFIG_ORNAMENT_BRIDGE_URL) == 0;
}

static esp_err_t probe_and_maybe_save(
    const char *url,
    const char *source,
    bool save,
    bridge_auto_match_result_t *result)
{
    if (url == NULL || url[0] == '\0') {
        result_set_error(result, ESP_ERR_INVALID_ARG);
        return ESP_ERR_INVALID_ARG;
    }

    if (result != NULL) {
        result->tested_count++;
    }

    bridge_probe_result_t probe;
    esp_err_t err = bridge_client_probe_url(url, &probe);
    if (err != ESP_OK) {
        result_set_error(result, err);
        return err;
    }

    if (save) {
        err = save_bridge_url(url);
        if (err != ESP_OK) {
            result_set_error(result, err);
            return err;
        }
    }

    result_set_success(result, url, source, !save, save);
    return ESP_OK;
}

static esp_err_t parse_discovery_response(const char *response, char *url, size_t url_size)
{
    cJSON *root = cJSON_Parse(response);
    if (root == NULL) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    char service[40] = {0};
    copy_json_string(root, "service", service, sizeof(service));
    copy_json_string(root, "stateUrl", url, url_size);
    cJSON_Delete(root);

    if (strcmp(service, "codex-ornament-bridge") != 0 || url == NULL || url[0] == '\0') {
        return ESP_ERR_INVALID_RESPONSE;
    }
    return ESP_OK;
}

static esp_err_t discover_bridge_url(char *url, size_t url_size)
{
    if (url == NULL || url_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    url[0] = '\0';

    int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (fd < 0) {
        return ESP_FAIL;
    }

    int broadcast = 1;
    (void)setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &broadcast, sizeof(broadcast));

    struct timeval timeout = {
        .tv_sec = DISCOVERY_TIMEOUT_MS / 1000,
        .tv_usec = (DISCOVERY_TIMEOUT_MS % 1000) * 1000,
    };
    (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

    struct sockaddr_in destination = {0};
    destination.sin_family = AF_INET;
    destination.sin_port = htons(DISCOVERY_UDP_PORT);
    destination.sin_addr.s_addr = htonl(INADDR_BROADCAST);

    ssize_t sent = sendto(
        fd,
        DISCOVERY_MAGIC,
        strlen(DISCOVERY_MAGIC),
        0,
        (const struct sockaddr *)&destination,
        sizeof(destination));
    if (sent < 0) {
        close(fd);
        return ESP_FAIL;
    }

    char response[256] = {0};
    struct sockaddr_in source = {0};
    socklen_t source_len = sizeof(source);
    ssize_t received = recvfrom(fd, response, sizeof(response) - 1, 0, (struct sockaddr *)&source, &source_len);
    close(fd);

    if (received <= 0) {
        return ESP_ERR_TIMEOUT;
    }
    response[received] = '\0';
    return parse_discovery_response(response, url, url_size);
}

static bool parse_bridge_ipv4(const char *url, uint8_t octets[4], uint16_t *port)
{
    int a = 0;
    int b = 0;
    int c = 0;
    int d = 0;
    int parsed_port = 0;
    if (url == NULL) {
        return false;
    }

    if (sscanf(url, "http://%d.%d.%d.%d:%d/state", &a, &b, &c, &d, &parsed_port) != 5) {
        return false;
    }
    if (a < 1 || a > 223 || b < 0 || b > 255 || c < 0 || c > 255 || d < 1 || d > 254 ||
        parsed_port < 1 || parsed_port > 65535) {
        return false;
    }

    octets[0] = (uint8_t)a;
    octets[1] = (uint8_t)b;
    octets[2] = (uint8_t)c;
    octets[3] = (uint8_t)d;
    *port = (uint16_t)parsed_port;
    return true;
}

static int candidate_last_octet(uint8_t base, int index)
{
    if (index == 0) {
        return base;
    }

    int offset = (index + 1) / 2;
    return (index % 2) == 1 ? (int)base - offset : (int)base + offset;
}

static esp_err_t probe_near_saved_bridge(
    const char *saved_url,
    bridge_auto_match_result_t *result)
{
    uint8_t octets[4] = {0};
    uint16_t port = 0;
    if (!parse_bridge_ipv4(saved_url, octets, &port)) {
        result_set_error(result, ESP_ERR_NOT_SUPPORTED);
        return ESP_ERR_NOT_SUPPORTED;
    }

    esp_err_t last_err = ESP_FAIL;
    char candidate[ORNAMENT_BRIDGE_URL_MAX] = {0};
    for (int i = 0; i <= SUBNET_PROBE_RADIUS * 2; i++) {
        int last = candidate_last_octet(octets[3], i);
        if (last <= 0 || last >= 255 || last == octets[3]) {
            continue;
        }

        snprintf(
            candidate,
            sizeof(candidate),
            "http://%u.%u.%u.%d:%u/state",
            octets[0],
            octets[1],
            octets[2],
            last,
            port);
        last_err = probe_and_maybe_save(candidate, "near_saved_subnet", true, result);
        if (last_err == ESP_OK) {
            return ESP_OK;
        }
    }

    result_set_error(result, last_err);
    return last_err;
}

esp_err_t bridge_client_fetch_state(ornament_state_t *state)
{
    ornament_settings_t settings;
    ESP_RETURN_ON_ERROR(settings_load(&settings), TAG, "settings load failed");

    char *response = calloc(MAX_RESPONSE_BYTES, 1);
    if (response == NULL) {
        return ESP_ERR_NO_MEM;
    }

    int status_code = 0;
    int response_len = 0;
    esp_err_t err = fetch_url_raw(
        settings_bridge_url_or_default(&settings),
        response,
        MAX_RESPONSE_BYTES,
        BRIDGE_FETCH_TIMEOUT_MS,
        &status_code,
        &response_len);
    (void)response_len;

    if (err == ESP_OK && status_code == 200) {
        err = parse_state_json(response, state);
    } else if (err == ESP_OK) {
        err = ESP_ERR_INVALID_RESPONSE;
    }

    free(response);
    return err;
}

esp_err_t bridge_client_probe_url(const char *url, bridge_probe_result_t *result)
{
    if (result == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(result, 0, sizeof(*result));

    char *response = calloc(MAX_RESPONSE_BYTES, 1);
    if (response == NULL) {
        result->error = ESP_ERR_NO_MEM;
        strlcpy(result->error_name, esp_err_to_name(result->error), sizeof(result->error_name));
        return result->error;
    }

    bool mdns_host = url_uses_mdns_host(url);
    int timeout_ms = mdns_host ? BRIDGE_MDNS_PROBE_TIMEOUT_MS : BRIDGE_PROBE_TIMEOUT_MS;
    if (mdns_host) {
        ESP_LOGI(TAG, "probing bridge mDNS URL: %s timeout_ms=%d", url, timeout_ms);
    }

    int status_code = 0;
    int response_len = 0;
    esp_err_t err = fetch_url_raw(url, response, MAX_RESPONSE_BYTES, timeout_ms, &status_code, &response_len);
    result->error = err;
    result->http_status = status_code;
    result->response_bytes = response_len;
    strlcpy(result->error_name, esp_err_to_name(err), sizeof(result->error_name));

    if (err == ESP_OK && status_code == 200) {
        cJSON *root = cJSON_Parse(response);
        result->json_ok = cJSON_IsObject(root);
        if (result->json_ok) {
            cJSON *quota = cJSON_GetObjectItemCaseSensitive(root, "quota");
            cJSON *task = cJSON_GetObjectItemCaseSensitive(root, "task");
            if (cJSON_IsObject(quota)) {
                copy_json_string(quota, "status", result->status_text, sizeof(result->status_text));
            }
            if (result->status_text[0] == '\0' && cJSON_IsObject(task)) {
                copy_json_string(task, "title", result->status_text, sizeof(result->status_text));
            }
            if (result->status_text[0] == '\0') {
                strlcpy(result->status_text, "json_ok", sizeof(result->status_text));
            }
        }
        cJSON_Delete(root);
    }

    free(response);
    return result->json_ok ? ESP_OK : (err == ESP_OK ? ESP_ERR_INVALID_RESPONSE : err);
}

esp_err_t bridge_client_auto_match(bool verify_current, bridge_auto_match_result_t *result)
{
    if (result == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(result, 0, sizeof(*result));
    result->last_error = ESP_ERR_NOT_FOUND;

    ornament_settings_t settings;
    ESP_RETURN_ON_ERROR(settings_load(&settings), TAG, "settings load failed before auto match");
    const char *current_url = settings_bridge_url_or_default(&settings);

    if (verify_current && current_url[0] != '\0' && !bridge_url_is_config_default(current_url)) {
        esp_err_t err = probe_and_maybe_save(current_url, "current", false, result);
        if (err == ESP_OK) {
            return ESP_OK;
        }
    }

    char discovered_url[ORNAMENT_BRIDGE_URL_MAX] = {0};
    esp_err_t err = discover_bridge_url(discovered_url, sizeof(discovered_url));
    if (err == ESP_OK) {
        err = probe_and_maybe_save(discovered_url, "udp_discovery", true, result);
        if (err == ESP_OK) {
            return ESP_OK;
        }
    } else {
        result_set_error(result, err);
        ESP_LOGW(TAG, "bridge UDP discovery failed: %s", esp_err_to_name(err));
    }

    if (current_url[0] != '\0' && !bridge_url_is_config_default(current_url)) {
        err = probe_near_saved_bridge(current_url, result);
        if (err == ESP_OK) {
            return ESP_OK;
        }
    }

    if (!verify_current && current_url[0] != '\0' && !bridge_url_is_config_default(current_url)) {
        err = probe_and_maybe_save(current_url, "current", false, result);
        if (err == ESP_OK) {
            return ESP_OK;
        }
    }

    if (result->last_error == ESP_OK) {
        result->last_error = ESP_ERR_NOT_FOUND;
    }
    return result->last_error;
}

esp_err_t bridge_client_restart(void)
{
    ornament_settings_t settings;
    ESP_RETURN_ON_ERROR(settings_load(&settings), TAG, "settings load failed before bridge restart");

    char response[256] = {0};
    char restart_url[ORNAMENT_BRIDGE_URL_MAX + 16] = {0};
    const char *state_url = settings_bridge_url_or_default(&settings);
    if (state_url == NULL || state_url[0] == '\0') {
        return ESP_ERR_INVALID_STATE;
    }

    const char *suffix = strstr(state_url, "/state");
    size_t base_len = suffix != NULL ? (size_t)(suffix - state_url) : strlen(state_url);
    if (base_len + strlen("/restart") + 1 > sizeof(restart_url)) {
        return ESP_ERR_INVALID_SIZE;
    }
    memcpy(restart_url, state_url, base_len);
    restart_url[base_len] = '\0';
    strlcat(restart_url, "/restart", sizeof(restart_url));

    size_t response_len = 0;
    int status_code = 0;
    ornament_http_request_t request = {
        .method = "POST",
        .url = restart_url,
        .body = "",
        .response = response,
        .response_capacity = sizeof(response),
        .response_len = &response_len,
        .status_code = &status_code,
        .timeout_ms = BRIDGE_PROBE_TIMEOUT_MS,
    };

    esp_err_t err = ornament_http_request(&request);
    if (err != ESP_OK) {
        return err;
    }
    return status_code == 202 ? ESP_OK : ESP_ERR_INVALID_RESPONSE;
}
