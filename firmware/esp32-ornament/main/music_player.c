#include "music_player.h"

#include "cJSON.h"
#include "device_identity.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mbedtls/sha256.h"
#include "settings.h"
#include "task_audio.h"
#include "xiaozhi_client.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef CONFIG_ORNAMENT_XIAOZHI_MCP_ENABLED
#define CONFIG_ORNAMENT_XIAOZHI_MCP_ENABLED 0
#endif

#ifndef CONFIG_ORNAMENT_MUSIC_REQUEST_TIMEOUT_MS
#define CONFIG_ORNAMENT_MUSIC_REQUEST_TIMEOUT_MS 10000
#endif

#ifndef CONFIG_ORNAMENT_MUSIC_STREAM_CHUNK_BYTES
#define CONFIG_ORNAMENT_MUSIC_STREAM_CHUNK_BYTES 2048
#endif

#ifndef CONFIG_ORNAMENT_MUSIC_MP3_INPUT_BUFFER_BYTES
#define CONFIG_ORNAMENT_MUSIC_MP3_INPUT_BUFFER_BYTES 16384
#endif

#if CONFIG_ORNAMENT_XIAOZHI_ENABLED && CONFIG_ORNAMENT_XIAOZHI_MCP_ENABLED

#define MUSIC_PLAYER_TASK_DONE BIT0
#define MUSIC_PLAYER_URL_MAX 768
#define MUSIC_PLAYER_RESPONSE_MAX 4096
#define MUSIC_PLAYER_TASK_STACK_MAX 14336
#define MUSIC_PLAYER_TASK_STACK_MIN 8192
#define MUSIC_PLAYER_EMPTY_READ_DELAY_MS 20
#define MUSIC_PLAYER_EMPTY_READ_RETRY_MAX 50
#define MUSIC_PLAYER_USER_AGENT "ESP32-Music-Player/1.0"
#define MUSIC_PLAYER_PCM_SAMPLE_BYTES 2

typedef struct {
    char *data;
    int length;
    int capacity;
} music_response_buffer_t;

typedef struct {
    char song_name[ORNAMENT_TEXT_MAX];
    char artist_name[ORNAMENT_TEXT_MAX];
    uint32_t index;
    ornament_settings_t settings;
} music_player_request_t;

typedef struct {
    char title[ORNAMENT_TEXT_MAX];
    char artist[ORNAMENT_TEXT_MAX];
    char album[ORNAMENT_TEXT_MAX];
    char picture[ORNAMENT_BRIDGE_URL_MAX];
    char audio_url[MUSIC_PLAYER_URL_MAX];
} resolved_song_t;

static const char *TAG = "music_player";

static SemaphoreHandle_t s_mutex;
static EventGroupHandle_t s_events;
static TaskHandle_t s_task;
static music_player_snapshot_t s_snapshot;
static bool s_stop_requested;
static size_t s_task_stack_bytes;
static bool s_task_stack_in_spiram;

static size_t current_stack_high_water_bytes(void)
{
    return uxTaskGetStackHighWaterMark(NULL) * sizeof(StackType_t);
}

static void log_music_heap_status(const char *stage)
{
    ESP_LOGI(
        TAG,
        "heap %s: free=%u largest8=%u largest_internal=%u internal=%u spiram=%u",
        stage,
        (unsigned int)esp_get_free_heap_size(),
        (unsigned int)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT),
        (unsigned int)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
        (unsigned int)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
        (unsigned int)heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
}

static void format_hex_preview(const uint8_t *data, size_t size, char *target, size_t target_size)
{
    if (target == NULL || target_size == 0) {
        return;
    }
    target[0] = '\0';
    if (data == NULL || size == 0) {
        return;
    }

    size_t used = 0;
    size_t limit = size < 16 ? size : 16;
    for (size_t i = 0; i < limit && used < target_size; i++) {
        int written = snprintf(
            target + used,
            target_size - used,
            "%s%02X",
            i == 0 ? "" : " ",
            data[i]);
        if (written <= 0 || written >= (int)(target_size - used)) {
            break;
        }
        used += (size_t)written;
    }
}

