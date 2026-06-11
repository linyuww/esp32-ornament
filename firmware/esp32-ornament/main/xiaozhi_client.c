#include "xiaozhi_client.h"

#include "cJSON.h"
#include "device_identity.h"
#include "esp_check.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "settings.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef CONFIG_ORNAMENT_XIAOZHI_PROTOCOL_VERSION
#define CONFIG_ORNAMENT_XIAOZHI_PROTOCOL_VERSION 1
#endif

#if CONFIG_ORNAMENT_XIAOZHI_ENABLED && CONFIG_ORNAMENT_XIAOZHI_TRANSPORT_BRIDGE

#define XIAOZHI_BRIDGE_RESPONSE_MAX 1536
#define XIAOZHI_BRIDGE_URL_MAX 192
#define XIAOZHI_BRIDGE_SESSION_ID_DEFAULT "bridge"

static const char *TAG = "xiaozhi_bridge";

typedef struct {
    char *data;
    int length;
    int capacity;
} xiaozhi_bridge_response_t;

static SemaphoreHandle_t s_mutex;
static xiaozhi_client_snapshot_t s_snapshot;
static bool s_initialized;

static esp_err_t http_event_handler(esp_http_client_event_t *event)
{
    xiaozhi_bridge_response_t *buffer = (xiaozhi_bridge_response_t *)event->user_data;
    if (event->event_id != HTTP_EVENT_ON_DATA || buffer == NULL || event->data == NULL) {
        return ESP_OK;
    }
    if (buffer->length + event->data_len >= buffer->capacity) {
        ESP_LOGW(TAG, "bridge response too large");
        return ESP_FAIL;
    }
    memcpy(buffer->data + buffer->length, event->data, event->data_len);
    buffer->length += event->data_len;
    buffer->data[buffer->length] = '\0';
    return ESP_OK;
}

static bool string_ends_with_len(const char *value, size_t value_len, const char *suffix)
{
    size_t suffix_len = strlen(suffix);
    return value != NULL && value_len >= suffix_len &&
           strncmp(value + value_len - suffix_len, suffix, suffix_len) == 0;
}

static esp_err_t resolve_bridge_base_url(const ornament_settings_t *settings, char *target, size_t target_size)
{
    if (target == NULL || target_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    target[0] = '\0';

    const char *url = settings_bridge_url_or_default(settings);
    if (url == NULL || url[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    size_t base_len = strcspn(url, "?");
    while (base_len > 0 && url[base_len - 1] == '/') {
        base_len--;
    }

    if (string_ends_with_len(url, base_len, "/state")) {
        base_len -= strlen("/state");
    } else if (string_ends_with_len(url, base_len, "/v1/xiaozhi/session/start")) {
        base_len -= strlen("/v1/xiaozhi/session/start");
    } else if (string_ends_with_len(url, base_len, "/v1/xiaozhi/session/stop")) {
        base_len -= strlen("/v1/xiaozhi/session/stop");
    } else if (string_ends_with_len(url, base_len, "/v1/xiaozhi/session/status")) {
        base_len -= strlen("/v1/xiaozhi/session/status");
    }

    while (base_len > 0 && url[base_len - 1] == '/') {
        base_len--;
    }
    if (base_len == 0) {
        return ESP_ERR_INVALID_STATE;
    }

    int written = snprintf(target, target_size, "%.*s", (int)base_len, url);
    return written > 0 && written < (int)target_size ? ESP_OK : ESP_ERR_NO_MEM;
}

static esp_err_t build_bridge_endpoint(
    const ornament_settings_t *settings,
    const char *path,
    char *target,
    size_t target_size)
{
    char base[XIAOZHI_BRIDGE_URL_MAX] = {0};
    ESP_RETURN_ON_ERROR(resolve_bridge_base_url(settings, base, sizeof(base)), TAG, "resolve bridge base");
    int written = snprintf(target, target_size, "%s%s", base, path);
    return written > 0 && written < (int)target_size ? ESP_OK : ESP_ERR_NO_MEM;
}

static esp_err_t bridge_request(
    const char *url,
    esp_http_client_method_t method,
    const char *body,
    char *response,
    int response_capacity,
    int *http_status)
{
    if (url == NULL || url[0] == '\0' || response == NULL || response_capacity <= 0) {
        return ESP_ERR_INVALID_ARG;
    }

    xiaozhi_bridge_response_t buffer = {
        .data = response,
        .length = 0,
        .capacity = response_capacity,
    };
    esp_http_client_config_t config = {
        .url = url,
        .event_handler = http_event_handler,
        .user_data = &buffer,
        .timeout_ms = CONFIG_ORNAMENT_XIAOZHI_BRIDGE_TIMEOUT_MS,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        return ESP_FAIL;
    }

    esp_http_client_set_method(client, method);
    if (body != NULL) {
        esp_http_client_set_header(client, "Content-Type", "application/json");
        esp_http_client_set_post_field(client, body, strlen(body));
    }

    esp_err_t err = esp_http_client_perform(client);
    if (http_status != NULL) {
        *http_status = esp_http_client_get_status_code(client);
    }
    esp_http_client_cleanup(client);
    return err;
}

static const char *json_string_or_empty(cJSON *parent, const char *name)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(parent, name);
    return cJSON_IsString(item) && item->valuestring != NULL ? item->valuestring : "";
}

static bool json_bool_or_false(cJSON *parent, const char *name)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(parent, name);
    return cJSON_IsTrue(item);
}

static uint32_t json_u32_or_current(cJSON *parent, const char *name, uint32_t current)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(parent, name);
    if (!cJSON_IsNumber(item) || item->valuedouble < 0.0 || item->valuedouble > UINT32_MAX) {
        return current;
    }
    return (uint32_t)item->valuedouble;
}

