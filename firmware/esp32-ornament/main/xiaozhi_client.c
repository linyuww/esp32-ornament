#include "xiaozhi_client.h"

#include "cJSON.h"
#include "device_identity.h"
#include "esp_check.h"
#include "esp_crt_bundle.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_websocket_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "task_audio.h"

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
#define XIAOZHI_JSON_MAX 640
#define XIAOZHI_HEADER_MAX 512
#define XIAOZHI_OPUS_SAMPLE_RATE_HZ ORNAMENT_AUDIO_SAMPLE_RATE_HZ
#define XIAOZHI_OPUS_CHANNELS 1
#define XIAOZHI_TASK_STACK 12288
#define XIAOZHI_TASK_PRIO 5
#define XIAOZHI_OPUS_DECODE_MAX_FRAMES (ORNAMENT_AUDIO_SAMPLE_RATE_HZ * 60 / 1000)

static const char *TAG = "xiaozhi";

static SemaphoreHandle_t s_mutex;
static EventGroupHandle_t s_events;
static TaskHandle_t s_session_task;
static esp_websocket_client_handle_t s_client;
static xiaozhi_client_snapshot_t s_snapshot;
static bool s_session_starting;

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
    if (s_mutex == NULL) {
        return;
    }
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        set_state_locked(XIAOZHI_CLIENT_STATE_ERROR);
        strlcpy(s_snapshot.last_error, message != NULL ? message : "unknown", sizeof(s_snapshot.last_error));
        xSemaphoreGive(s_mutex);
    }
}

static void copy_settings_to_snapshot(const ornament_settings_t *settings)
{
    if (s_mutex == NULL || settings == NULL) {
        return;
    }
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        s_snapshot.enabled = true;
        strlcpy(s_snapshot.ws_url, settings_xiaozhi_ws_url_or_default(settings), sizeof(s_snapshot.ws_url));
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
    } else {
        strlcpy(target, device_identity_hostname(), target_size);
    }
}

