#include "xiaozhi_client.h"

#include "cJSON.h"
#include "device_identity.h"
#include "esp_check.h"
#include "esp_crt_bundle.h"
#include "esp_event.h"
#include "esp_http_client.h"
#include "esp_idf_version.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_websocket_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/idf_additions.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"
#include "music_player.h"
#include "task_audio.h"
#include "xiaozhi_mcp.h"

#include <opus.h>

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if CONFIG_ORNAMENT_XIAOZHI_ENABLED

#define XIAOZHI_EVENT_CONNECTED BIT0
#define XIAOZHI_EVENT_HELLO BIT1
#define XIAOZHI_EVENT_STOP BIT2
#define XIAOZHI_EVENT_SPEAKING BIT3
#define XIAOZHI_EVENT_ERROR BIT4
#define XIAOZHI_EVENT_LISTEN_RESTART BIT5
#define XIAOZHI_EVENT_CAPTURE_DONE BIT6
#define XIAOZHI_JSON_MAX 640
#define XIAOZHI_HEADER_MAX 512
#define XIAOZHI_HTTP_RESPONSE_MAX 4096
#define XIAOZHI_OPUS_SAMPLE_RATE_HZ ORNAMENT_AUDIO_SAMPLE_RATE_HZ
#define XIAOZHI_OPUS_CHANNELS 1
#define XIAOZHI_TASK_STACK_LARGE 12288
#define XIAOZHI_TASK_STACK_MEDIUM 10240
#define XIAOZHI_TASK_STACK_SMALL 8192
#define XIAOZHI_TASK_STACK_MIN 6144
#define XIAOZHI_CAPTURE_TASK_STACK 16384
#define XIAOZHI_TASK_PRIO 5
#define XIAOZHI_RECONNECT_DELAY_MS 2000
#define XIAOZHI_OPUS_DECODE_MAX_FRAMES (ORNAMENT_AUDIO_SAMPLE_RATE_HZ * 60 / 1000)
#define XIAOZHI_WS_TASK_STACK 8192
#define XIAOZHI_DEFAULT_OTA_URL "https://api.tenclass.net/xiaozhi/ota/"

static const char *TAG = "xiaozhi";
static const char *XIAOZHI_SETTINGS_NAMESPACE = "xiaozhi";
static const char *XIAOZHI_UUID_KEY = "client_id";

static SemaphoreHandle_t s_mutex;
static EventGroupHandle_t s_events;
static SemaphoreHandle_t s_codec_mutex;
static bool s_tts_output_active;
static TaskHandle_t s_session_task;
static TaskHandle_t s_capture_task;
static esp_websocket_client_handle_t s_client;
static xiaozhi_client_snapshot_t s_snapshot;
static bool s_session_starting;
static volatile esp_err_t s_capture_result;
static size_t s_session_task_stack_bytes = XIAOZHI_TASK_STACK_LARGE;
static char s_runtime_client_id[XIAOZHI_CLIENT_ID_MAX];
static char s_runtime_ws_url[ORNAMENT_XIAOZHI_WS_URL_MAX];
static char s_runtime_token[ORNAMENT_XIAOZHI_TOKEN_MAX];
static int s_runtime_protocol_version = CONFIG_ORNAMENT_XIAOZHI_PROTOCOL_VERSION;

static void release_tts_output_if_active(void);
static void get_device_mac(char *target, size_t target_size);
static void set_error_with_http_status(const char *message, int http_status);
static void format_http_error_detail(
    char *target,
    size_t target_size,
    const char *prefix,
    esp_err_t err,
    int http_status,
    const char *response);
static esp_err_t send_mcp_response_on_client(esp_websocket_client_handle_t client, const char *response_json);

typedef struct {
    ornament_settings_t settings;
    char headers[XIAOZHI_HEADER_MAX];
} xiaozhi_session_context_t;

typedef struct {
    OpusEncoder *encoder;
    esp_websocket_client_handle_t client;
} xiaozhi_capture_context_t;

typedef struct {
    char *data;
    int length;
    int capacity;
} http_response_buffer_t;

static uint32_t current_stack_high_water_bytes(void)
{
    return uxTaskGetStackHighWaterMark(NULL) * sizeof(StackType_t);
}

typedef struct {
    EventGroupHandle_t events;
    xiaozhi_probe_result_t *result;
    esp_websocket_client_handle_t client;
} xiaozhi_probe_context_t;

static esp_err_t http_event_handler(esp_http_client_event_t *event)
{
    http_response_buffer_t *buffer = (http_response_buffer_t *)event->user_data;
    if (event->event_id != HTTP_EVENT_ON_DATA || buffer == NULL || event->data == NULL) {
        return ESP_OK;
    }
    if (buffer->length + event->data_len >= buffer->capacity) {
        ESP_LOGW(TAG, "OTA response too large");
        return ESP_FAIL;
    }
    memcpy(buffer->data + buffer->length, event->data, event->data_len);
    buffer->length += event->data_len;
    buffer->data[buffer->length] = '\0';
    return ESP_OK;
}

static void log_heap_status(const char *stage)
{
    ESP_LOGI(
        TAG,
        "heap %s: free=%u min=%u largest8=%u largest_internal=%u internal=%u spiram=%u",
        stage,
        (unsigned int)esp_get_free_heap_size(),
        (unsigned int)esp_get_minimum_free_heap_size(),
        (unsigned int)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT),
        (unsigned int)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
        (unsigned int)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
        (unsigned int)heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
}

static size_t select_session_task_stack_bytes(void)
{
    static const size_t candidates[] = {
        XIAOZHI_TASK_STACK_LARGE,
        XIAOZHI_TASK_STACK_MEDIUM,
        XIAOZHI_TASK_STACK_SMALL,
        XIAOZHI_TASK_STACK_MIN,
    };

    size_t largest_internal = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    const size_t reserve_for_ws_task = XIAOZHI_WS_TASK_STACK + 2048;
    for (size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); i++) {
        size_t stack_bytes = candidates[i];
        if (largest_internal >= stack_bytes + reserve_for_ws_task) {
            return stack_bytes;
        }
    }

    return XIAOZHI_TASK_STACK_MIN;
}

static void format_http_error_detail(
    char *target,
    size_t target_size,
    const char *prefix,
    esp_err_t err,
    int http_status,
    const char *response)
{
    if (target == NULL || target_size == 0) {
        return;
    }

    const char *err_name = esp_err_to_name(err);
    if (response != NULL && response[0] != '\0') {
        snprintf(
            target,
            target_size,
            "%s: %s status=%d body=%.96s",
            prefix != NULL ? prefix : "http error",
            err_name,
            http_status,
            response);
        return;
    }

    snprintf(
        target,
        target_size,
        "%s: %s status=%d",
        prefix != NULL ? prefix : "http error",
        err_name,
        http_status);
}

static const char *ota_url(void)
{
    return XIAOZHI_DEFAULT_OTA_URL;
}

static bool is_official_backend_url(const char *ws_url)
{
    return ws_url != NULL && strstr(ws_url, "api.tenclass.net") != NULL;
}

static bool should_fetch_official_runtime_config(const ornament_settings_t *settings)
{
    const char *ws_url = settings_xiaozhi_ws_url_or_default(settings);
    if (!is_official_backend_url(ws_url)) {
        return false;
    }

    return strstr(ws_url, "/xiaozhi/v1/") != NULL;
}

static void generate_uuid_v4(char *target, size_t target_size)
{
    uint8_t uuid[16] = {0};
    esp_fill_random(uuid, sizeof(uuid));
    uuid[6] = (uint8_t)((uuid[6] & 0x0F) | 0x40);
    uuid[8] = (uint8_t)((uuid[8] & 0x3F) | 0x80);
    snprintf(
        target,
        target_size,
        "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
        uuid[0],
        uuid[1],
        uuid[2],
        uuid[3],
        uuid[4],
        uuid[5],
        uuid[6],
        uuid[7],
        uuid[8],
        uuid[9],
        uuid[10],
        uuid[11],
        uuid[12],
        uuid[13],
        uuid[14],
        uuid[15]);
}

static esp_err_t load_or_create_client_id(char *target, size_t target_size)
{
    if (target == NULL || target_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (CONFIG_ORNAMENT_XIAOZHI_CLIENT_ID[0] != '\0') {
        strlcpy(target, CONFIG_ORNAMENT_XIAOZHI_CLIENT_ID, target_size);
        return ESP_OK;
    }

    nvs_handle_t handle;
    esp_err_t err = nvs_open(XIAOZHI_SETTINGS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }

    size_t required = target_size;
    err = nvs_get_str(handle, XIAOZHI_UUID_KEY, target, &required);
    if (err == ESP_OK && target[0] != '\0') {
        nvs_close(handle);
        return ESP_OK;
    }

    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
        nvs_close(handle);
        return err;
    }

    generate_uuid_v4(target, target_size);
    err = nvs_set_str(handle, XIAOZHI_UUID_KEY, target);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}

