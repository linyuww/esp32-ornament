#include "bridge_client.h"

#include "cJSON.h"
#include "esp_check.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "settings.h"

#include <stdlib.h>
#include <string.h>

static const char *TAG = "bridge_client";
static const int MAX_RESPONSE_BYTES = 8192;

typedef struct {
    char *data;
    int length;
    int capacity;
} response_buffer_t;

static esp_err_t http_event_handler(esp_http_client_event_t *event)
{
    response_buffer_t *buffer = (response_buffer_t *)event->user_data;

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

static esp_err_t parse_state_json(const char *json_text, ornament_state_t *state)
{
    cJSON *root = cJSON_Parse(json_text);
    if (root == NULL) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    ornament_state_init(state);
    copy_json_string(root, "status", state->quota_status, sizeof(state->quota_status));
    state->status = ornament_status_from_text(state->quota_status);

    cJSON *task = cJSON_GetObjectItemCaseSensitive(root, "task");
    if (cJSON_IsObject(task)) {
        state->has_task = true;
        copy_json_string(task, "title", state->task_title, sizeof(state->task_title));
        copy_json_string(task, "message", state->task_message, sizeof(state->task_message));
        copy_json_string(task, "receivedAt", state->task_received_at, sizeof(state->task_received_at));
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

    cJSON_Delete(root);
    return ESP_OK;
}

static esp_err_t fetch_url_raw(const char *url, char *response, int response_capacity, int timeout_ms, int *status_code, int *response_len)
{
    if (url == NULL || url[0] == '\0' || response == NULL || response_capacity <= 0) {
        return ESP_ERR_INVALID_ARG;
    }

    response_buffer_t buffer = {
        .data = response,
        .length = 0,
        .capacity = response_capacity,
    };
    esp_http_client_config_t config = {
        .url = url,
        .event_handler = http_event_handler,
        .user_data = &buffer,
        .timeout_ms = timeout_ms,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        return ESP_FAIL;
    }

    esp_err_t err = esp_http_client_perform(client);
    if (status_code != NULL) {
        *status_code = esp_http_client_get_status_code(client);
    }
    if (response_len != NULL) {
        *response_len = buffer.length;
    }
    esp_http_client_cleanup(client);
    return err;
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
        2500,
        &status_code,
        &response_len);
    (void)response_len;

    if (err == ESP_OK && status_code == 200) {
        err = parse_state_json(response, state);
    } else if (err == ESP_OK) {
        err = ESP_ERR_HTTP_BASE + status_code;
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

    int status_code = 0;
    int response_len = 0;
    esp_err_t err = fetch_url_raw(url, response, MAX_RESPONSE_BYTES, 2500, &status_code, &response_len);
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