static esp_err_t send_text_frame(const char *json)
{
    if (s_client == NULL || json == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    int len = (int)strlen(json);
    int sent = esp_websocket_client_send_text(s_client, json, len, pdMS_TO_TICKS(2000));
    return sent == len ? ESP_OK : ESP_FAIL;
}

static esp_err_t send_hello(void)
{
    char json[XIAOZHI_JSON_MAX];
    int written = snprintf(
        json,
        sizeof(json),
        "{\"type\":\"hello\",\"version\":%d,\"transport\":\"websocket\","
        "\"audio_params\":{\"format\":\"opus\",\"sample_rate\":%d,\"channels\":1,\"frame_duration\":%d}}",
        CONFIG_ORNAMENT_XIAOZHI_PROTOCOL_VERSION,
        XIAOZHI_OPUS_SAMPLE_RATE_HZ,
        CONFIG_ORNAMENT_XIAOZHI_FRAME_MS);
    if (written <= 0 || written >= (int)sizeof(json)) {
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "sending hello");
    return send_text_frame(json);
}

static esp_err_t send_listen_state(const char *state)
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
        "{\"session_id\":\"%s\",\"type\":\"listen\",\"state\":\"%s\",\"mode\":\"manual\"}",
        session_id,
        state);
    if (written <= 0 || written >= (int)sizeof(json)) {
        return ESP_ERR_NO_MEM;
    }
    return send_text_frame(json);
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

    if (strcmp(type->valuestring, "hello") == 0) {
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
                set_state(XIAOZHI_CLIENT_STATE_SPEAKING);
                xEventGroupSetBits(s_events, XIAOZHI_EVENT_SPEAKING);
            } else if (strcmp(state->valuestring, "stop") == 0) {
                set_state(XIAOZHI_CLIENT_STATE_LISTENING);
                xEventGroupClearBits(s_events, XIAOZHI_EVENT_SPEAKING);
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
            int decoded = opus_decode((OpusDecoder *)data->user_context, (const unsigned char *)data->data_ptr, data->data_len, pcm, XIAOZHI_OPUS_DECODE_MAX_FRAMES, 0);
            if (decoded > 0 && (xEventGroupGetBits(s_events) & XIAOZHI_EVENT_SPEAKING)) {
                if (task_audio_output_acquire() == ESP_OK) {
                    esp_err_t err = task_audio_output_write_mono(pcm, (size_t)decoded, 1000);
                    task_audio_output_release();
                    if (err == ESP_OK) {
                        increment_downlink_frames();
                    } else {
                        ESP_LOGW(TAG, "tts write failed: %s", esp_err_to_name(err));
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
        xEventGroupSetBits(s_events, XIAOZHI_EVENT_STOP);
        set_state(XIAOZHI_CLIENT_STATE_IDLE);
        break;
    case WEBSOCKET_EVENT_ERROR:
        ESP_LOGW(TAG, "websocket error type=%d status=%d", data->error_handle.error_type, data->error_handle.esp_ws_handshake_status_code);
        set_error("websocket error");
        xEventGroupSetBits(s_events, XIAOZHI_EVENT_ERROR | XIAOZHI_EVENT_STOP);
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

    const char *token = settings_xiaozhi_token_or_default(settings);
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
            CONFIG_ORNAMENT_XIAOZHI_PROTOCOL_VERSION,
            mac,
            client_id);
    } else {
        written = snprintf(
            headers,
            headers_size,
            "Protocol-Version: %d\r\nDevice-Id: %s\r\nClient-Id: %s\r\n",
            CONFIG_ORNAMENT_XIAOZHI_PROTOCOL_VERSION,
            mac,
            client_id);
    }
    return written > 0 && written < (int)headers_size ? ESP_OK : ESP_ERR_NO_MEM;
}

static esp_err_t run_capture_loop(OpusEncoder *encoder)
{
    const int frame_samples = (ORNAMENT_AUDIO_SAMPLE_RATE_HZ * CONFIG_ORNAMENT_XIAOZHI_FRAME_MS) / 1000;
    int16_t *pcm = calloc((size_t)frame_samples, sizeof(int16_t));
    unsigned char *opus = malloc(CONFIG_ORNAMENT_XIAOZHI_OPUS_MAX_BYTES);
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

    err = send_listen_state("start");
    if (err != ESP_OK) {
        task_audio_input_stop();
        free(pcm);
        free(opus);
        return err;
    }

    while ((xEventGroupGetBits(s_events) & XIAOZHI_EVENT_STOP) == 0 &&
           s_client != NULL &&
           esp_websocket_client_is_connected(s_client)) {
        EventBits_t bits = xEventGroupGetBits(s_events);
        if (bits & XIAOZHI_EVENT_SPEAKING) {
            vTaskDelay(pdMS_TO_TICKS(CONFIG_ORNAMENT_XIAOZHI_FRAME_MS));
            continue;
        }

        err = task_audio_input_read_mono(pcm, (size_t)frame_samples, CONFIG_ORNAMENT_XIAOZHI_FRAME_MS + 100);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "mic read failed: %s", esp_err_to_name(err));
            break;
        }

        int opus_len = opus_encode(encoder, pcm, frame_samples, opus, CONFIG_ORNAMENT_XIAOZHI_OPUS_MAX_BYTES);
        if (opus_len < 0) {
            ESP_LOGW(TAG, "opus encode failed: %d", opus_len);
            err = ESP_FAIL;
            break;
        }
        int sent = esp_websocket_client_send_bin(s_client, (const char *)opus, opus_len, pdMS_TO_TICKS(1000));
        if (sent != opus_len) {
            ESP_LOGW(TAG, "opus frame send failed: %d/%d", sent, opus_len);
            err = ESP_FAIL;
            break;
        }
        increment_uplink_frames();
    }

    (void)send_listen_state("stop");
    task_audio_input_stop();
    free(pcm);
    free(opus);
    return err;
}