static void *music_calloc(size_t count, size_t size)
{
    return heap_caps_calloc(count, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

static size_t select_music_task_stack_bytes(uint32_t caps)
{
    static const size_t candidates[] = {
        MUSIC_PLAYER_TASK_STACK_MAX,
        12288,
        10240,
        MUSIC_PLAYER_TASK_STACK_MIN,
    };
    size_t largest = heap_caps_get_largest_free_block(caps);

    for (size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); i++) {
        size_t stack_bytes = candidates[i];
        if (largest >= stack_bytes) {
            return stack_bytes;
        }
    }

    return MUSIC_PLAYER_TASK_STACK_MIN;
}

static bool xiaozhi_runtime_active_for_music(const xiaozhi_client_snapshot_t *snapshot)
{
    if (snapshot == NULL) {
        return false;
    }

    return snapshot->connected ||
           snapshot->session_requested ||
           snapshot->state == XIAOZHI_CLIENT_STATE_CONNECTING ||
           snapshot->state == XIAOZHI_CLIENT_STATE_LISTENING ||
           snapshot->state == XIAOZHI_CLIENT_STATE_SPEAKING;
}

static void release_xiaozhi_session_for_music(void)
{
#if CONFIG_ORNAMENT_XIAOZHI_ENABLED
    xiaozhi_client_snapshot_t snapshot = {0};
    xiaozhi_client_status_snapshot(&snapshot);
    if (!xiaozhi_runtime_active_for_music(&snapshot)) {
        return;
    }

    ESP_LOGI(
        TAG,
        "stopping Xiaozhi session before music: state=%s requested=%d connected=%d",
        xiaozhi_client_state_name(snapshot.state),
        snapshot.session_requested,
        snapshot.connected);
    esp_err_t stop_err = xiaozhi_client_stop_session();
    if (stop_err != ESP_OK) {
        ESP_LOGW(TAG, "stop Xiaozhi before music failed: %s", esp_err_to_name(stop_err));
    } else {
        log_music_heap_status("after_xiaozhi_stop");
    }
#endif
}

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

static bool is_debug_test_audio_url(const char *audio_url)
{
    if (audio_url == NULL) {
        return false;
    }
    const char *test_path = strstr(audio_url, "/test.mp3");
    if (test_path == NULL) {
        return false;
    }
    return strstr(audio_url, ":2233/test.mp3") != NULL ||
           strcmp(test_path, "/test.mp3") == 0;
}

static bool is_mp3_audio_url(const char *audio_url)
{
    if (audio_url == NULL) {
        return false;
    }

    const char *query = strchr(audio_url, '?');
    const char *mp3 = strstr(audio_url, ".mp3");
    return mp3 != NULL && (query == NULL || mp3 < query);
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
    const char *song_name,
    const char *artist_name,
    char *target,
    size_t target_size)
{
    if (base_url == NULL || song_name == NULL || song_name[0] == '\0' || target == NULL || target_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    size_t base_len = strnlen(base_url, target_size);
    while (base_len > 0 && base_url[base_len - 1] == '/') {
        base_len--;
    }

    bool has_query = memchr(base_url, '?', base_len) != NULL;
    bool endpoint_url =
        has_query ||
        strstr(base_url, "/stream_pcm") != NULL ||
        strstr(base_url, "/v1/music/resolve") != NULL;
    int written = endpoint_url ?
        snprintf(target, target_size, "%.*s%csong=", (int)base_len, base_url, has_query ? '&' : '?') :
        snprintf(target, target_size, "%.*s/stream_pcm?song=", (int)base_len, base_url);
    if (written <= 0 || written >= (int)target_size) {
        return ESP_ERR_NO_MEM;
    }
    size_t used = (size_t)written;
    append_urlencoded(target, target_size, &used, song_name);

    if (artist_name != NULL) {
        const char *suffix = "&artist=";
        if (used + strlen(suffix) >= target_size) {
            return ESP_ERR_NO_MEM;
        }
        memcpy(target + used, suffix, strlen(suffix));
        used += strlen(suffix);
        target[used] = '\0';
        append_urlencoded(target, target_size, &used, artist_name);
    }

    return ESP_OK;
}

static void get_device_mac_string(char *target, size_t target_size)
{
    uint8_t mac[6] = {0};
    if (target == NULL || target_size == 0) {
        return;
    }
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

static void get_device_chip_id_string(char *target, size_t target_size)
{
    uint8_t mac[6] = {0};
    if (target == NULL || target_size == 0) {
        return;
    }
    if (esp_read_mac(mac, ESP_MAC_WIFI_STA) == ESP_OK) {
        snprintf(
            target,
            target_size,
            "%02X%02X%02X%02X%02X%02X",
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

static esp_err_t apply_music_auth_headers(esp_http_client_handle_t client, const ornament_settings_t *settings)
{
    if (client == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    char mac[32] = {0};
    char chip_id[32] = {0};
    char timestamp[24] = {0};
    char dynamic_key[65] = {0};
    unsigned char hash[32] = {0};
    char data[256] = {0};

    get_device_mac_string(mac, sizeof(mac));
    get_device_chip_id_string(chip_id, sizeof(chip_id));
    int64_t timestamp_sec = esp_timer_get_time() / 1000000;
    snprintf(timestamp, sizeof(timestamp), "%lld", (long long)timestamp_sec);

    const char *secret = settings_music_auth_secret_or_default(settings);
    int written = snprintf(data, sizeof(data), "%s:%s:%s:%s", mac, chip_id, timestamp, secret != NULL ? secret : "");
    if (written <= 0 || written >= (int)sizeof(data)) {
        return ESP_ERR_NO_MEM;
    }

#if MBEDTLS_VERSION_MAJOR >= 3
    int sha_err = mbedtls_sha256((const unsigned char *)data, strlen(data), hash, 0);
    if (sha_err != 0) {
        return ESP_FAIL;
    }
#else
    mbedtls_sha256((const unsigned char *)data, strlen(data), hash, 0);
#endif

    for (size_t i = 0; i < 16; i++) {
        snprintf(dynamic_key + i * 2, sizeof(dynamic_key) - i * 2, "%02X", hash[i]);
    }

    ESP_RETURN_ON_ERROR(esp_http_client_set_header(client, "User-Agent", MUSIC_PLAYER_USER_AGENT), TAG, "set user-agent failed");
    ESP_RETURN_ON_ERROR(esp_http_client_set_header(client, "X-MAC-Address", mac), TAG, "set mac header failed");
    ESP_RETURN_ON_ERROR(esp_http_client_set_header(client, "X-Chip-ID", chip_id), TAG, "set chip header failed");
    ESP_RETURN_ON_ERROR(esp_http_client_set_header(client, "X-Timestamp", timestamp), TAG, "set timestamp header failed");
    ESP_RETURN_ON_ERROR(esp_http_client_set_header(client, "X-Dynamic-Key", dynamic_key), TAG, "set dynamic key header failed");
    return ESP_OK;
}

static esp_err_t resolve_song(
    const ornament_settings_t *settings,
    const music_player_request_t *request,
    resolved_song_t *resolved)
{
    if (settings == NULL || request == NULL || resolved == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    const char *base_url = settings_music_service_base_url_or_default(settings);
    if (base_url == NULL || base_url[0] == '\0') {
        return ESP_ERR_INVALID_STATE;
    }

    char *url = heap_caps_calloc(MUSIC_PLAYER_URL_MAX, 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (url == NULL) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = build_music_request_url(
        base_url,
        request->song_name,
        request->artist_name,
        url,
        MUSIC_PLAYER_URL_MAX);
    if (err != ESP_OK) {
        free(url);
        ESP_LOGW(TAG, "resolve url build failed: %s", esp_err_to_name(err));
        return err;
    }

    char *response = heap_caps_calloc(MUSIC_PLAYER_RESPONSE_MAX, 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (response == NULL) {
        ESP_LOGW(TAG, "resolve response alloc failed");
        log_music_heap_status("resolve_alloc_fail");
        free(url);
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
        .timeout_ms = CONFIG_ORNAMENT_MUSIC_REQUEST_TIMEOUT_MS,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        free(url);
        free(response);
        return ESP_FAIL;
    }

    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_http_client_set_header(client, "Accept", "application/json"));
    ESP_ERROR_CHECK_WITHOUT_ABORT(apply_music_auth_headers(client, settings));

    ESP_LOGI(TAG, "resolve request: %s", url);
    err = esp_http_client_perform(client);
    int status_code = esp_http_client_get_status_code(client);
    ESP_LOGI(
        TAG,
        "resolve response: err=%s status=%d bytes=%d",
        esp_err_to_name(err),
        status_code,
        buffer.length);
    esp_http_client_cleanup(client);
    free(url);
    if (err != ESP_OK) {
        free(response);
        return err;
    }
    if (status_code != 200) {
        ESP_LOGW(TAG, "resolve rejected: status=%d body=%.*s", status_code, 160, response);
        free(response);
        return ESP_ERR_HTTP_FETCH_HEADER;
    }

    cJSON *root = cJSON_Parse(response);
    free(response);
    if (root == NULL) {
        ESP_LOGW(TAG, "resolve JSON parse failed");
        return ESP_ERR_INVALID_RESPONSE;
    }

    resolved_song_t parsed = {0};
    char error_text[ORNAMENT_TEXT_MAX] = {0};
    copy_json_string(root, "title", parsed.title, sizeof(parsed.title));
    copy_json_string(root, "artist", parsed.artist, sizeof(parsed.artist));
    copy_json_string(root, "album", parsed.album, sizeof(parsed.album));
    copy_json_string(root, "picture", parsed.picture, sizeof(parsed.picture));
    copy_json_string(root, "audio_url", parsed.audio_url, sizeof(parsed.audio_url));
    if (parsed.audio_url[0] == '\0') {
        copy_json_string(root, "url", parsed.audio_url, sizeof(parsed.audio_url));
    }
    cJSON *error = cJSON_GetObjectItemCaseSensitive(root, "error");
    if (cJSON_IsString(error) && error->valuestring != NULL) {
        strlcpy(error_text, error->valuestring, sizeof(error_text));
    }
    cJSON_Delete(root);

    if (parsed.audio_url[0] == '\0') {
        if (error_text[0] != '\0') {
            ESP_LOGW(TAG, "music resolve rejected: %s", error_text);
        } else {
            ESP_LOGW(TAG, "music resolve missing audio_url");
        }
        return ESP_FAIL;
    }
    if (is_debug_test_audio_url(parsed.audio_url)) {
        ESP_LOGW(TAG, "music resolve returned debug MP3 test audio, refusing playback: %s", parsed.audio_url);
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (is_mp3_audio_url(parsed.audio_url)) {
        ESP_LOGW(TAG, "music resolve returned MP3 URL, refusing unsafe ESP32-side decode: %s", parsed.audio_url);
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (parsed.title[0] == '\0') {
        strlcpy(parsed.title, request->song_name, sizeof(parsed.title));
    }
    if (parsed.artist[0] == '\0') {
        strlcpy(parsed.artist, request->artist_name, sizeof(parsed.artist));
    }
    ESP_LOGI(TAG, "resolved song: title=%s artist=%s audio_url=%s", parsed.title, parsed.artist, parsed.audio_url);
    *resolved = parsed;
    return ESP_OK;
}

static bool content_type_is_rejected(const char *content_type)
{
    if (content_type == NULL || content_type[0] == '\0') {
        return false;
    }

    return strcasestr(content_type, "audio/mpeg") != NULL ||
           strcasestr(content_type, "audio/mp3") != NULL ||
           strcasestr(content_type, "application/json") != NULL ||
           strcasestr(content_type, "text/") != NULL;
}

static bool buffer_looks_like_non_pcm(const uint8_t *data, size_t size)
{
    if (data == NULL || size < 4) {
        return false;
    }

    if (memcmp(data, "ID3", 3) == 0 ||
        memcmp(data, "RIFF", 4) == 0 ||
        memcmp(data, "{", 1) == 0 ||
        memcmp(data, "<", 1) == 0) {
        return true;
    }

    return data[0] == 0xff && (data[1] & 0xe0) == 0xe0;
}

static esp_err_t stream_song(
    const ornament_settings_t *settings,
    const char *audio_url)
{
    if (settings == NULL || audio_url == NULL || audio_url[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    esp_http_client_config_t config = {
        .url = audio_url,
        .timeout_ms = CONFIG_ORNAMENT_MUSIC_REQUEST_TIMEOUT_MS,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        return ESP_FAIL;
    }

    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_http_client_set_header(client, "Accept", "*/*"));
    ESP_ERROR_CHECK_WITHOUT_ABORT(apply_music_auth_headers(client, settings));

    ESP_LOGI(TAG, "stream request: %s", audio_url);
    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "stream open failed: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        return err;
    }

    (void)esp_http_client_fetch_headers(client);
    int status_code = esp_http_client_get_status_code(client);
    int64_t content_length = esp_http_client_get_content_length(client);
    char *content_type = NULL;
    (void)esp_http_client_get_header(client, "Content-Type", &content_type);
    ESP_LOGI(
        TAG,
        "stream headers: status=%d content_length=%lld content_type=%s",
        status_code,
        (long long)content_length,
        content_type != NULL ? content_type : "<none>");
    if (status_code < 200 || status_code >= 300) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_ERR_HTTP_FETCH_HEADER;
    }
    if (content_type_is_rejected(content_type)) {
        ESP_LOGW(TAG, "stream content type is not raw PCM: %s", content_type);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_ERR_NOT_SUPPORTED;
    }

    uint8_t *read_buffer = heap_caps_calloc(CONFIG_ORNAMENT_MUSIC_STREAM_CHUNK_BYTES, 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (read_buffer == NULL) {
        ESP_LOGW(
            TAG,
            "stream buffer alloc failed read=%p",
            (void *)read_buffer);
        log_music_heap_status("stream_alloc_fail");
        free(read_buffer);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_ERR_NO_MEM;
    }

    size_t total_read = 0;
    size_t total_frames = 0;
    size_t empty_reads = 0;
    bool first_chunk_logged = false;
    bool output_acquired = false;
    err = ESP_OK;

    while (!stop_requested()) {
        int read = esp_http_client_read(
            client,
            (char *)read_buffer,
            CONFIG_ORNAMENT_MUSIC_STREAM_CHUNK_BYTES);
        if (read == -ESP_ERR_HTTP_EAGAIN) {
            continue;
        }
        if (read < 0) {
            err = ESP_FAIL;
            break;
        }
        if (read == 0) {
            if (esp_http_client_is_complete_data_received(client)) {
                err = ESP_OK;
                break;
            }
            empty_reads++;
            if (empty_reads == 1 || empty_reads % 10 == 0) {
                ESP_LOGW(
                    TAG,
                    "stream empty read before complete: total_read=%u content_length=%lld retries=%u",
                    (unsigned int)total_read,
                    (long long)content_length,
                    (unsigned int)empty_reads);
            }
            if (empty_reads >= MUSIC_PLAYER_EMPTY_READ_RETRY_MAX) {
                err = ESP_ERR_TIMEOUT;
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(MUSIC_PLAYER_EMPTY_READ_DELAY_MS));
            continue;
        }

        empty_reads = 0;
        if (!first_chunk_logged) {
            char hex[64];
            format_hex_preview(read_buffer, (size_t)read, hex, sizeof(hex));
            ESP_LOGI(TAG, "stream first chunk: bytes=%d head=%s", read, hex);
            if (buffer_looks_like_non_pcm(read_buffer, (size_t)read)) {
                ESP_LOGW(TAG, "stream body is not raw PCM, refusing playback");
                err = ESP_ERR_NOT_SUPPORTED;
                break;
            }
            first_chunk_logged = true;
            err = task_audio_output_acquire_with_volume(settings);
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "audio output acquire failed: %s", esp_err_to_name(err));
                break;
            }
            output_acquired = true;
        }

        total_read += (size_t)read;
        size_t frames = (size_t)read / MUSIC_PLAYER_PCM_SAMPLE_BYTES;
        if (frames == 0) {
            continue;
        }
        err = task_audio_output_write_mono((const int16_t *)read_buffer, frames, 1000);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "audio write failed: %s", esp_err_to_name(err));
            break;
        }
        total_frames += frames;
    }

    ESP_LOGI(
        TAG,
        "stream finished: err=%s stop=%d total_read=%u pcm_frames=%u",
        esp_err_to_name(err),
        stop_requested(),
        (unsigned int)total_read,
        (unsigned int)total_frames);

    free(read_buffer);
    if (output_acquired) {
        (void)task_audio_output_write_silence(ORNAMENT_AUDIO_SAMPLE_RATE_HZ / 20, 1000);
        task_audio_output_release();
    }
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
    resolved_song_t *resolved = music_calloc(1, sizeof(*resolved));
    esp_err_t err = ESP_ERR_INVALID_ARG;
    bool requested_stop = false;

    ESP_LOGI(TAG, "music task entry: stack_hwm=%u bytes", (unsigned int)current_stack_high_water_bytes());
    if (resolved == NULL) {
        ESP_LOGW(TAG, "resolved song alloc failed: bytes=%u", (unsigned int)sizeof(*resolved));
        log_music_heap_status("resolved_alloc_fail");
        err = ESP_ERR_NO_MEM;
        goto cleanup;
    }
    if (s_mutex != NULL && xSemaphoreTake(s_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        set_state_locked(MUSIC_PLAYER_STATE_RESOLVING);
        xSemaphoreGive(s_mutex);
    }

    err = resolve_song(&request->settings, request, resolved);
    ESP_LOGI(
        TAG,
        "music task after resolve: err=%s stack_hwm=%u bytes",
        esp_err_to_name(err),
        (unsigned int)current_stack_high_water_bytes());
    if (err != ESP_OK || stop_requested()) {
        requested_stop = stop_requested();
        goto cleanup;
    }

    release_xiaozhi_session_for_music();

    if (s_mutex != NULL && xSemaphoreTake(s_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        strlcpy(s_snapshot.title, resolved->title, sizeof(s_snapshot.title));
        strlcpy(s_snapshot.album, resolved->album, sizeof(s_snapshot.album));
        strlcpy(s_snapshot.picture, resolved->picture, sizeof(s_snapshot.picture));
        if (resolved->artist[0] != '\0') {
            strlcpy(s_snapshot.artist_name, resolved->artist, sizeof(s_snapshot.artist_name));
        }
        set_state_locked(MUSIC_PLAYER_STATE_PLAYING);
        xSemaphoreGive(s_mutex);
    }

    char *audio_url = music_calloc(MUSIC_PLAYER_URL_MAX, 1);
    if (audio_url == NULL) {
        ESP_LOGW(TAG, "stream url copy alloc failed: bytes=%u", (unsigned int)MUSIC_PLAYER_URL_MAX);
        log_music_heap_status("stream_url_alloc_fail");
        err = ESP_ERR_NO_MEM;
        goto cleanup;
    }
    strlcpy(audio_url, resolved->audio_url, MUSIC_PLAYER_URL_MAX);
    free(resolved);
    resolved = NULL;

    err = stream_song(&request->settings, audio_url);
    free(audio_url);
    requested_stop = stop_requested();

cleanup:
    ESP_LOGI(
        TAG,
        "music task cleanup: err=%s stop=%d stack_hwm=%u bytes",
        esp_err_to_name(err),
        requested_stop,
        (unsigned int)current_stack_high_water_bytes());
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
    free(resolved);
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

static esp_err_t start_music_request(
    const char *song_name,
    const char *artist_name,
    uint32_t index,
    const ornament_settings_t *settings)
{
    (void)index;
    if (song_name == NULL || song_name[0] == '\0' || settings == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    ESP_RETURN_ON_ERROR(music_player_init(), TAG, "init failed");

    if (music_player_is_active()) {
        ESP_RETURN_ON_ERROR(music_player_stop(), TAG, "stop previous music failed");
    }

    music_player_request_t *request = music_calloc(1, sizeof(*request));
    if (request == NULL) {
        log_music_heap_status("request_alloc_fail");
        return ESP_ERR_NO_MEM;
    }
    strlcpy(request->song_name, song_name, sizeof(request->song_name));
    if (artist_name != NULL) {
        strlcpy(request->artist_name, artist_name, sizeof(request->artist_name));
    }
    request->index = index == 0 ? 1 : index;
    request->settings = *settings;

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

    log_music_heap_status("before_music_task");
    s_task_stack_in_spiram = true;
    s_task_stack_bytes = select_music_task_stack_bytes(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    ESP_LOGI(
        TAG,
        "music task stack select: largest_spiram=%u largest_internal=%u selected_spiram=%u",
        (unsigned int)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT),
        (unsigned int)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
        (unsigned int)s_task_stack_bytes);
    BaseType_t created = xTaskCreateWithCaps(
        music_player_task,
        "music_player",
        s_task_stack_bytes,
        request,
        5,
        &s_task,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (created != pdPASS) {
        ESP_LOGW(
            TAG,
            "music task PSRAM stack create failed: stack=%u largest_spiram=%u largest_internal=%u",
            (unsigned int)s_task_stack_bytes,
            (unsigned int)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT),
            (unsigned int)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
        s_task_stack_in_spiram = false;
        s_task_stack_bytes = select_music_task_stack_bytes(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        created = xTaskCreateWithCaps(
            music_player_task,
            "music_player",
            s_task_stack_bytes,
            request,
            5,
            &s_task,
            MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    if (created != pdPASS) {
        set_error_locked("task create failed");
        s_task = NULL;
        xEventGroupSetBits(s_events, MUSIC_PLAYER_TASK_DONE);
        xSemaphoreGive(s_mutex);
        free(request);
        ESP_LOGW(
            TAG,
            "music task create failed stack=%u caps=%s",
            (unsigned int)s_task_stack_bytes,
            s_task_stack_in_spiram ? "spiram" : "internal");
        log_music_heap_status("music_task_create_fail");
        return ESP_ERR_NO_MEM;
    }

    xSemaphoreGive(s_mutex);
    ESP_LOGI(
        TAG,
        "music task started stack=%u caps=%s",
        (unsigned int)s_task_stack_bytes,
        s_task_stack_in_spiram ? "spiram" : "internal");
    log_music_heap_status("after_music_task");
    return ESP_OK;
}

esp_err_t music_player_play_song(const char *song_name, const char *artist_name, uint32_t index)
{
    ornament_settings_t settings;
    esp_err_t settings_err = settings_load(&settings);
    if (settings_err != ESP_OK) {
        ESP_LOGW(TAG, "settings load failed before music task: %s", esp_err_to_name(settings_err));
        return settings_err;
    }

    return start_music_request(song_name, artist_name, index, &settings);
}

esp_err_t music_player_play_song_with_settings(
    const char *song_name,
    const char *artist_name,
    uint32_t index,
    const ornament_settings_t *settings)
{
    return start_music_request(song_name, artist_name, index, settings);
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
