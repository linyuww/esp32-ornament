#include "music_player.h"

#include "cJSON.h"
#include "esp_check.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "settings.h"
#include "task_audio.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef CONFIG_ORNAMENT_XIAOZHI_MCP_ENABLED
#define CONFIG_ORNAMENT_XIAOZHI_MCP_ENABLED 0
#endif

#ifndef CONFIG_ORNAMENT_BRIDGE_MUSIC_STREAM_TIMEOUT_MS
#define CONFIG_ORNAMENT_BRIDGE_MUSIC_STREAM_TIMEOUT_MS 2000
#endif

#ifndef CONFIG_ORNAMENT_BRIDGE_MUSIC_PCM_CHUNK_BYTES
#define CONFIG_ORNAMENT_BRIDGE_MUSIC_PCM_CHUNK_BYTES 2048
#endif

#if CONFIG_ORNAMENT_XIAOZHI_ENABLED && CONFIG_ORNAMENT_XIAOZHI_MCP_ENABLED

#define MUSIC_PLAYER_TASK_DONE BIT0
#define MUSIC_PLAYER_URL_MAX 512
#define MUSIC_PLAYER_RESPONSE_MAX 2048
#define MUSIC_PLAYER_TASK_STACK 8192

typedef struct {
    char *data;
    int length;
    int capacity;
} music_response_buffer_t;

typedef struct {
    char song_name[ORNAMENT_TEXT_MAX];
    char artist_name[ORNAMENT_TEXT_MAX];
    uint32_t index;
} music_player_request_t;

typedef struct {
    char title[ORNAMENT_TEXT_MAX];
    char artist[ORNAMENT_TEXT_MAX];
    char album[ORNAMENT_TEXT_MAX];
    char picture[ORNAMENT_BRIDGE_URL_MAX];
} resolved_song_t;

static const char *TAG = "music_player";

static SemaphoreHandle_t s_mutex;
static EventGroupHandle_t s_events;
static TaskHandle_t s_task;
static music_player_snapshot_t s_snapshot;
static bool s_stop_requested;

static void set_state_locked(music_player_state_t state)
{
    s_snapshot.state = state;
    s_snapshot.active = state == MUSIC_PLAYER_STATE_RESOLVING ||
                        state == MUSIC_PLAYER_STATE_PLAYING ||
                        state == MUSIC_PLAYER_STATE_STOPPING;
}

static void clear_metadata_locked(void)
{
    s_snapshot.title[0] = '\0';
    s_snapshot.album[0] = '\0';
    s_snapshot.picture[0] = '\0';
    s_snapshot.last_error[0] = '\0';
}

static void set_error_locked(const char *message)
{
    set_state_locked(MUSIC_PLAYER_STATE_ERROR);
    strlcpy(s_snapshot.last_error, message != NULL ? message : "unknown", sizeof(s_snapshot.last_error));
}

static void mark_idle_locked(void)
{
    s_stop_requested = false;
    s_snapshot.stop_requested = false;
    set_state_locked(MUSIC_PLAYER_STATE_IDLE);
}

static bool stop_requested(void)
{
    if (s_mutex == NULL) {
        return false;
    }
    bool requested = false;
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
        requested = s_stop_requested;
        xSemaphoreGive(s_mutex);
    }
    return requested;
}

static esp_err_t http_event_handler(esp_http_client_event_t *event)
{
    music_response_buffer_t *buffer = (music_response_buffer_t *)event->user_data;
    if (event->event_id != HTTP_EVENT_ON_DATA || buffer == NULL || event->data == NULL) {
        return ESP_OK;
    }
    if (buffer->length + event->data_len >= buffer->capacity) {
        return ESP_FAIL;
    }
    memcpy(buffer->data + buffer->length, event->data, event->data_len);
    buffer->length += event->data_len;
    buffer->data[buffer->length] = '\0';
    return ESP_OK;
}

static void copy_json_string(const cJSON *root, const char *name, char *target, size_t target_size)
{
    if (root == NULL || name == NULL || target == NULL || target_size == 0) {
        return;
    }
    const cJSON *value = cJSON_GetObjectItemCaseSensitive((cJSON *)root, name);
    if (cJSON_IsString(value) && value->valuestring != NULL) {
        strlcpy(target, value->valuestring, target_size);
    }
}