static xiaozhi_client_state_t state_from_bridge_text(const char *text)
{
    if (text == NULL) {
        return XIAOZHI_CLIENT_STATE_ERROR;
    }
    if (strcmp(text, "idle") == 0) {
        return XIAOZHI_CLIENT_STATE_IDLE;
    }
    if (strcmp(text, "configMissing") == 0 || strcmp(text, "config_missing") == 0) {
        return XIAOZHI_CLIENT_STATE_CONFIG_MISSING;
    }
    if (strcmp(text, "connecting") == 0) {
        return XIAOZHI_CLIENT_STATE_CONNECTING;
    }
    if (strcmp(text, "listening") == 0) {
        return XIAOZHI_CLIENT_STATE_LISTENING;
    }
    if (strcmp(text, "speaking") == 0) {
        return XIAOZHI_CLIENT_STATE_SPEAKING;
    }
    if (strcmp(text, "disabled") == 0) {
        return XIAOZHI_CLIENT_STATE_DISABLED;
    }
    return XIAOZHI_CLIENT_STATE_ERROR;
}

static const char *state_to_bridge_text(xiaozhi_client_state_t state)
{
    switch (state) {
    case XIAOZHI_CLIENT_STATE_DISABLED:
        return "disabled";
    case XIAOZHI_CLIENT_STATE_IDLE:
        return "idle";
    case XIAOZHI_CLIENT_STATE_CONFIG_MISSING:
        return "configMissing";
    case XIAOZHI_CLIENT_STATE_CONNECTING:
        return "connecting";
    case XIAOZHI_CLIENT_STATE_LISTENING:
        return "listening";
    case XIAOZHI_CLIENT_STATE_SPEAKING:
        return "speaking";
    case XIAOZHI_CLIENT_STATE_ERROR:
    default:
        return "error";
    }
}