static void store_text_field_or_clear(const cJSON *root, const char *name, char *target, size_t target_size)
{
    if (target == NULL || target_size == 0) {
        return;
    }
    target[0] = '\0';
    const cJSON *value = cJSON_GetObjectItem(root, name);
    if (cJSON_IsString(value) && value->valuestring != NULL) {
        strlcpy(target, value->valuestring, target_size);
    }
}

static esp_err_t build_ota_request_body(
    const ornament_settings_t *settings,
    const char *client_id,
    char *body,
    size_t body_size)
{
    (void)settings;
    if (client_id == NULL || body == NULL || body_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    char mac[24] = {0};
    get_device_mac(mac, sizeof(mac));

    int written = snprintf(
        body,
        body_size,
        "{"
        "\"version\":2,"
        "\"language\":\"zh-CN\","
        "\"flash_size\":16777216,"
        "\"minimum_free_heap_size\":%u,"
        "\"mac_address\":\"%s\","
        "\"uuid\":\"%s\","
        "\"chip_model_name\":\"" CONFIG_IDF_TARGET "\","
        "\"chip_info\":{\"model\":9,\"cores\":2,\"revision\":0,\"features\":0},"
        "\"application\":{\"name\":\"esp32-ornament\",\"version\":\"0.1.0\",\"compile_time\":\"%sT%sZ\",\"idf_version\":\"%s\",\"elf_sha256\":\"\"},"
        "\"partition_table\":[{\"label\":\"factory\",\"type\":0,\"subtype\":0,\"address\":65536,\"size\":4194304}],"
        "\"ota\":{\"label\":\"factory\"},"
        "\"display\":{\"monochrome\":false,\"width\":%d,\"height\":%d},"
        "\"board\":{\"type\":\"esp32-ornament\"}"
        "}",
        (unsigned int)esp_get_minimum_free_heap_size(),
        mac,
        client_id,
        __DATE__,
        __TIME__,
        esp_get_idf_version(),
        CONFIG_ORNAMENT_LCD_H_RES,
        CONFIG_ORNAMENT_LCD_V_RES);
    return written > 0 && written < (int)body_size ? ESP_OK : ESP_ERR_NO_MEM;
}

static void reset_runtime_config_locked(const ornament_settings_t *settings)
{
    strlcpy(s_runtime_ws_url, settings_xiaozhi_ws_url_or_default(settings), sizeof(s_runtime_ws_url));
    strlcpy(s_runtime_token, settings_xiaozhi_token_or_default(settings), sizeof(s_runtime_token));
    s_runtime_protocol_version = CONFIG_ORNAMENT_XIAOZHI_PROTOCOL_VERSION;
    s_snapshot.protocol_version = s_runtime_protocol_version;
    s_snapshot.activation_pending = false;
    s_snapshot.activation_code[0] = '\0';
    s_snapshot.activation_message[0] = '\0';
}

static void apply_runtime_websocket_config_locked(const cJSON *websocket, const ornament_settings_t *settings)
{
    reset_runtime_config_locked(settings);
    if (!cJSON_IsObject(websocket)) {
        return;
    }

    const cJSON *url = cJSON_GetObjectItem(websocket, "url");
    const cJSON *token = cJSON_GetObjectItem(websocket, "token");
    const cJSON *version = cJSON_GetObjectItem(websocket, "version");
    if (cJSON_IsString(url) && url->valuestring != NULL) {
        strlcpy(s_runtime_ws_url, url->valuestring, sizeof(s_runtime_ws_url));
    }
    if (cJSON_IsString(token) && token->valuestring != NULL) {
        strlcpy(s_runtime_token, token->valuestring, sizeof(s_runtime_token));
    }
    if (cJSON_IsNumber(version)) {
        s_runtime_protocol_version = version->valueint;
    }
    s_snapshot.protocol_version = s_runtime_protocol_version;
}

static void apply_activation_status_locked(const cJSON *root)
{
    s_snapshot.activation_pending = false;
    s_snapshot.activation_code[0] = '\0';
    s_snapshot.activation_message[0] = '\0';

    const cJSON *activation = cJSON_GetObjectItem(root, "activation");
    if (!cJSON_IsObject(activation)) {
        return;
    }

    store_text_field_or_clear(activation, "code", s_snapshot.activation_code, sizeof(s_snapshot.activation_code));
    store_text_field_or_clear(activation, "message", s_snapshot.activation_message, sizeof(s_snapshot.activation_message));
    s_snapshot.activation_pending = s_snapshot.activation_code[0] != '\0';
}

static esp_err_t fetch_runtime_config(ornament_settings_t *settings, int *http_status, bool *activation_pending)
{
    if (settings == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    char *response = calloc(1, XIAOZHI_HTTP_RESPONSE_MAX);
    char *request_body = calloc(1, 2048);
    if (response == NULL || request_body == NULL) {
        free(response);
        free(request_body);
        return ESP_ERR_NO_MEM;
    }
    http_response_buffer_t buffer = {
        .data = response,
        .length = 0,
        .capacity = XIAOZHI_HTTP_RESPONSE_MAX,
    };

    esp_err_t err = build_ota_request_body(settings, s_runtime_client_id, request_body, 2048);
    if (err != ESP_OK) {
        free(response);
        free(request_body);
        return err;
    }

    esp_http_client_config_t config = {
        .url = ota_url(),
        .event_handler = http_event_handler,
        .user_data = &buffer,
        .timeout_ms = CONFIG_ORNAMENT_XIAOZHI_CONNECT_TIMEOUT_MS,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        free(response);
        free(request_body);
        return ESP_FAIL;
    }

    char mac[24] = {0};
    get_device_mac(mac, sizeof(mac));
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_http_client_set_method(client, HTTP_METHOD_POST));
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_http_client_set_header(client, "Activation-Version", "1"));
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_http_client_set_header(client, "Device-Id", mac));
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_http_client_set_header(client, "Client-Id", s_runtime_client_id));
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_http_client_set_header(client, "User-Agent", "esp32-ornament/0.1.0"));
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_http_client_set_header(client, "Accept-Language", "zh-CN"));
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_http_client_set_header(client, "Content-Type", "application/json"));
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_http_client_set_post_field(client, request_body, (int)strlen(request_body)));

    err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    if (http_status != NULL) {
        *http_status = status;
    }
    if (err != ESP_OK) {
        ESP_LOGW(
            TAG,
            "runtime config fetch failed: err=%s status=%d response=%.96s",
            esp_err_to_name(err),
            status,
            response);
        esp_http_client_cleanup(client);
        free(response);
        free(request_body);
        return err;
    }
    if (status != 200) {
        ESP_LOGW(TAG, "runtime config status=%d body=%.96s", status, response);
        esp_http_client_cleanup(client);
        free(response);
        free(request_body);
        return ESP_FAIL;
    }

    cJSON *root = cJSON_Parse(response);
    esp_http_client_cleanup(client);
    free(request_body);
    if (root == NULL) {
        free(response);
        return ESP_ERR_INVALID_RESPONSE;
    }

    if (s_mutex != NULL && xSemaphoreTake(s_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        apply_runtime_websocket_config_locked(cJSON_GetObjectItem(root, "websocket"), settings);
        apply_activation_status_locked(root);
        xSemaphoreGive(s_mutex);
    }

    if (activation_pending != NULL) {
        *activation_pending = false;
        if (s_mutex != NULL && xSemaphoreTake(s_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            *activation_pending = s_snapshot.activation_pending;
            xSemaphoreGive(s_mutex);
        }
    }

    cJSON_Delete(root);
    free(response);
    return ESP_OK;
}

static esp_err_t activate_runtime_config(int *http_status)
{
    char response[256] = {0};
    http_response_buffer_t buffer = {
        .data = response,
        .length = 0,
        .capacity = sizeof(response),
    };

    esp_http_client_config_t config = {
        .url = "https://api.tenclass.net/xiaozhi/ota/activate",
        .event_handler = http_event_handler,
        .user_data = &buffer,
        .timeout_ms = CONFIG_ORNAMENT_XIAOZHI_CONNECT_TIMEOUT_MS,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        return ESP_FAIL;
    }

    char mac[24] = {0};
    get_device_mac(mac, sizeof(mac));
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_http_client_set_method(client, HTTP_METHOD_POST));
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_http_client_set_header(client, "Activation-Version", "1"));
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_http_client_set_header(client, "Device-Id", mac));
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_http_client_set_header(client, "Client-Id", s_runtime_client_id));
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_http_client_set_header(client, "User-Agent", "esp32-ornament/0.1.0"));
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_http_client_set_header(client, "Accept-Language", "zh-CN"));
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_http_client_set_header(client, "Content-Type", "application/json"));
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_http_client_set_post_field(client, "{}", 2));

    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    if (http_status != NULL) {
        *http_status = status;
    }
    esp_http_client_cleanup(client);
    if (err != ESP_OK) {
        return err;
    }
    if (status == 200) {
        return ESP_OK;
    }
    if (status == 202) {
        return ESP_ERR_TIMEOUT;
    }
    return ESP_FAIL;
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

static void set_state_locked(xiaozhi_client_state_t state)
{
    s_snapshot.state = state;
    s_snapshot.connected = state == XIAOZHI_CLIENT_STATE_LISTENING ||
                           state == XIAOZHI_CLIENT_STATE_SPEAKING ||
                           state == XIAOZHI_CLIENT_STATE_CONNECTING;
}