static char hex_digit(unsigned value)
{
    return (char)(value < 10 ? ('0' + value) : ('A' + (value - 10)));
}

static void append_urlencoded(char *target, size_t target_size, size_t *used, const char *value)
{
    if (target == NULL || target_size == 0 || used == NULL || value == NULL) {
        return;
    }

    for (const unsigned char *cursor = (const unsigned char *)value; *cursor != '\0'; cursor++) {
        unsigned char ch = *cursor;
        if ((ch >= 'A' && ch <= 'Z') ||
            (ch >= 'a' && ch <= 'z') ||
            (ch >= '0' && ch <= '9') ||
            ch == '-' || ch == '_' || ch == '.' || ch == '~') {
            if (*used + 1 >= target_size) {
                return;
            }
            target[(*used)++] = (char)ch;
        } else if (ch == ' ') {
            if (*used + 1 >= target_size) {
                return;
            }
            target[(*used)++] = '+';
        } else {
            if (*used + 3 >= target_size) {
                return;
            }
            target[(*used)++] = '%';
            target[(*used)++] = hex_digit(ch >> 4);
            target[(*used)++] = hex_digit(ch & 0x0F);
        }
    }
    target[*used] = '\0';
}

static esp_err_t build_music_request_url(
    const char *base_url,
    const char *endpoint,
    const char *song_name,
    const char *artist_name,
    uint32_t index,
    char *target,
    size_t target_size)
{
    if (base_url == NULL || endpoint == NULL || song_name == NULL || song_name[0] == '\0' || target == NULL || target_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    int written = snprintf(target, target_size, "%s/%s?song=", base_url, endpoint);
    if (written <= 0 || written >= (int)target_size) {
        return ESP_ERR_NO_MEM;
    }
    size_t used = (size_t)written;
    append_urlencoded(target, target_size, &used, song_name);

    if (artist_name != NULL && artist_name[0] != '\0') {
        if (used + strlen("&artist=") >= target_size) {
            return ESP_ERR_NO_MEM;
        }
        memcpy(target + used, "&artist=", strlen("&artist="));
        used += strlen("&artist=");
        target[used] = '\0';
        append_urlencoded(target, target_size, &used, artist_name);
    }

    written = snprintf(target + used, target_size - used, "&index=%lu", (unsigned long)(index == 0 ? 1UL : index));
    if (written <= 0 || written >= (int)(target_size - used)) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

static esp_err_t apply_music_auth_header(esp_http_client_handle_t client, const ornament_settings_t *settings)
{
    const char *token = settings_bridge_music_token_or_default(settings);
    if (client == NULL || token == NULL || token[0] == '\0') {
        return ESP_OK;
    }

    char bearer[ORNAMENT_BRIDGE_MUSIC_TOKEN_MAX + 16] = {0};
    int written = snprintf(bearer, sizeof(bearer), "Bearer %s", token);
    if (written <= 0 || written >= (int)sizeof(bearer)) {
        return ESP_ERR_NO_MEM;
    }
    return esp_http_client_set_header(client, "Authorization", bearer);
}

static esp_err_t resolve_song(
    const ornament_settings_t *settings,
    const music_player_request_t *request,
    resolved_song_t *resolved)
{
    if (settings == NULL || request == NULL || resolved == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    char base_url[ORNAMENT_BRIDGE_MUSIC_BASE_URL_MAX] = {0};
    ESP_RETURN_ON_ERROR(
        settings_resolve_bridge_music_base_url(settings, base_url, sizeof(base_url)),
        TAG,
        "music base url resolve failed");

    char url[MUSIC_PLAYER_URL_MAX] = {0};
    ESP_RETURN_ON_ERROR(
        build_music_request_url(
            base_url,
            "resolve",
            request->song_name,
            request->artist_name[0] != '\0' ? request->artist_name : NULL,
            request->index,
            url,
            sizeof(url)),
        TAG,
        "resolve url build failed");

    char *response = calloc(MUSIC_PLAYER_RESPONSE_MAX, 1);
    if (response == NULL) {
        return ESP_ERR_NO_MEM;
    }

    music_response_buffer_t buffer = {
        .data = response,
        .length = 0,
        .capacity = MUSIC_PLAYER_RESPONSE_MAX,
    };
    esp_http_client_config_t config = {
        .url = url,
        .event_handler = http_event_handler,
        .user_data = &buffer,
        .timeout_ms = CONFIG_ORNAMENT_BRIDGE_MUSIC_STREAM_TIMEOUT_MS,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        free(response);
        return ESP_FAIL;
    }
    ESP_ERROR_CHECK_WITHOUT_ABORT(apply_music_auth_header(client, settings));

    esp_err_t err = esp_http_client_perform(client);
    int status_code = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    if (err != ESP_OK) {
        free(response);
        return err;
    }
    if (status_code != 200) {
        free(response);
        return ESP_ERR_HTTP_FETCH_HEADER;
    }

    cJSON *root = cJSON_Parse(response);
    free(response);
    if (root == NULL) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    resolved_song_t parsed = {0};
    copy_json_string(root, "title", parsed.title, sizeof(parsed.title));
    copy_json_string(root, "artist", parsed.artist, sizeof(parsed.artist));
    copy_json_string(root, "album", parsed.album, sizeof(parsed.album));
    copy_json_string(root, "picture", parsed.picture, sizeof(parsed.picture));
    cJSON *ok = cJSON_GetObjectItemCaseSensitive(root, "ok");
    cJSON *error = cJSON_GetObjectItemCaseSensitive(root, "error");
    cJSON_Delete(root);

    if (ok != NULL && cJSON_IsBool(ok) && !cJSON_IsTrue(ok)) {
        if (cJSON_IsString(error) && error->valuestring != NULL) {
            ESP_LOGW(TAG, "bridge music resolve rejected: %s", error->valuestring);
        }
        return ESP_FAIL;
    }
    if (parsed.title[0] == '\0') {
        strlcpy(parsed.title, request->song_name, sizeof(parsed.title));
    }
    if (parsed.artist[0] == '\0') {
        strlcpy(parsed.artist, request->artist_name, sizeof(parsed.artist));
    }
    *resolved = parsed;
    return ESP_OK;
}

static esp_err_t stream_song(
    const ornament_settings_t *settings,
    const music_player_request_t *request)
{
    char base_url[ORNAMENT_BRIDGE_MUSIC_BASE_URL_MAX] = {0};
    ESP_RETURN_ON_ERROR(
        settings_resolve_bridge_music_base_url(settings, base_url, sizeof(base_url)),
        TAG,
        "music base url resolve failed");

    char url[MUSIC_PLAYER_URL_MAX] = {0};
    ESP_RETURN_ON_ERROR(
        build_music_request_url(
            base_url,
            "stream",
            request->song_name,
            request->artist_name[0] != '\0' ? request->artist_name : NULL,
            request->index,
            url,
            sizeof(url)),
        TAG,
        "stream url build failed");

    esp_http_client_config_t config = {
        .url = url,
        .timeout_ms = CONFIG_ORNAMENT_BRIDGE_MUSIC_STREAM_TIMEOUT_MS,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        return ESP_FAIL;
    }

    ESP_ERROR_CHECK_WITHOUT_ABORT(apply_music_auth_header(client, settings));
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_http_client_set_header(client, "Accept", "audio/L16"));

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        esp_http_client_cleanup(client);
        return err;
    }

    (void)esp_http_client_fetch_headers(client);
    int status_code = esp_http_client_get_status_code(client);
    if (status_code != 200) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_ERR_HTTP_FETCH_HEADER;
    }

    err = task_audio_output_acquire_with_volume(settings);
    if (err != ESP_OK) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return err;
    }

    uint8_t *buffer = calloc(CONFIG_ORNAMENT_BRIDGE_MUSIC_PCM_CHUNK_BYTES + 1, 1);
    if (buffer == NULL) {
        task_audio_output_release();
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_ERR_NO_MEM;
    }

    size_t carry = 0;
    while (!stop_requested()) {
        int read = esp_http_client_read(
            client,
            (char *)buffer + carry,
            CONFIG_ORNAMENT_BRIDGE_MUSIC_PCM_CHUNK_BYTES - (int)carry);
        if (read == -ESP_ERR_HTTP_EAGAIN) {
            continue;
        }
        if (read < 0) {
            err = ESP_FAIL;
            break;
        }
        if (read == 0) {
            err = ESP_OK;
            break;
        }

        size_t total = carry + (size_t)read;
        size_t bytes_to_write = total & ~(size_t)1;
        carry = total - bytes_to_write;
        if (carry > 0) {
            buffer[0] = buffer[bytes_to_write];
        }
        if (bytes_to_write == 0) {
            continue;
        }

        err = task_audio_output_write_mono((const int16_t *)buffer, bytes_to_write / sizeof(int16_t), 1000);
        if (err != ESP_OK) {
            break;
        }
    }

    free(buffer);
    task_audio_output_release();
    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    if (stop_requested()) {
        return ESP_OK;
    }
    return err;
}