static void apply_status_json_locked(const char *json_text)
{
    cJSON *root = cJSON_Parse(json_text);
    if (root == NULL) {
        s_snapshot.state = XIAOZHI_CLIENT_STATE_ERROR;
        strlcpy(s_snapshot.last_error, "invalid bridge response", sizeof(s_snapshot.last_error));
        return;
    }

    cJSON *ok = cJSON_GetObjectItemCaseSensitive(root, "ok");
    if (cJSON_IsBool(ok) && !cJSON_IsTrue(ok)) {
        s_snapshot.state = XIAOZHI_CLIENT_STATE_ERROR;
        strlcpy(s_snapshot.last_error, json_string_or_empty(root, "error"), sizeof(s_snapshot.last_error));
        cJSON_Delete(root);
        return;
    }

    const char *state_text = json_string_or_empty(root, "state");
    s_snapshot.state = state_from_bridge_text(state_text);
    s_snapshot.configured = json_bool_or_false(root, "configured");
    s_snapshot.connected = json_bool_or_false(root, "connected");
    s_snapshot.session_requested = json_bool_or_false(root, "sessionRequested");
    s_snapshot.official_runtime_config = json_bool_or_false(root, "runtimeConfig");
    s_snapshot.activation_pending = json_bool_or_false(root, "activationPending");
    s_snapshot.uplink_frames = json_u32_or_current(root, "uplinkFrames", s_snapshot.uplink_frames);
    s_snapshot.downlink_frames = json_u32_or_current(root, "downlinkFrames", s_snapshot.downlink_frames);

    strlcpy(s_snapshot.session_id, json_string_or_empty(root, "sessionId"), sizeof(s_snapshot.session_id));
    strlcpy(s_snapshot.last_error, json_string_or_empty(root, "lastError"), sizeof(s_snapshot.last_error));
    strlcpy(s_snapshot.last_stt, json_string_or_empty(root, "lastStt"), sizeof(s_snapshot.last_stt));
    strlcpy(s_snapshot.last_tts, json_string_or_empty(root, "lastTts"), sizeof(s_snapshot.last_tts));
    strlcpy(s_snapshot.activation_code, json_string_or_empty(root, "activationCode"), sizeof(s_snapshot.activation_code));
    strlcpy(s_snapshot.activation_message, json_string_or_empty(root, "activationMessage"), sizeof(s_snapshot.activation_message));

    if (s_snapshot.session_id[0] == '\0' && s_snapshot.session_requested) {
        strlcpy(s_snapshot.session_id, XIAOZHI_BRIDGE_SESSION_ID_DEFAULT, sizeof(s_snapshot.session_id));
    }

    cJSON_Delete(root);
}

static void set_error_locked(const char *message)
{
    s_snapshot.connected = false;
    s_snapshot.state = XIAOZHI_CLIENT_STATE_ERROR;
    strlcpy(s_snapshot.last_error, message != NULL ? message : "bridge error", sizeof(s_snapshot.last_error));
}

static void load_local_config_locked(const ornament_settings_t *settings)
{
    s_snapshot.enabled = true;
    s_snapshot.protocol_version = CONFIG_ORNAMENT_XIAOZHI_PROTOCOL_VERSION;
    strlcpy(s_snapshot.ws_url, settings_xiaozhi_ws_url_or_default(settings), sizeof(s_snapshot.ws_url));
    strlcpy(s_snapshot.saved_ws_url, settings_xiaozhi_ws_url_or_default(settings), sizeof(s_snapshot.saved_ws_url));
    strlcpy(s_snapshot.runtime_ws_url, settings_xiaozhi_ws_url_or_default(settings), sizeof(s_snapshot.runtime_ws_url));
    strlcpy(s_snapshot.active_ws_url, settings_xiaozhi_ws_url_or_default(settings), sizeof(s_snapshot.active_ws_url));
    if (s_snapshot.client_id[0] == '\0') {
        strlcpy(s_snapshot.client_id, device_identity_hostname(), sizeof(s_snapshot.client_id));
    }
}