static void session_task(void *arg)
{
    (void)arg;
    ornament_settings_t settings;
    esp_err_t err = settings_load(&settings);
    if (err != ESP_OK) {
        set_error("settings load failed");
        goto done;
    }
    copy_settings_to_snapshot(&settings);
    if (settings_xiaozhi_ws_url_or_default(&settings)[0] == '\0') {
        set_error("missing websocket url");
        goto done;
    }
    if (CONFIG_ORNAMENT_XIAOZHI_MIC_PIN_DIN < 0) {
        set_error("missing mic gpio");
        goto done;
    }
    if (!is_supported_frame_ms(CONFIG_ORNAMENT_XIAOZHI_FRAME_MS)) {
        set_error("unsupported opus frame ms");
        goto done;
    }

    int opus_err = 0;
    OpusEncoder *encoder = opus_encoder_create(XIAOZHI_OPUS_SAMPLE_RATE_HZ, XIAOZHI_OPUS_CHANNELS, OPUS_APPLICATION_VOIP, &opus_err);
    if (encoder == NULL || opus_err != OPUS_OK) {
        set_error("opus encoder failed");
        goto done;
    }
    OpusDecoder *decoder = opus_decoder_create(XIAOZHI_OPUS_SAMPLE_RATE_HZ, XIAOZHI_OPUS_CHANNELS, &opus_err);
    if (decoder == NULL || opus_err != OPUS_OK) {
        opus_encoder_destroy(encoder);
        set_error("opus decoder failed");
        goto done;
    }
    (void)opus_encoder_ctl(encoder, OPUS_SET_COMPLEXITY(CONFIG_ORNAMENT_XIAOZHI_OPUS_COMPLEXITY));
    (void)opus_encoder_ctl(encoder, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE));

    char headers[XIAOZHI_HEADER_MAX];
    err = build_headers(&settings, headers, sizeof(headers));
    if (err != ESP_OK) {
        set_error("header build failed");
        goto cleanup_opus;
    }

    esp_websocket_client_config_t cfg = {
        .uri = settings_xiaozhi_ws_url_or_default(&settings),
        .headers = headers,
        .buffer_size = CONFIG_ORNAMENT_XIAOZHI_WS_BUFFER_BYTES,
        .task_stack = 8192,
        .task_prio = 5,
        .network_timeout_ms = CONFIG_ORNAMENT_XIAOZHI_CONNECT_TIMEOUT_MS,
        .disable_auto_reconnect = true,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .user_context = decoder,
    };

    set_state(XIAOZHI_CLIENT_STATE_CONNECTING);
    xEventGroupClearBits(s_events, XIAOZHI_EVENT_CONNECTED | XIAOZHI_EVENT_HELLO | XIAOZHI_EVENT_STOP | XIAOZHI_EVENT_SPEAKING | XIAOZHI_EVENT_ERROR);
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
        set_error("hello timeout");
        goto cleanup_ws;
    }

    err = run_capture_loop(encoder);
    if (err != ESP_OK && (xEventGroupGetBits(s_events) & XIAOZHI_EVENT_STOP) == 0) {
        set_error(esp_err_to_name(err));
    }

cleanup_ws:
    if (s_client != NULL) {
        if (esp_websocket_client_is_connected(s_client)) {
            (void)esp_websocket_client_close(s_client, pdMS_TO_TICKS(1000));
        }
        (void)esp_websocket_unregister_events(s_client, WEBSOCKET_EVENT_ANY, websocket_event_handler);
        (void)esp_websocket_client_destroy(s_client);
        s_client = NULL;
    }
cleanup_opus:
    opus_decoder_destroy(decoder);
    opus_encoder_destroy(encoder);
done:
    if (s_mutex != NULL && xSemaphoreTake(s_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        if (s_snapshot.state != XIAOZHI_CLIENT_STATE_ERROR &&
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

    memset(&s_snapshot, 0, sizeof(s_snapshot));
    s_snapshot.enabled = true;
    s_snapshot.state = XIAOZHI_CLIENT_STATE_IDLE;

    ornament_settings_t settings;
    if (settings_load(&settings) == ESP_OK) {
        copy_settings_to_snapshot(&settings);
        if (s_snapshot.configured) {
            set_state(XIAOZHI_CLIENT_STATE_IDLE);
        }
    }

    ESP_LOGI(TAG, "Xiaozhi client ready enabled=%d mic_gpio=%d", CONFIG_ORNAMENT_XIAOZHI_ENABLED, CONFIG_ORNAMENT_XIAOZHI_MIC_PIN_DIN);
    return ESP_OK;
}

esp_err_t xiaozhi_client_start_session(void)
{
    ESP_RETURN_ON_ERROR(xiaozhi_client_init(), TAG, "init failed");
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    if (s_session_task != NULL || s_session_starting) {
        xSemaphoreGive(s_mutex);
        return ESP_ERR_INVALID_STATE;
    }
    s_session_starting = true;
    BaseType_t created = xTaskCreate(session_task, "xiaozhi", XIAOZHI_TASK_STACK, NULL, XIAOZHI_TASK_PRIO, &s_session_task);
    if (created != pdPASS) {
        s_session_starting = false;
        s_session_task = NULL;
        xSemaphoreGive(s_mutex);
        return ESP_ERR_NO_MEM;
    }
    s_session_starting = false;
    xSemaphoreGive(s_mutex);
    return ESP_OK;
}

esp_err_t xiaozhi_client_stop_session(void)
{
    if (s_events == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    xEventGroupSetBits(s_events, XIAOZHI_EVENT_STOP);
    esp_websocket_client_handle_t client = s_client;
    if (client != NULL && esp_websocket_client_is_connected(client)) {
        return esp_websocket_client_close(client, pdMS_TO_TICKS(1000));
    }
    return ESP_OK;
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

esp_err_t xiaozhi_client_stop_session(void)
{
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