static void music_player_task(void *arg)
{
    music_player_request_t *request = (music_player_request_t *)arg;
    ornament_settings_t settings;
    resolved_song_t resolved = {0};
    esp_err_t err = ESP_ERR_INVALID_ARG;
    bool requested_stop = false;

    if (settings_load(&settings) != ESP_OK) {
        err = ESP_FAIL;
        goto cleanup;
    }

    if (s_mutex != NULL && xSemaphoreTake(s_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        set_state_locked(MUSIC_PLAYER_STATE_RESOLVING);
        xSemaphoreGive(s_mutex);
    }

    err = resolve_song(&settings, request, &resolved);
    if (err != ESP_OK || stop_requested()) {
        requested_stop = stop_requested();
        goto cleanup;
    }

    if (s_mutex != NULL && xSemaphoreTake(s_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        strlcpy(s_snapshot.title, resolved.title, sizeof(s_snapshot.title));
        strlcpy(s_snapshot.album, resolved.album, sizeof(s_snapshot.album));
        strlcpy(s_snapshot.picture, resolved.picture, sizeof(s_snapshot.picture));
        if (resolved.artist[0] != '\0') {
            strlcpy(s_snapshot.artist_name, resolved.artist, sizeof(s_snapshot.artist_name));
        }
        set_state_locked(MUSIC_PLAYER_STATE_PLAYING);
        xSemaphoreGive(s_mutex);
    }

    err = stream_song(&settings, request);
    requested_stop = stop_requested();

cleanup:
    if (s_mutex != NULL && xSemaphoreTake(s_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        if (err == ESP_OK) {
            mark_idle_locked();
        } else if (requested_stop) {
            mark_idle_locked();
        } else {
            set_error_locked(esp_err_to_name(err));
        }
        s_task = NULL;
        xSemaphoreGive(s_mutex);
    } else {
        s_task = NULL;
    }

    xEventGroupSetBits(s_events, MUSIC_PLAYER_TASK_DONE);
    free(request);
    vTaskDelete(NULL);
}

const char *music_player_state_name(music_player_state_t state)
{
    switch (state) {
    case MUSIC_PLAYER_STATE_RESOLVING:
        return "resolving";
    case MUSIC_PLAYER_STATE_PLAYING:
        return "playing";
    case MUSIC_PLAYER_STATE_STOPPING:
        return "stopping";
    case MUSIC_PLAYER_STATE_ERROR:
        return "error";
    case MUSIC_PLAYER_STATE_IDLE:
    default:
        return "idle";
    }
}

esp_err_t music_player_init(void)
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
    s_snapshot.state = MUSIC_PLAYER_STATE_IDLE;
    xEventGroupSetBits(s_events, MUSIC_PLAYER_TASK_DONE);
    return ESP_OK;
}

esp_err_t music_player_play_song(const char *song_name, const char *artist_name, uint32_t index)
{
    if (song_name == NULL || song_name[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    ESP_RETURN_ON_ERROR(music_player_init(), TAG, "init failed");

    if (music_player_is_active()) {
        ESP_RETURN_ON_ERROR(music_player_stop(), TAG, "stop previous music failed");
    }

    music_player_request_t *request = calloc(1, sizeof(*request));
    if (request == NULL) {
        return ESP_ERR_NO_MEM;
    }
    strlcpy(request->song_name, song_name, sizeof(request->song_name));
    if (artist_name != NULL) {
        strlcpy(request->artist_name, artist_name, sizeof(request->artist_name));
    }
    request->index = index == 0 ? 1 : index;

    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        free(request);
        return ESP_ERR_TIMEOUT;
    }

    s_stop_requested = false;
    s_snapshot.stop_requested = false;
    s_snapshot.index = request->index;
    strlcpy(s_snapshot.song_name, request->song_name, sizeof(s_snapshot.song_name));
    strlcpy(s_snapshot.artist_name, request->artist_name, sizeof(s_snapshot.artist_name));
    clear_metadata_locked();
    set_state_locked(MUSIC_PLAYER_STATE_RESOLVING);
    xEventGroupClearBits(s_events, MUSIC_PLAYER_TASK_DONE);

    BaseType_t created = xTaskCreate(music_player_task, "music_player", MUSIC_PLAYER_TASK_STACK, request, 5, &s_task);
    if (created != pdPASS) {
        set_error_locked("task create failed");
        s_task = NULL;
        xEventGroupSetBits(s_events, MUSIC_PLAYER_TASK_DONE);
        xSemaphoreGive(s_mutex);
        free(request);
        return ESP_ERR_NO_MEM;
    }

    xSemaphoreGive(s_mutex);
    return ESP_OK;
}

void music_player_request_stop(void)
{
    if (s_mutex == NULL) {
        return;
    }
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(20)) != pdTRUE) {
        return;
    }
    if (s_task != NULL) {
        s_stop_requested = true;
        s_snapshot.stop_requested = true;
        set_state_locked(MUSIC_PLAYER_STATE_STOPPING);
    }
    xSemaphoreGive(s_mutex);
}

esp_err_t music_player_stop(void)
{
    if (s_mutex == NULL || s_events == NULL) {
        return ESP_OK;
    }
    music_player_request_stop();
    if (!music_player_is_active()) {
        return ESP_OK;
    }
    EventBits_t bits = xEventGroupWaitBits(s_events, MUSIC_PLAYER_TASK_DONE, pdFALSE, pdTRUE, pdMS_TO_TICKS(5000));
    return (bits & MUSIC_PLAYER_TASK_DONE) != 0 ? ESP_OK : ESP_ERR_TIMEOUT;
}

bool music_player_is_active(void)
{
    if (s_mutex == NULL) {
        return false;
    }
    bool active = false;
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
        active = s_snapshot.active || s_task != NULL;
        xSemaphoreGive(s_mutex);
    }
    return active;
}

void music_player_status_snapshot(music_player_snapshot_t *snapshot)
{
    if (snapshot == NULL) {
        return;
    }
    memset(snapshot, 0, sizeof(*snapshot));
    snapshot->state = MUSIC_PLAYER_STATE_IDLE;
    if (s_mutex == NULL) {
        return;
    }
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
        *snapshot = s_snapshot;
        xSemaphoreGive(s_mutex);
    }
}

#else

const char *music_player_state_name(music_player_state_t state)
{
    (void)state;
    return "disabled";
}

esp_err_t music_player_init(void)
{
    return ESP_OK;
}

esp_err_t music_player_play_song(const char *song_name, const char *artist_name, uint32_t index)
{
    (void)song_name;
    (void)artist_name;
    (void)index;
    return ESP_ERR_NOT_SUPPORTED;
}

void music_player_request_stop(void)
{
}

esp_err_t music_player_stop(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

bool music_player_is_active(void)
{
    return false;
}

void music_player_status_snapshot(music_player_snapshot_t *snapshot)
{
    if (snapshot != NULL) {
        memset(snapshot, 0, sizeof(*snapshot));
        snapshot->state = MUSIC_PLAYER_STATE_IDLE;
    }
}

#endif