static void set_session_requested_locked(bool requested)
{
    s_snapshot.session_requested = requested;
}

static bool session_runtime_active_locked(void)
{
    return s_session_task != NULL || s_session_starting || s_client != NULL || s_capture_task != NULL;
}

static bool session_requested_enabled(void)
{
    if (s_mutex == NULL) {
        return false;
    }

    bool requested = false;
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        requested = s_snapshot.session_requested;
        xSemaphoreGive(s_mutex);
    }
    return requested;
}

static esp_err_t wait_for_session_runtime_inactive(TickType_t timeout_ticks)
{
    if (s_mutex == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    TickType_t poll_ticks = pdMS_TO_TICKS(100);
    if (poll_ticks == 0) {
        poll_ticks = 1;
    }
    uint32_t max_attempts = (uint32_t)(timeout_ticks / poll_ticks);
    if (max_attempts == 0) {
        max_attempts = 1;
    }

    for (uint32_t attempt = 0; attempt <= max_attempts; attempt++) {
        bool runtime_active = false;
        if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
            return ESP_ERR_TIMEOUT;
        }
        runtime_active = session_runtime_active_locked();
        xSemaphoreGive(s_mutex);
        if (!runtime_active) {
            return ESP_OK;
        }
        if (attempt < max_attempts) {
            vTaskDelay(poll_ticks);
        }
    }

    return ESP_ERR_TIMEOUT;
}

static void set_state(xiaozhi_client_state_t state)
{
    if (s_mutex == NULL) {
        return;
    }
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        set_state_locked(state);
        xSemaphoreGive(s_mutex);
    }
}

static void set_error(const char *message)
{
    set_error_with_http_status(message, -1);
}

static void set_error_with_http_status(const char *message, int http_status)
{
    if (s_mutex == NULL) {
        return;
    }
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        set_state_locked(XIAOZHI_CLIENT_STATE_ERROR);
        if (http_status > 0 && message != NULL && strstr(message, "hello timeout") != NULL) {
            snprintf(s_snapshot.last_error, sizeof(s_snapshot.last_error), "%s (%d)", message, http_status);
        } else if (http_status > 0 && message != NULL && strstr(message, "websocket error") != NULL) {
            snprintf(s_snapshot.last_error, sizeof(s_snapshot.last_error), "%s (%d)", message, http_status);
        } else {
            strlcpy(s_snapshot.last_error, message != NULL ? message : "unknown", sizeof(s_snapshot.last_error));
        }
        xSemaphoreGive(s_mutex);
    }
}

static esp_err_t sync_runtime_config_from_settings(const ornament_settings_t *settings)
{
    if (s_mutex == NULL || settings == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    reset_runtime_config_locked(settings);
    xSemaphoreGive(s_mutex);
    return ESP_OK;
}

static void copy_settings_to_snapshot(const ornament_settings_t *settings)
{
    if (s_mutex == NULL || settings == NULL) {
        return;
    }
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        s_snapshot.enabled = true;
        s_snapshot.official_runtime_config = should_fetch_official_runtime_config(settings);
        strlcpy(s_snapshot.saved_ws_url, settings_xiaozhi_ws_url_or_default(settings), sizeof(s_snapshot.saved_ws_url));
        strlcpy(s_snapshot.runtime_ws_url, s_runtime_ws_url, sizeof(s_snapshot.runtime_ws_url));
        if (s_runtime_ws_url[0] != '\0') {
            strlcpy(s_snapshot.ws_url, s_runtime_ws_url, sizeof(s_snapshot.ws_url));
        } else {
            strlcpy(s_snapshot.ws_url, settings_xiaozhi_ws_url_or_default(settings), sizeof(s_snapshot.ws_url));
        }
        s_snapshot.protocol_version = s_runtime_protocol_version;
        s_snapshot.configured = s_snapshot.ws_url[0] != '\0' && CONFIG_ORNAMENT_XIAOZHI_MIC_PIN_DIN >= 0;
        if (!s_snapshot.configured) {
            set_state_locked(XIAOZHI_CLIENT_STATE_CONFIG_MISSING);
            strlcpy(
                s_snapshot.last_error,
                s_snapshot.ws_url[0] == '\0' ? "missing websocket url" : "missing mic gpio",
                sizeof(s_snapshot.last_error));
        } else if (s_snapshot.state == XIAOZHI_CLIENT_STATE_CONFIG_MISSING) {
            set_state_locked(XIAOZHI_CLIENT_STATE_IDLE);
            s_snapshot.last_error[0] = '\0';
        } else {
            s_snapshot.last_error[0] = '\0';
        }
        xSemaphoreGive(s_mutex);
    }
}

static bool is_supported_frame_ms(int frame_ms)
{
    return frame_ms == 20 || frame_ms == 40 || frame_ms == 60;
}

void xiaozhi_client_status_snapshot(xiaozhi_client_snapshot_t *snapshot)
{
    if (snapshot == NULL) {
        return;
    }
    memset(snapshot, 0, sizeof(*snapshot));
    if (s_mutex == NULL) {
        snapshot->state = XIAOZHI_CLIENT_STATE_DISABLED;
        return;
    }
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        *snapshot = s_snapshot;
        xSemaphoreGive(s_mutex);
    } else {
        snapshot->state = XIAOZHI_CLIENT_STATE_ERROR;
        strlcpy(snapshot->last_error, "snapshot timeout", sizeof(snapshot->last_error));
    }
}

bool xiaozhi_client_session_requested(void)
{
    if (s_mutex == NULL) {
        return false;
    }

    bool requested = false;
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        requested = s_snapshot.session_requested;
        xSemaphoreGive(s_mutex);
    }
    return requested;
}

static void get_device_mac(char *target, size_t target_size)
{
    uint8_t mac[6] = {0};
    if (esp_read_mac(mac, ESP_MAC_WIFI_STA) == ESP_OK) {
        snprintf(
            target,
            target_size,
            "%02x:%02x:%02x:%02x:%02x:%02x",
            mac[0],
            mac[1],
            mac[2],
            mac[3],
            mac[4],
            mac[5]);
    } else {
        strlcpy(target, device_identity_hostname(), target_size);
    }
}

static void get_client_id(char *target, size_t target_size)
{
    if (CONFIG_ORNAMENT_XIAOZHI_CLIENT_ID[0] != '\0') {
        strlcpy(target, CONFIG_ORNAMENT_XIAOZHI_CLIENT_ID, target_size);
    } else if (s_runtime_client_id[0] != '\0') {
        strlcpy(target, s_runtime_client_id, target_size);
    } else {
        strlcpy(target, device_identity_hostname(), target_size);
    }
}