static esp_err_t refresh_status_with_settings(const ornament_settings_t *settings)
{
    char url[XIAOZHI_BRIDGE_URL_MAX] = {0};
    ESP_RETURN_ON_ERROR(
        build_bridge_endpoint(settings, "/v1/xiaozhi/session/status", url, sizeof(url)),
        TAG,
        "build status endpoint");

    char *response = calloc(1, XIAOZHI_BRIDGE_RESPONSE_MAX);
    if (response == NULL) {
        return ESP_ERR_NO_MEM;
    }

    int http_status = -1;
    esp_err_t err = bridge_request(url, HTTP_METHOD_GET, NULL, response, XIAOZHI_BRIDGE_RESPONSE_MAX, &http_status);
    if (err == ESP_OK && (http_status < 200 || http_status >= 300)) {
        err = ESP_FAIL;
    }

    if (s_mutex != NULL && xSemaphoreTake(s_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        if (err == ESP_OK) {
            apply_status_json_locked(response);
        } else {
            char detail[XIAOZHI_STATUS_TEXT_MAX] = {0};
            snprintf(detail, sizeof(detail), "bridge status failed status=%d", http_status);
            set_error_locked(detail);
        }
        xSemaphoreGive(s_mutex);
    }

    free(response);
    return err;
}

static esp_err_t post_session_command(const char *path, bool request_session)
{
    ESP_RETURN_ON_ERROR(xiaozhi_client_init(), TAG, "init failed");

    ornament_settings_t settings;
    ESP_RETURN_ON_ERROR(settings_load(&settings), TAG, "settings load failed");

    char url[XIAOZHI_BRIDGE_URL_MAX] = {0};
    ESP_RETURN_ON_ERROR(build_bridge_endpoint(&settings, path, url, sizeof(url)), TAG, "build command endpoint");

    char body[160] = {0};
    snprintf(
        body,
        sizeof(body),
        "{\"clientId\":\"%s\",\"sampleRate\":16000,\"channels\":1,\"format\":\"pcm_s16le\"}",
        device_identity_hostname());

    char *response = calloc(1, XIAOZHI_BRIDGE_RESPONSE_MAX);
    if (response == NULL) {
        return ESP_ERR_NO_MEM;
    }

    int http_status = -1;
    esp_err_t err = bridge_request(url, HTTP_METHOD_POST, body, response, XIAOZHI_BRIDGE_RESPONSE_MAX, &http_status);
    if (err == ESP_OK && (http_status < 200 || http_status >= 300)) {
        err = ESP_FAIL;
    }

    if (s_mutex != NULL && xSemaphoreTake(s_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        load_local_config_locked(&settings);
        if (err == ESP_OK) {
            apply_status_json_locked(response);
            s_snapshot.session_requested = request_session;
            if (request_session && s_snapshot.state == XIAOZHI_CLIENT_STATE_IDLE) {
                s_snapshot.state = XIAOZHI_CLIENT_STATE_CONNECTING;
            }
        } else {
            s_snapshot.session_requested = false;
            char detail[XIAOZHI_STATUS_TEXT_MAX] = {0};
            snprintf(detail, sizeof(detail), "bridge command failed status=%d", http_status);
            set_error_locked(detail);
        }
        xSemaphoreGive(s_mutex);
    }

    free(response);
    return err;
}

const char *xiaozhi_client_state_name(xiaozhi_client_state_t state)
{
    switch (state) {
    case XIAOZHI_CLIENT_STATE_DISABLED:
        return "disabled";
    case XIAOZHI_CLIENT_STATE_IDLE:
        return "idle";
    case XIAOZHI_CLIENT_STATE_CONFIG_MISSING:
        return "config_missing";
    case XIAOZHI_CLIENT_STATE_CONNECTING:
        return "connecting";
    case XIAOZHI_CLIENT_STATE_LISTENING:
        return "listening";
    case XIAOZHI_CLIENT_STATE_SPEAKING:
        return "speaking";
    case XIAOZHI_CLIENT_STATE_ERROR:
    default:
        return "error";
    }
}

esp_err_t xiaozhi_client_init(void)
{
    if (s_initialized) {
        ornament_settings_t settings;
        if (settings_load(&settings) == ESP_OK && s_mutex != NULL &&
            xSemaphoreTake(s_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            load_local_config_locked(&settings);
            xSemaphoreGive(s_mutex);
        }
        return ESP_OK;
    }

    s_mutex = xSemaphoreCreateMutex();
    if (s_mutex == NULL) {
        return ESP_ERR_NO_MEM;
    }

    memset(&s_snapshot, 0, sizeof(s_snapshot));
    s_snapshot.enabled = true;
    s_snapshot.configured = true;
    s_snapshot.state = XIAOZHI_CLIENT_STATE_IDLE;
    s_snapshot.protocol_version = CONFIG_ORNAMENT_XIAOZHI_PROTOCOL_VERSION;
    strlcpy(s_snapshot.session_id, XIAOZHI_BRIDGE_SESSION_ID_DEFAULT, sizeof(s_snapshot.session_id));

    ornament_settings_t settings;
    if (settings_load(&settings) == ESP_OK) {
        load_local_config_locked(&settings);
    }

    s_initialized = true;
    ESP_LOGI(TAG, "Xiaozhi bridge client ready timeout=%d", CONFIG_ORNAMENT_XIAOZHI_BRIDGE_TIMEOUT_MS);
    return ESP_OK;
}

esp_err_t xiaozhi_client_probe(const char *ws_url_override, const char *token_override, xiaozhi_probe_result_t *result)
{
    (void)ws_url_override;
    (void)token_override;
    if (result == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(result, 0, sizeof(*result));
    result->http_status = -1;

    ESP_RETURN_ON_ERROR(xiaozhi_client_init(), TAG, "init failed");

    ornament_settings_t settings;
    esp_err_t err = settings_load(&settings);
    if (err != ESP_OK) {
        result->err = err;
        strlcpy(result->detail, "settings load failed", sizeof(result->detail));
        return err;
    }

    char url[XIAOZHI_BRIDGE_URL_MAX] = {0};
    err = build_bridge_endpoint(&settings, "/v1/xiaozhi/session/status", url, sizeof(url));
    if (err != ESP_OK) {
        result->err = err;
        strlcpy(result->detail, "bridge url invalid", sizeof(result->detail));
        return err;
    }

    char *response = calloc(1, XIAOZHI_BRIDGE_RESPONSE_MAX);
    if (response == NULL) {
        result->err = ESP_ERR_NO_MEM;
        strlcpy(result->detail, "response alloc failed", sizeof(result->detail));
        return ESP_ERR_NO_MEM;
    }

    int http_status = -1;
    err = bridge_request(url, HTTP_METHOD_GET, NULL, response, XIAOZHI_BRIDGE_RESPONSE_MAX, &http_status);
    result->http_status = http_status;
    result->configured = err == ESP_OK && http_status >= 200 && http_status < 300;
    result->websocket_connected = result->configured;
    result->hello_received = result->configured;
    result->err = result->configured ? ESP_OK : err;
    strlcpy(result->session_id, XIAOZHI_BRIDGE_SESSION_ID_DEFAULT, sizeof(result->session_id));
    strlcpy(
        result->detail,
        result->configured ? "bridge reachable" : "bridge status failed",
        sizeof(result->detail));
    if (result->configured) {
        (void)refresh_status_with_settings(&settings);
    }

    free(response);
    return result->err;
}

esp_err_t xiaozhi_client_start_session(void)
{
    return post_session_command("/v1/xiaozhi/session/start", true);
}

esp_err_t xiaozhi_client_reconnect_session(bool start_if_idle)
{
    ESP_RETURN_ON_ERROR(xiaozhi_client_init(), TAG, "init failed");
    (void)xiaozhi_client_stop_session();
    return start_if_idle ? xiaozhi_client_start_session() : ESP_OK;
}

esp_err_t xiaozhi_client_stop_session(void)
{
    return post_session_command("/v1/xiaozhi/session/stop", false);
}

void xiaozhi_client_status_snapshot(xiaozhi_client_snapshot_t *snapshot)
{
    if (snapshot == NULL) {
        return;
    }
    memset(snapshot, 0, sizeof(*snapshot));
    snapshot->state = XIAOZHI_CLIENT_STATE_DISABLED;

    if (xiaozhi_client_init() != ESP_OK || s_mutex == NULL) {
        return;
    }

    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
        *snapshot = s_snapshot;
        xSemaphoreGive(s_mutex);
    }

    (void)state_to_bridge_text(snapshot->state);
}

bool xiaozhi_client_session_requested(void)
{
    if (s_mutex == NULL) {
        return false;
    }
    bool requested = false;
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
        requested = s_snapshot.session_requested;
        xSemaphoreGive(s_mutex);
    }
    return requested;
}

#elif !CONFIG_ORNAMENT_XIAOZHI_ENABLED

const char *xiaozhi_client_state_name(xiaozhi_client_state_t state)
{
    (void)state;
    return "disabled";
}

esp_err_t xiaozhi_client_init(void)
{
    return ESP_OK;
}

esp_err_t xiaozhi_client_start_session(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t xiaozhi_client_reconnect_session(bool start_if_idle)
{
    (void)start_if_idle;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t xiaozhi_client_stop_session(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t xiaozhi_client_probe(const char *ws_url_override, const char *token_override, xiaozhi_probe_result_t *result)
{
    (void)ws_url_override;
    (void)token_override;
    if (result != NULL) {
        memset(result, 0, sizeof(*result));
        result->err = ESP_ERR_NOT_SUPPORTED;
        result->http_status = -1;
        strlcpy(result->detail, "disabled", sizeof(result->detail));
    }
    return ESP_ERR_NOT_SUPPORTED;
}

void xiaozhi_client_status_snapshot(xiaozhi_client_snapshot_t *snapshot)
{
    if (snapshot != NULL) {
        memset(snapshot, 0, sizeof(*snapshot));
        snapshot->state = XIAOZHI_CLIENT_STATE_DISABLED;
    }
}

bool xiaozhi_client_session_requested(void)
{
    return false;
}

#endif