static esp_err_t send_text_frame_on_client(esp_websocket_client_handle_t client, const char *json)
{
    if (client == NULL || json == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    int len = (int)strlen(json);
    int sent = esp_websocket_client_send_text(client, json, len, pdMS_TO_TICKS(2000));
    return sent == len ? ESP_OK : ESP_FAIL;
}

static esp_err_t send_mcp_response_on_client(esp_websocket_client_handle_t client, const char *response_json)
{
    if (client == NULL || response_json == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    char envelope[XIAOZHI_JSON_MAX * 2];
    int written = snprintf(
        envelope,
        sizeof(envelope),
        "{\"type\":\"mcp\",\"payload\":%s}",
        response_json);
    if (written <= 0 || written >= (int)sizeof(envelope)) {
        return ESP_ERR_NO_MEM;
    }
    return send_text_frame_on_client(client, envelope);
}

static esp_err_t send_hello_on_client(esp_websocket_client_handle_t client)
{
    char json[XIAOZHI_JSON_MAX];
    int written = snprintf(
        json,
        sizeof(json),
        "{\"type\":\"hello\",\"version\":%d,\"transport\":\"websocket\","
        "\"audio_params\":{\"format\":\"opus\",\"sample_rate\":%d,\"channels\":1,\"frame_duration\":%d},"
        "\"features\":{\"mcp\":%s}}",
        s_runtime_protocol_version,
        XIAOZHI_OPUS_SAMPLE_RATE_HZ,
        CONFIG_ORNAMENT_XIAOZHI_FRAME_MS,
        CONFIG_ORNAMENT_XIAOZHI_MCP_ENABLED ? "true" : "false");
    if (written <= 0 || written >= (int)sizeof(json)) {
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "sending hello");
    return send_text_frame_on_client(client, json);
}

static esp_err_t send_hello(void)
{
    return send_hello_on_client(s_client);
}

static esp_err_t send_listen_state_on_client(esp_websocket_client_handle_t client, const char *state)
{
    char session_id[XIAOZHI_SESSION_ID_MAX] = {0};
    if (s_mutex != NULL && xSemaphoreTake(s_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        strlcpy(session_id, s_snapshot.session_id, sizeof(session_id));
        xSemaphoreGive(s_mutex);
    }

    char json[XIAOZHI_JSON_MAX];
    int written = snprintf(
        json,
        sizeof(json),
        /* The Xiaozhi websocket protocol uses auto mode for open-mic, start-then-talk sessions. */
        "{\"session_id\":\"%s\",\"type\":\"listen\",\"state\":\"%s\",\"mode\":\"auto\"}",
        session_id,
        state);
    if (written <= 0 || written >= (int)sizeof(json)) {
        return ESP_ERR_NO_MEM;
    }
    return send_text_frame_on_client(client, json);
}

static void store_text_field(const cJSON *root, const char *name, char *target, size_t target_size)
{
    const cJSON *value = cJSON_GetObjectItem(root, name);
    if (cJSON_IsString(value) && value->valuestring != NULL) {
        strlcpy(target, value->valuestring, target_size);
    }
}

static void handle_text_message(const char *data, int len)
{
    char *json = calloc(1, (size_t)len + 1);
    if (json == NULL) {
        set_error("json alloc failed");
        return;
    }
    memcpy(json, data, (size_t)len);

    cJSON *root = cJSON_Parse(json);
    if (root == NULL) {
        ESP_LOGW(TAG, "ignored malformed json: %.*s", len, data);
        free(json);
        return;
    }

    const cJSON *type = cJSON_GetObjectItem(root, "type");
    if (!cJSON_IsString(type) || type->valuestring == NULL) {
        cJSON_Delete(root);
        free(json);
        return;
    }

    if (strcmp(type->valuestring, "mcp") == 0) {
        const cJSON *payload = cJSON_GetObjectItem(root, "payload");
        char *response_json = NULL;
        esp_err_t err = xiaozhi_mcp_handle_request(payload, &response_json);
        if (err == ESP_OK && response_json != NULL) {
            esp_err_t send_err = send_mcp_response_on_client(s_client, response_json);
            if (send_err != ESP_OK) {
                ESP_LOGW(TAG, "mcp response send failed: %s", esp_err_to_name(send_err));
            }
        } else if (err != ESP_OK) {
            ESP_LOGW(TAG, "mcp request handling failed: %s", esp_err_to_name(err));
        }
        free(response_json);
    } else if (strcmp(type->valuestring, "hello") == 0) {
        const cJSON *transport = cJSON_GetObjectItem(root, "transport");
        if (cJSON_IsString(transport) && strcmp(transport->valuestring, "websocket") == 0) {
            if (s_mutex != NULL && xSemaphoreTake(s_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
                store_text_field(root, "session_id", s_snapshot.session_id, sizeof(s_snapshot.session_id));
                set_state_locked(XIAOZHI_CLIENT_STATE_LISTENING);
                s_snapshot.last_error[0] = '\0';
                xSemaphoreGive(s_mutex);
            }
            xEventGroupSetBits(s_events, XIAOZHI_EVENT_HELLO);
            ESP_LOGI(TAG, "server hello ok");
        }
    } else if (strcmp(type->valuestring, "stt") == 0) {
        if (s_mutex != NULL && xSemaphoreTake(s_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            store_text_field(root, "text", s_snapshot.last_stt, sizeof(s_snapshot.last_stt));
            xSemaphoreGive(s_mutex);
        }
    } else if (strcmp(type->valuestring, "tts") == 0) {
        const cJSON *state = cJSON_GetObjectItem(root, "state");
        if (cJSON_IsString(state) && state->valuestring != NULL) {
            if (strcmp(state->valuestring, "start") == 0) {
                music_player_request_stop();
                set_state(XIAOZHI_CLIENT_STATE_SPEAKING);
                xEventGroupSetBits(s_events, XIAOZHI_EVENT_SPEAKING);
            } else if (strcmp(state->valuestring, "stop") == 0) {
                release_tts_output_if_active();
                set_state(XIAOZHI_CLIENT_STATE_LISTENING);
                xEventGroupClearBits(s_events, XIAOZHI_EVENT_SPEAKING);
                xEventGroupSetBits(s_events, XIAOZHI_EVENT_LISTEN_RESTART);
            } else if (strcmp(state->valuestring, "sentence_start") == 0) {
                if (s_mutex != NULL && xSemaphoreTake(s_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
                    store_text_field(root, "text", s_snapshot.last_tts, sizeof(s_snapshot.last_tts));
                    xSemaphoreGive(s_mutex);
                }
            }
        }
    } else if (strcmp(type->valuestring, "alert") == 0) {
        if (s_mutex != NULL && xSemaphoreTake(s_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            store_text_field(root, "message", s_snapshot.last_error, sizeof(s_snapshot.last_error));
            xSemaphoreGive(s_mutex);
        }
    } else if (strcmp(type->valuestring, "system") == 0) {
        const cJSON *command = cJSON_GetObjectItem(root, "command");
        if (cJSON_IsString(command) && strcmp(command->valuestring, "reboot") == 0) {
            ESP_LOGW(TAG, "server requested reboot");
            esp_restart();
        }
    }

    cJSON_Delete(root);
    free(json);
}

static void handle_probe_text_message(xiaozhi_probe_context_t *probe, const char *data, int len)
{
    if (probe == NULL || probe->result == NULL || data == NULL || len <= 0) {
        return;
    }

    char *json = calloc(1, (size_t)len + 1);
    if (json == NULL) {
        probe->result->err = ESP_ERR_NO_MEM;
        strlcpy(probe->result->detail, "json alloc failed", sizeof(probe->result->detail));
        return;
    }
    memcpy(json, data, (size_t)len);

    cJSON *root = cJSON_Parse(json);
    if (root == NULL) {
        free(json);
        return;
    }

    const cJSON *type = cJSON_GetObjectItem(root, "type");
    if (cJSON_IsString(type) && type->valuestring != NULL) {
        if (strcmp(type->valuestring, "hello") == 0) {
            const cJSON *transport = cJSON_GetObjectItem(root, "transport");
            if (cJSON_IsString(transport) && strcmp(transport->valuestring, "websocket") == 0) {
                probe->result->hello_received = true;
                probe->result->err = ESP_OK;
                store_text_field(root, "session_id", probe->result->session_id, sizeof(probe->result->session_id));
                strlcpy(probe->result->detail, "hello ok", sizeof(probe->result->detail));
                xEventGroupSetBits(probe->events, XIAOZHI_EVENT_HELLO);
            }
        } else if (strcmp(type->valuestring, "alert") == 0) {
            store_text_field(root, "message", probe->result->detail, sizeof(probe->result->detail));
        }
    }

    cJSON_Delete(root);
    free(json);
}

static void increment_downlink_frames(void)
{
    if (s_mutex != NULL && xSemaphoreTake(s_mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
        s_snapshot.downlink_frames++;
        xSemaphoreGive(s_mutex);
    }
}

static void increment_uplink_frames(void)
{
    if (s_mutex != NULL && xSemaphoreTake(s_mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
        s_snapshot.uplink_frames++;
        xSemaphoreGive(s_mutex);
    }
}

static void release_tts_output_if_active(void)
{
    if (s_tts_output_active) {
        task_audio_output_release();
        s_tts_output_active = false;
    }
}

static void websocket_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    (void)handler_args;
    (void)base;
    esp_websocket_event_data_t *data = (esp_websocket_event_data_t *)event_data;
    switch (event_id) {
    case WEBSOCKET_EVENT_CONNECTED:
        ESP_LOGI(TAG, "websocket connected");
        set_state(XIAOZHI_CLIENT_STATE_CONNECTING);
        xEventGroupSetBits(s_events, XIAOZHI_EVENT_CONNECTED);
        if (send_hello() != ESP_OK) {
            set_error("hello send failed");
            xEventGroupSetBits(s_events, XIAOZHI_EVENT_ERROR);
        }
        break;
    case WEBSOCKET_EVENT_DATA:
        if (data->op_code == 0x2 && data->payload_offset == 0 && data->fin) {
            int16_t pcm[XIAOZHI_OPUS_DECODE_MAX_FRAMES];
            if (s_codec_mutex == NULL || xSemaphoreTake(s_codec_mutex, pdMS_TO_TICKS(200)) != pdTRUE) {
                ESP_LOGW(TAG, "opus decode skipped: codec mutex busy");
                break;
            }
            int decoded = opus_decode((OpusDecoder *)data->user_context, (const unsigned char *)data->data_ptr, data->data_len, pcm, XIAOZHI_OPUS_DECODE_MAX_FRAMES, 0);
            xSemaphoreGive(s_codec_mutex);
            if (decoded > 0 && (xEventGroupGetBits(s_events) & XIAOZHI_EVENT_SPEAKING)) {
                if (!s_tts_output_active) {
                    ornament_settings_t settings = {0};
                    esp_err_t acquire_err = settings_load(&settings) == ESP_OK ?
                        task_audio_output_acquire_with_volume(&settings) :
                        task_audio_output_acquire();
                    if (acquire_err == ESP_OK) {
                        s_tts_output_active = true;
                    } else {
                        ESP_LOGW(TAG, "tts output acquire failed: %s", esp_err_to_name(acquire_err));
                    }
                }
                if (s_tts_output_active) {
                    esp_err_t err = task_audio_output_write_mono(pcm, (size_t)decoded, 1000);
                    if (err == ESP_OK) {
                        increment_downlink_frames();
                    } else {
                        ESP_LOGW(TAG, "tts write failed: %s", esp_err_to_name(err));
                        release_tts_output_if_active();
                    }
                }
            }
        } else if (data->op_code == 0x1 || data->op_code == 0x0) {
            if (data->payload_offset == 0 && data->fin) {
                handle_text_message(data->data_ptr, data->data_len);
            } else {
                ESP_LOGW(TAG, "fragmented text message ignored len=%d offset=%d payload=%d", data->data_len, data->payload_offset, data->payload_len);
            }
        }
        break;
    case WEBSOCKET_EVENT_DISCONNECTED:
    case WEBSOCKET_EVENT_CLOSED:
        ESP_LOGI(TAG, "websocket disconnected");
        release_tts_output_if_active();
        xEventGroupSetBits(s_events, XIAOZHI_EVENT_STOP);
        set_state(XIAOZHI_CLIENT_STATE_IDLE);
        break;
    case WEBSOCKET_EVENT_ERROR:
        ESP_LOGW(TAG, "websocket error type=%d status=%d", data->error_handle.error_type, data->error_handle.esp_ws_handshake_status_code);
        release_tts_output_if_active();
        if (data->error_handle.esp_ws_handshake_status_code > 0) {
            char detail[48];
            snprintf(detail, sizeof(detail), "websocket error (%d)", data->error_handle.esp_ws_handshake_status_code);
            set_error_with_http_status(detail, data->error_handle.esp_ws_handshake_status_code);
        } else {
            set_error_with_http_status("websocket error", -1);
        }
        xEventGroupSetBits(s_events, XIAOZHI_EVENT_ERROR | XIAOZHI_EVENT_STOP);
        break;
    default:
        break;
    }
}

static void probe_websocket_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    (void)base;
    xiaozhi_probe_context_t *probe = (xiaozhi_probe_context_t *)handler_args;
    esp_websocket_event_data_t *data = (esp_websocket_event_data_t *)event_data;
    if (probe == NULL || probe->result == NULL) {
        return;
    }

    switch (event_id) {
    case WEBSOCKET_EVENT_CONNECTED:
        probe->result->websocket_connected = true;
        probe->result->http_status = data->error_handle.esp_ws_handshake_status_code;
        if (send_hello_on_client(probe->client) != ESP_OK) {
            probe->result->err = ESP_FAIL;
            strlcpy(probe->result->detail, "hello send failed", sizeof(probe->result->detail));
            xEventGroupSetBits(probe->events, XIAOZHI_EVENT_ERROR);
        }
        break;
    case WEBSOCKET_EVENT_DATA:
        probe->result->http_status = data->error_handle.esp_ws_handshake_status_code;
        if ((data->op_code == 0x1 || data->op_code == 0x0) && data->payload_offset == 0 && data->fin) {
            handle_probe_text_message(probe, data->data_ptr, data->data_len);
        }
        break;
    case WEBSOCKET_EVENT_ERROR:
        probe->result->http_status = data->error_handle.esp_ws_handshake_status_code;
        probe->result->err = ESP_FAIL;
        if (probe->result->detail[0] == '\0') {
            strlcpy(probe->result->detail, "websocket error", sizeof(probe->result->detail));
        }
        xEventGroupSetBits(probe->events, XIAOZHI_EVENT_ERROR);
        break;
    case WEBSOCKET_EVENT_DISCONNECTED:
    case WEBSOCKET_EVENT_CLOSED:
        xEventGroupSetBits(probe->events, XIAOZHI_EVENT_STOP);
        break;
    default:
        break;
    }
}

static esp_err_t build_headers(const ornament_settings_t *settings, char *headers, size_t headers_size)
{
    char mac[24] = {0};
    char client_id[48] = {0};
    char bearer[ORNAMENT_XIAOZHI_TOKEN_MAX + 16] = {0};
    get_device_mac(mac, sizeof(mac));
    get_client_id(client_id, sizeof(client_id));

    const char *token = s_runtime_token[0] != '\0' ? s_runtime_token : settings_xiaozhi_token_or_default(settings);
    if (token[0] != '\0') {
        snprintf(bearer, sizeof(bearer), "Bearer %s", token);
    }

    int written;
    if (bearer[0] != '\0') {
        written = snprintf(
            headers,
            headers_size,
            "Authorization: %s\r\nProtocol-Version: %d\r\nDevice-Id: %s\r\nClient-Id: %s\r\n",
            bearer,
            s_runtime_protocol_version,
            mac,
            client_id);
    } else {
        written = snprintf(
            headers,
            headers_size,
            "Protocol-Version: %d\r\nDevice-Id: %s\r\nClient-Id: %s\r\n",
            s_runtime_protocol_version,
            mac,
            client_id);
    }
    return written > 0 && written < (int)headers_size ? ESP_OK : ESP_ERR_NO_MEM;
}

static esp_err_t build_headers_for_values(
    const ornament_settings_t *settings,
    const char *token_override,
    char *headers,
    size_t headers_size)
{
    ornament_settings_t temp = {0};
    if (settings != NULL) {
        temp = *settings;
    }
    if (token_override != NULL && token_override[0] != '\0') {
        strlcpy(temp.xiaozhi_token, token_override, sizeof(temp.xiaozhi_token));
        temp.has_xiaozhi_token = true;
    }
    return build_headers(&temp, headers, headers_size);
}

static esp_err_t run_capture_loop(OpusEncoder *encoder, esp_websocket_client_handle_t client)
{
    const int frame_samples = (ORNAMENT_AUDIO_SAMPLE_RATE_HZ * CONFIG_ORNAMENT_XIAOZHI_FRAME_MS) / 1000;
    int16_t *pcm = calloc((size_t)frame_samples, sizeof(int16_t));
    unsigned char *opus = malloc(CONFIG_ORNAMENT_XIAOZHI_OPUS_MAX_BYTES);
    bool paused_for_tts = false;
    if (pcm == NULL || opus == NULL) {
        free(pcm);
        free(opus);
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = task_audio_input_start();
    if (err != ESP_OK) {
        free(pcm);
        free(opus);
        return err;
    }

    err = send_listen_state_on_client(client, "start");
    if (err != ESP_OK) {
        task_audio_input_stop();
        free(pcm);
        free(opus);
        return err;
    }

    ESP_LOGI(TAG, "capture loop start: stack_hwm=%lu bytes", (unsigned long)current_stack_high_water_bytes());

    while ((xEventGroupGetBits(s_events) & XIAOZHI_EVENT_STOP) == 0 &&
           client != NULL &&
           esp_websocket_client_is_connected(client)) {
        EventBits_t bits = xEventGroupGetBits(s_events);
        if (music_player_is_active()) {
            paused_for_tts = false;
            vTaskDelay(pdMS_TO_TICKS(CONFIG_ORNAMENT_XIAOZHI_FRAME_MS));
            continue;
        }
        if (bits & XIAOZHI_EVENT_SPEAKING) {
            paused_for_tts = true;
            vTaskDelay(pdMS_TO_TICKS(CONFIG_ORNAMENT_XIAOZHI_FRAME_MS));
            continue;
        }
        if ((bits & XIAOZHI_EVENT_LISTEN_RESTART) != 0 || paused_for_tts) {
            ESP_LOGI(TAG, "restarting listen window after speaking");
            err = send_listen_state_on_client(client, "start");
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "listen restart failed: %s", esp_err_to_name(err));
                break;
            }
            xEventGroupClearBits(s_events, XIAOZHI_EVENT_LISTEN_RESTART);
            paused_for_tts = false;
        }

        err = task_audio_input_read_mono(pcm, (size_t)frame_samples, CONFIG_ORNAMENT_XIAOZHI_FRAME_MS + 100);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "mic read failed: %s", esp_err_to_name(err));
            break;
        }

        if (s_codec_mutex == NULL || xSemaphoreTake(s_codec_mutex, pdMS_TO_TICKS(200)) != pdTRUE) {
            ESP_LOGW(TAG, "opus encode skipped: codec mutex busy");
            err = ESP_ERR_TIMEOUT;
            break;
        }
        int opus_len = opus_encode(encoder, pcm, frame_samples, opus, CONFIG_ORNAMENT_XIAOZHI_OPUS_MAX_BYTES);
        xSemaphoreGive(s_codec_mutex);
        if (opus_len < 0) {
            ESP_LOGW(TAG, "opus encode failed: %d", opus_len);
            err = ESP_FAIL;
            break;
        }
        int sent = esp_websocket_client_send_bin(client, (const char *)opus, opus_len, pdMS_TO_TICKS(1000));
        if (sent != opus_len) {
            ESP_LOGW(TAG, "opus frame send failed: %d/%d", sent, opus_len);
            err = ESP_FAIL;
            break;
        }
        increment_uplink_frames();
    }

    (void)send_listen_state_on_client(client, "stop");
    task_audio_input_stop();
    free(pcm);
    free(opus);
    return err;
}

static void capture_task(void *arg)
{
    xiaozhi_capture_context_t *ctx = (xiaozhi_capture_context_t *)arg;
    esp_err_t err = ESP_ERR_INVALID_ARG;

    ESP_LOGI(TAG, "capture task start: stack_bytes=%d", XIAOZHI_CAPTURE_TASK_STACK);
    if (ctx != NULL && ctx->encoder != NULL && ctx->client != NULL) {
        err = run_capture_loop(ctx->encoder, ctx->client);
    } else {
        set_error("capture ctx invalid");
    }

    s_capture_result = err;
    if (err != ESP_OK && (xEventGroupGetBits(s_events) & XIAOZHI_EVENT_STOP) == 0) {
        ESP_LOGW(TAG, "capture loop failed: %s", esp_err_to_name(err));
        set_error(esp_err_to_name(err));
        xEventGroupSetBits(s_events, XIAOZHI_EVENT_ERROR);
    }
    ESP_LOGI(
        TAG,
        "capture task done: err=%s stack_hwm=%lu bytes",
        esp_err_to_name(err),
        (unsigned long)current_stack_high_water_bytes());
    if (ctx != NULL && ctx->encoder != NULL) {
        opus_encoder_destroy(ctx->encoder);
        ctx->encoder = NULL;
    }

    if (s_mutex != NULL && xSemaphoreTake(s_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        s_capture_task = NULL;
        xSemaphoreGive(s_mutex);
    } else {
        s_capture_task = NULL;
    }
    xEventGroupSetBits(s_events, XIAOZHI_EVENT_CAPTURE_DONE);
    free(ctx);
    vTaskDelete(NULL);
}

static esp_err_t start_capture_task(OpusEncoder *encoder, esp_websocket_client_handle_t client)
{
    if (s_events == NULL || encoder == NULL || client == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    xiaozhi_capture_context_t *ctx = calloc(1, sizeof(*ctx));
    if (ctx == NULL) {
        return ESP_ERR_NO_MEM;
    }
    ctx->encoder = encoder;
    ctx->client = client;

    xEventGroupClearBits(s_events, XIAOZHI_EVENT_CAPTURE_DONE);
    s_capture_result = ESP_OK;

    TaskHandle_t task = NULL;
    BaseType_t created = xTaskCreateWithCaps(
        capture_task,
        "xiaozhi_cap",
        XIAOZHI_CAPTURE_TASK_STACK,
        ctx,
        XIAOZHI_TASK_PRIO,
        &task,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (created != pdPASS) {
        ESP_LOGW(
            TAG,
            "capture task PSRAM stack alloc failed: largest_spiram=%u largest_internal=%u",
            (unsigned int)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT),
            (unsigned int)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
        created = xTaskCreateWithCaps(
            capture_task,
            "xiaozhi_cap",
            XIAOZHI_CAPTURE_TASK_STACK,
            ctx,
            XIAOZHI_TASK_PRIO,
            &task,
            MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    if (created != pdPASS) {
        free(ctx);
        return ESP_ERR_NO_MEM;
    }

    if (s_mutex != NULL && xSemaphoreTake(s_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        s_capture_task = task;
        xSemaphoreGive(s_mutex);
    } else {
        s_capture_task = task;
    }
    log_heap_status("after_capture_task");
    return ESP_OK;
}

static esp_err_t wait_for_capture_task_done(TickType_t timeout_ticks)
{
    if (s_events == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    TaskHandle_t capture_task = NULL;
    if (s_mutex != NULL && xSemaphoreTake(s_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        capture_task = s_capture_task;
        xSemaphoreGive(s_mutex);
    } else {
        capture_task = s_capture_task;
    }
    if (capture_task == NULL) {
        return ESP_OK;
    }

    EventBits_t bits = xEventGroupWaitBits(
        s_events,
        XIAOZHI_EVENT_CAPTURE_DONE,
        pdFALSE,
        pdFALSE,
        timeout_ticks);
    return (bits & XIAOZHI_EVENT_CAPTURE_DONE) != 0 ? ESP_OK : ESP_ERR_TIMEOUT;
}

static void session_task(void *arg)
{
    (void)arg;
    while (true) {
        ESP_LOGI(TAG, "session task start: stack_bytes=%u", (unsigned int)s_session_task_stack_bytes);
        log_heap_status("session_start");

        xiaozhi_session_context_t *ctx = calloc(1, sizeof(*ctx));
        OpusEncoder *encoder = NULL;
        OpusDecoder *decoder = NULL;
        bool retry_requested = false;

        if (ctx == NULL) {
            set_error("session ctx no mem");
            goto attempt_done;
        }

        esp_err_t err = settings_load(&ctx->settings);
        if (err != ESP_OK) {
            set_error("settings load failed");
            goto cleanup_ctx;
        }
        copy_settings_to_snapshot(&ctx->settings);
        if (settings_xiaozhi_ws_url_or_default(&ctx->settings)[0] == '\0') {
            set_error("missing websocket url");
            goto cleanup_ctx;
        }
        if (CONFIG_ORNAMENT_XIAOZHI_MIC_PIN_DIN < 0) {
            set_error("missing mic gpio");
            goto cleanup_ctx;
        }
        if (!is_supported_frame_ms(CONFIG_ORNAMENT_XIAOZHI_FRAME_MS)) {
            set_error("unsupported opus frame ms");
            goto cleanup_ctx;
        }

        if (should_fetch_official_runtime_config(&ctx->settings)) {
            bool activation_pending = false;
            int ota_status = -1;
            err = fetch_runtime_config(&ctx->settings, &ota_status, &activation_pending);
            if (err != ESP_OK) {
                char detail[64];
                format_http_error_detail(detail, sizeof(detail), "ota config failed", err, ota_status, NULL);
                set_error(detail);
                goto cleanup_ctx;
            }
            copy_settings_to_snapshot(&ctx->settings);
            if (activation_pending) {
                for (int attempt = 0; attempt < 10 && activation_pending; attempt++) {
                    int activate_status = -1;
                    err = activate_runtime_config(&activate_status);
                    if (err == ESP_OK) {
                        err = fetch_runtime_config(&ctx->settings, &ota_status, &activation_pending);
                        if (err != ESP_OK) {
                            char detail[64];
                            format_http_error_detail(detail, sizeof(detail), "ota refresh failed", err, ota_status, NULL);
                            set_error(detail);
                            goto cleanup_ctx;
                        }
                        copy_settings_to_snapshot(&ctx->settings);
                        break;
                    }
                    if (err != ESP_ERR_TIMEOUT) {
                        char detail[64];
                        snprintf(detail, sizeof(detail), "activate failed (%d)", activate_status);
                        set_error(detail);
                        goto cleanup_ctx;
                    }
                    vTaskDelay(pdMS_TO_TICKS(3000));
                }
                if (activation_pending) {
                    if (s_mutex != NULL && xSemaphoreTake(s_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
                        set_state_locked(XIAOZHI_CLIENT_STATE_IDLE);
                        xSemaphoreGive(s_mutex);
                    }
                    goto cleanup_ctx;
                }
            }
        }

        int opus_err = 0;
        encoder = opus_encoder_create(XIAOZHI_OPUS_SAMPLE_RATE_HZ, XIAOZHI_OPUS_CHANNELS, OPUS_APPLICATION_VOIP, &opus_err);
        if (encoder == NULL || opus_err != OPUS_OK) {
            set_error("opus encoder failed");
            goto attempt_done;
        }
        decoder = opus_decoder_create(XIAOZHI_OPUS_SAMPLE_RATE_HZ, XIAOZHI_OPUS_CHANNELS, &opus_err);
        if (decoder == NULL || opus_err != OPUS_OK) {
            set_error("opus decoder failed");
            goto cleanup_opus;
        }
        (void)opus_encoder_ctl(encoder, OPUS_SET_COMPLEXITY(CONFIG_ORNAMENT_XIAOZHI_OPUS_COMPLEXITY));
        (void)opus_encoder_ctl(encoder, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE));

        err = build_headers(&ctx->settings, ctx->headers, sizeof(ctx->headers));
        if (err != ESP_OK) {
            set_error("header build failed");
            goto cleanup_opus;
        }

        const char *active_ws_url = s_runtime_ws_url[0] != '\0' ? s_runtime_ws_url : settings_xiaozhi_ws_url_or_default(&ctx->settings);
        if (s_mutex != NULL && xSemaphoreTake(s_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            strlcpy(s_snapshot.active_ws_url, active_ws_url, sizeof(s_snapshot.active_ws_url));
            xSemaphoreGive(s_mutex);
        }
        ESP_LOGI(
            TAG,
            "session target ws: saved=%s runtime=%s active=%s official_runtime=%d",
            settings_xiaozhi_ws_url_or_default(&ctx->settings),
            s_runtime_ws_url[0] != '\0' ? s_runtime_ws_url : "<empty>",
            active_ws_url,
            should_fetch_official_runtime_config(&ctx->settings));
        ESP_LOGI(
            TAG,
            "starting websocket: ws_task_stack=%d session_stack_hwm=%lu largest_internal=%u",
            XIAOZHI_WS_TASK_STACK,
            (unsigned long)current_stack_high_water_bytes(),
            (unsigned int)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));

        esp_websocket_client_config_t cfg = {
            .uri = active_ws_url,
            .headers = ctx->headers,
            .buffer_size = CONFIG_ORNAMENT_XIAOZHI_WS_BUFFER_BYTES,
            .task_stack = XIAOZHI_WS_TASK_STACK,
            .task_prio = 5,
            .network_timeout_ms = CONFIG_ORNAMENT_XIAOZHI_CONNECT_TIMEOUT_MS,
            .disable_auto_reconnect = true,
            .crt_bundle_attach = esp_crt_bundle_attach,
            .user_context = decoder,
        };

        set_state(XIAOZHI_CLIENT_STATE_CONNECTING);
        xEventGroupClearBits(
            s_events,
            XIAOZHI_EVENT_CONNECTED |
                XIAOZHI_EVENT_HELLO |
                XIAOZHI_EVENT_STOP |
                XIAOZHI_EVENT_SPEAKING |
                XIAOZHI_EVENT_ERROR |
                XIAOZHI_EVENT_LISTEN_RESTART |
                XIAOZHI_EVENT_CAPTURE_DONE);
        s_client = esp_websocket_client_init(&cfg);
        if (s_client == NULL) {
            set_error("websocket init failed");
            goto cleanup_opus;
        }
        ESP_ERROR_CHECK_WITHOUT_ABORT(esp_websocket_register_events(s_client, WEBSOCKET_EVENT_ANY, websocket_event_handler, NULL));

        err = esp_websocket_client_start(s_client);
        if (err != ESP_OK) {
            set_error("websocket start failed");
            goto cleanup_ws;
        }

        EventBits_t bits = xEventGroupWaitBits(
            s_events,
            XIAOZHI_EVENT_HELLO | XIAOZHI_EVENT_ERROR | XIAOZHI_EVENT_STOP,
            pdFALSE,
            pdFALSE,
            pdMS_TO_TICKS(CONFIG_ORNAMENT_XIAOZHI_CONNECT_TIMEOUT_MS));
        if ((bits & XIAOZHI_EVENT_HELLO) == 0) {
            char last_error[XIAOZHI_STATUS_TEXT_MAX] = {0};
            if (s_mutex != NULL && xSemaphoreTake(s_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
                strlcpy(last_error, s_snapshot.last_error, sizeof(last_error));
                xSemaphoreGive(s_mutex);
            }
            if ((bits & XIAOZHI_EVENT_ERROR) != 0 && last_error[0] != '\0') {
                set_error(last_error);
            } else {
                set_error("hello timeout");
            }
            goto cleanup_ws;
        }

        err = start_capture_task(encoder, s_client);
        if (err != ESP_OK) {
            set_error("capture task create failed");
            goto cleanup_ws;
        }
        encoder = NULL;

        EventBits_t session_bits = xEventGroupWaitBits(
            s_events,
            XIAOZHI_EVENT_ERROR | XIAOZHI_EVENT_STOP | XIAOZHI_EVENT_CAPTURE_DONE,
            pdFALSE,
            pdFALSE,
            portMAX_DELAY);
        if ((session_bits & XIAOZHI_EVENT_CAPTURE_DONE) == 0 &&
            s_client != NULL &&
            esp_websocket_client_is_connected(s_client)) {
            (void)esp_websocket_client_close(s_client, pdMS_TO_TICKS(1000));
        }
        err = wait_for_capture_task_done(pdMS_TO_TICKS(3000));
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "capture task exit timeout");
            set_error("capture task exit timeout");
        } else {
            ESP_LOGI(TAG, "capture task joined: err=%s", esp_err_to_name(s_capture_result));
        }

cleanup_ws:
        release_tts_output_if_active();
        if (s_client != NULL) {
            if (esp_websocket_client_is_connected(s_client)) {
                (void)esp_websocket_client_close(s_client, pdMS_TO_TICKS(1000));
            }
            (void)esp_websocket_unregister_events(s_client, WEBSOCKET_EVENT_ANY, websocket_event_handler);
            (void)esp_websocket_client_destroy(s_client);
            s_client = NULL;
        }
cleanup_opus:
        if (decoder != NULL) {
            opus_decoder_destroy(decoder);
        }
        if (encoder != NULL) {
            opus_encoder_destroy(encoder);
        }
cleanup_ctx:
        free(ctx);

attempt_done:
        log_heap_status("session_done");
        if (s_mutex != NULL && xSemaphoreTake(s_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            retry_requested = s_snapshot.session_requested;
            if (!retry_requested &&
                s_snapshot.state != XIAOZHI_CLIENT_STATE_ERROR &&
                s_snapshot.state != XIAOZHI_CLIENT_STATE_CONFIG_MISSING) {
                set_state_locked(XIAOZHI_CLIENT_STATE_IDLE);
            }
            s_snapshot.connected = false;
            xSemaphoreGive(s_mutex);
        }

        if (!retry_requested) {
            break;
        }

        ESP_LOGW(TAG, "session ended while AI is enabled, retrying in %d ms", XIAOZHI_RECONNECT_DELAY_MS);
        vTaskDelay(pdMS_TO_TICKS(XIAOZHI_RECONNECT_DELAY_MS));
        if (!session_requested_enabled()) {
            break;
        }
    }

    if (s_mutex != NULL && xSemaphoreTake(s_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        if (!s_snapshot.session_requested &&
            s_snapshot.state != XIAOZHI_CLIENT_STATE_ERROR &&
            s_snapshot.state != XIAOZHI_CLIENT_STATE_CONFIG_MISSING) {
            set_state_locked(XIAOZHI_CLIENT_STATE_IDLE);
        }
        s_snapshot.connected = false;
        s_session_task = NULL;
        s_session_starting = false;
        xSemaphoreGive(s_mutex);
    } else {
        s_session_task = NULL;
        s_session_starting = false;
    }
    vTaskDelete(NULL);
}

esp_err_t xiaozhi_client_init(void)
{
    if (s_mutex != NULL) {
        ornament_settings_t settings;
        if (settings_load(&settings) == ESP_OK) {
            if (s_runtime_client_id[0] == '\0' &&
                load_or_create_client_id(s_runtime_client_id, sizeof(s_runtime_client_id)) == ESP_OK) {
                strlcpy(s_snapshot.client_id, s_runtime_client_id, sizeof(s_snapshot.client_id));
            }
            if (sync_runtime_config_from_settings(&settings) == ESP_OK) {
                copy_settings_to_snapshot(&settings);
            }
        }
        return ESP_OK;
    }
    s_mutex = xSemaphoreCreateMutex();
    if (s_mutex == NULL) {
        return ESP_ERR_NO_MEM;
    }
    s_events = xEventGroupCreate();
    if (s_events == NULL) {
        vSemaphoreDelete(s_mutex);
        s_mutex = NULL;
        return ESP_ERR_NO_MEM;
    }
    s_codec_mutex = xSemaphoreCreateMutex();
    if (s_codec_mutex == NULL) {
        vEventGroupDelete(s_events);
        vSemaphoreDelete(s_mutex);
        s_events = NULL;
        s_mutex = NULL;
        return ESP_ERR_NO_MEM;
    }

    memset(&s_snapshot, 0, sizeof(s_snapshot));
    s_snapshot.enabled = true;
    s_snapshot.state = XIAOZHI_CLIENT_STATE_IDLE;
    s_snapshot.protocol_version = CONFIG_ORNAMENT_XIAOZHI_PROTOCOL_VERSION;

    ornament_settings_t settings;
    if (settings_load(&settings) == ESP_OK) {
        if (load_or_create_client_id(s_runtime_client_id, sizeof(s_runtime_client_id)) == ESP_OK) {
            strlcpy(s_snapshot.client_id, s_runtime_client_id, sizeof(s_snapshot.client_id));
        }
        (void)sync_runtime_config_from_settings(&settings);
        copy_settings_to_snapshot(&settings);
        if (s_snapshot.configured) {
            set_state(XIAOZHI_CLIENT_STATE_IDLE);
        }
    }
    ESP_RETURN_ON_ERROR(xiaozhi_mcp_init(), TAG, "mcp init failed");

    ESP_LOGI(TAG, "Xiaozhi client ready enabled=%d mic_gpio=%d", CONFIG_ORNAMENT_XIAOZHI_ENABLED, CONFIG_ORNAMENT_XIAOZHI_MIC_PIN_DIN);
    return ESP_OK;
}

esp_err_t xiaozhi_client_probe(const char *ws_url_override, const char *token_override, xiaozhi_probe_result_t *result)
{
    if (result == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(result, 0, sizeof(*result));
    result->err = ESP_ERR_INVALID_STATE;
    result->http_status = -1;

    ESP_RETURN_ON_ERROR(xiaozhi_client_init(), TAG, "init failed");

    if (s_mutex == NULL) {
        result->err = ESP_ERR_INVALID_STATE;
        strlcpy(result->detail, "client not initialized", sizeof(result->detail));
        return result->err;
    }

    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        result->err = ESP_ERR_TIMEOUT;
        strlcpy(result->detail, "client busy", sizeof(result->detail));
        return result->err;
    }
    bool session_busy = s_session_task != NULL || s_session_starting || s_client != NULL;
    xSemaphoreGive(s_mutex);
    if (session_busy) {
        result->err = ESP_ERR_INVALID_STATE;
        strlcpy(result->detail, "AI session running", sizeof(result->detail));
        return result->err;
    }

    ornament_settings_t settings;
    esp_err_t err = settings_load(&settings);
    if (err != ESP_OK) {
        result->err = err;
        strlcpy(result->detail, "settings load failed", sizeof(result->detail));
        return err;
    }

    if (s_runtime_client_id[0] == '\0') {
        err = load_or_create_client_id(s_runtime_client_id, sizeof(s_runtime_client_id));
        if (err != ESP_OK) {
            result->err = err;
            strlcpy(result->detail, "client id init failed", sizeof(result->detail));
            return err;
        }
    }
    if (ws_url_override == NULL && token_override == NULL &&
        should_fetch_official_runtime_config(&settings)) {
        int ota_status = -1;
        bool activation_pending = false;
        err = fetch_runtime_config(&settings, &ota_status, &activation_pending);
        if (err != ESP_OK) {
            result->err = err;
            format_http_error_detail(result->detail, sizeof(result->detail), "ota config failed", err, ota_status, NULL);
            return err;
        }
        if (activation_pending) {
            result->err = ESP_OK;
            strlcpy(result->detail, "activation code ready", sizeof(result->detail));
            return ESP_OK;
        }
    }

    const char *ws_url = (ws_url_override != NULL && ws_url_override[0] != '\0') ?
        ws_url_override : (s_runtime_ws_url[0] != '\0' ? s_runtime_ws_url : settings_xiaozhi_ws_url_or_default(&settings));
    const char *token = (token_override != NULL && token_override[0] != '\0') ?
        token_override : (s_runtime_token[0] != '\0' ? s_runtime_token : settings_xiaozhi_token_or_default(&settings));
    result->configured = ws_url[0] != '\0';
    if (!result->configured) {
        result->err = ESP_ERR_INVALID_ARG;
        strlcpy(result->detail, "missing websocket url", sizeof(result->detail));
        return result->err;
    }
    if (!is_supported_frame_ms(CONFIG_ORNAMENT_XIAOZHI_FRAME_MS)) {
        result->err = ESP_ERR_NOT_SUPPORTED;
        strlcpy(result->detail, "unsupported opus frame ms", sizeof(result->detail));
        return result->err;
    }

    char headers[XIAOZHI_HEADER_MAX];
    err = build_headers_for_values(&settings, token, headers, sizeof(headers));
    if (err != ESP_OK) {
        result->err = err;
        strlcpy(result->detail, "header build failed", sizeof(result->detail));
        return err;
    }

    EventGroupHandle_t probe_events = xEventGroupCreate();
    if (probe_events == NULL) {
        result->err = ESP_ERR_NO_MEM;
        strlcpy(result->detail, "event alloc failed", sizeof(result->detail));
        return result->err;
    }

    esp_websocket_client_config_t cfg = {
        .uri = ws_url,
        .headers = headers,
        .buffer_size = CONFIG_ORNAMENT_XIAOZHI_WS_BUFFER_BYTES,
        .task_stack = XIAOZHI_WS_TASK_STACK,
        .task_prio = 5,
        .network_timeout_ms = CONFIG_ORNAMENT_XIAOZHI_CONNECT_TIMEOUT_MS,
        .disable_auto_reconnect = true,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };

    esp_websocket_client_handle_t probe_client = esp_websocket_client_init(&cfg);
    if (probe_client == NULL) {
        vEventGroupDelete(probe_events);
        result->err = ESP_FAIL;
        strlcpy(result->detail, "websocket init failed", sizeof(result->detail));
        return result->err;
    }

    xiaozhi_probe_context_t probe = {
        .events = probe_events,
        .result = result,
        .client = probe_client,
    };

    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_websocket_register_events(probe_client, WEBSOCKET_EVENT_ANY, probe_websocket_event_handler, &probe));

    err = esp_websocket_client_start(probe_client);
    if (err != ESP_OK) {
        result->err = err;
        strlcpy(result->detail, "websocket start failed", sizeof(result->detail));
        goto cleanup;
    }

    EventBits_t bits = xEventGroupWaitBits(
        probe_events,
        XIAOZHI_EVENT_HELLO | XIAOZHI_EVENT_ERROR | XIAOZHI_EVENT_STOP,
        pdFALSE,
        pdFALSE,
        pdMS_TO_TICKS(CONFIG_ORNAMENT_XIAOZHI_CONNECT_TIMEOUT_MS));
    if (bits & XIAOZHI_EVENT_HELLO) {
        result->err = ESP_OK;
    } else if (bits & XIAOZHI_EVENT_ERROR) {
        if (result->detail[0] == '\0') {
            strlcpy(result->detail, "websocket error", sizeof(result->detail));
        }
        if (result->err == ESP_ERR_INVALID_STATE) {
            result->err = ESP_FAIL;
        }
    } else if (bits & XIAOZHI_EVENT_STOP) {
        result->err = ESP_FAIL;
        if (result->detail[0] == '\0') {
            strlcpy(result->detail, "connection closed", sizeof(result->detail));
        }
    } else {
        result->err = ESP_ERR_TIMEOUT;
        if (result->detail[0] == '\0') {
            strlcpy(result->detail, "hello timeout", sizeof(result->detail));
        }
    }

cleanup:
    if (esp_websocket_client_is_connected(probe_client)) {
        (void)esp_websocket_client_close(probe_client, pdMS_TO_TICKS(1000));
    }
    (void)esp_websocket_unregister_events(probe_client, WEBSOCKET_EVENT_ANY, probe_websocket_event_handler);
    (void)esp_websocket_client_destroy(probe_client);
    vEventGroupDelete(probe_events);
    return result->err;
}

esp_err_t xiaozhi_client_start_session(void)
{
    ESP_RETURN_ON_ERROR(xiaozhi_client_init(), TAG, "init failed");
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    if (s_session_task != NULL || s_session_starting || s_capture_task != NULL) {
        xSemaphoreGive(s_mutex);
        return ESP_ERR_INVALID_STATE;
    }
    set_session_requested_locked(true);
    if (s_snapshot.configured) {
        set_state_locked(XIAOZHI_CLIENT_STATE_CONNECTING);
    }
    s_snapshot.last_error[0] = '\0';
    s_session_starting = true;
    log_heap_status("before_session_task");
    s_session_task_stack_bytes = select_session_task_stack_bytes();
    /*
     * TLS / flash operations in this path can run with cache disabled.
     * Keep the session task stack in internal RAM instead of external PSRAM.
     */
    BaseType_t created = xTaskCreateWithCaps(
        session_task,
        "xiaozhi",
        s_session_task_stack_bytes,
        NULL,
        XIAOZHI_TASK_PRIO,
        &s_session_task,
        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (created != pdPASS) {
        set_session_requested_locked(false);
        if (s_snapshot.configured) {
            set_state_locked(XIAOZHI_CLIENT_STATE_IDLE);
        }
        s_session_starting = false;
        s_session_task = NULL;
        xSemaphoreGive(s_mutex);
        ESP_LOGW(
            TAG,
            "session task create failed: stack=%u largest_internal=%u",
            (unsigned int)s_session_task_stack_bytes,
            (unsigned int)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
        return ESP_ERR_NO_MEM;
    }
    s_session_starting = false;
    xSemaphoreGive(s_mutex);
    log_heap_status("after_session_task");
    return ESP_OK;
}

esp_err_t xiaozhi_client_reconnect_session(bool start_if_idle)
{
    ESP_RETURN_ON_ERROR(xiaozhi_client_init(), TAG, "init failed");
    if (s_mutex == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    bool restart_running_session = false;
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    restart_running_session = session_runtime_active_locked();
    xSemaphoreGive(s_mutex);

    if (!restart_running_session) {
        return start_if_idle ? xiaozhi_client_start_session() : ESP_OK;
    }

    esp_err_t err = xiaozhi_client_stop_session();
    if (err != ESP_OK) {
        return err;
    }

    return xiaozhi_client_start_session();
}

esp_err_t xiaozhi_client_stop_session(void)
{
    bool had_runtime = false;
    if (s_mutex != NULL && xSemaphoreTake(s_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        had_runtime = session_runtime_active_locked();
        set_session_requested_locked(false);
        if (s_snapshot.configured) {
            set_state_locked(XIAOZHI_CLIENT_STATE_IDLE);
        }
        xSemaphoreGive(s_mutex);
    }
    if (s_events == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    xEventGroupSetBits(s_events, XIAOZHI_EVENT_STOP);
    esp_websocket_client_handle_t client = s_client;
    if (client != NULL && esp_websocket_client_is_connected(client)) {
        (void)esp_websocket_client_close(client, pdMS_TO_TICKS(1000));
    }
    if (!had_runtime) {
        return ESP_OK;
    }
    return wait_for_session_runtime_inactive(pdMS_TO_TICKS(5000));
}

#else

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

bool xiaozhi_client_session_requested(void)
{
    return false;
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

#endif
