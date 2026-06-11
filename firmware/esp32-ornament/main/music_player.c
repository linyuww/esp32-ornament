#include "music_player.h"

#include "cJSON.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "ornament_http_client.h"
#include "settings.h"
#include "task_audio.h"
#include "xiaozhi_client.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef CONFIG_ORNAMENT_MUSIC_PLAYER_ENABLED
#define CONFIG_ORNAMENT_MUSIC_PLAYER_ENABLED 0
#endif

#ifndef CONFIG_ORNAMENT_MUSIC_REQUEST_TIMEOUT_MS
#define CONFIG_ORNAMENT_MUSIC_REQUEST_TIMEOUT_MS 10000
#endif

#ifndef CONFIG_ORNAMENT_MUSIC_STREAM_CHUNK_BYTES
#define CONFIG_ORNAMENT_MUSIC_STREAM_CHUNK_BYTES 2048
#endif

#ifndef CONFIG_ORNAMENT_MUSIC_COVER_ENABLED
#define CONFIG_ORNAMENT_MUSIC_COVER_ENABLED 0
#endif

#if CONFIG_ORNAMENT_MUSIC_PLAYER_ENABLED

#define MUSIC_PLAYER_TASK_DONE BIT0
#define MUSIC_PLAYER_URL_MAX 768
#define MUSIC_PLAYER_RESPONSE_MAX 4096
#if CONFIG_ORNAMENT_MUSIC_COVER_ENABLED
#define MUSIC_PLAYER_COVER_BYTES (MUSIC_PLAYER_COVER_PIXELS * sizeof(uint16_t))
#endif
#define MUSIC_PLAYER_TASK_STACK_MAX 14336
#define MUSIC_PLAYER_TASK_STACK_MIN 8192
#define MUSIC_PLAYER_EMPTY_READ_DELAY_MS 20
#define MUSIC_PLAYER_EMPTY_READ_RETRY_MAX 50
#define MUSIC_PLAYER_STREAM_READ_TIMEOUT_MS 250
#define MUSIC_PLAYER_USER_AGENT "ESP32-Music-Player/1.0"
#define MUSIC_PLAYER_PCM_SAMPLE_BYTES 2

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
    char cover_url[ORNAMENT_BRIDGE_URL_MAX];
    char lyrics[MUSIC_PLAYER_LYRICS_MAX];
} resolved_song_t;

static const char *TAG = "music_player";

static SemaphoreHandle_t s_mutex;
static EventGroupHandle_t s_events;
static TaskHandle_t s_task;
static music_player_snapshot_t s_snapshot;
#if CONFIG_ORNAMENT_MUSIC_COVER_ENABLED
static uint16_t *s_cover_pixels;
#endif
static bool s_stop_requested;
static size_t s_task_stack_bytes;
static bool s_task_stack_in_spiram;

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
    s_snapshot.cover_url[0] = '\0';
    s_snapshot.lyrics[0] = '\0';
    s_snapshot.has_cover = false;
    s_snapshot.last_error[0] = '\0';
#if CONFIG_ORNAMENT_MUSIC_COVER_ENABLED
    s_snapshot.cover_pixels = s_cover_pixels;
    if (s_cover_pixels != NULL) {
        memset(s_cover_pixels, 0, MUSIC_PLAYER_COVER_BYTES);
    }
#endif
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

static void update_playback_ms(size_t total_frames)
{
    if (s_mutex == NULL) {
        return;
    }
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(5)) == pdTRUE) {
        s_snapshot.playback_ms = (uint32_t)((total_frames * 1000ULL) / ORNAMENT_AUDIO_SAMPLE_RATE_HZ);
        xSemaphoreGive(s_mutex);
    }
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
    if (base_url == NULL || endpoint == NULL || song_name == NULL || song_name[0] == '\0' ||
        target == NULL || target_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    size_t base_len = strnlen(base_url, target_size);
    while (base_len > 0 && base_url[base_len - 1] == '/') {
        base_len--;
    }

    int written = snprintf(target, target_size, "%.*s/%s?song=", (int)base_len, base_url, endpoint);
    if (written <= 0 || written >= (int)target_size) {
        return ESP_ERR_NO_MEM;
    }
    size_t used = (size_t)written;
    append_urlencoded(target, target_size, &used, song_name);

    if (artist_name != NULL && artist_name[0] != '\0') {
        const char *suffix = "&artist=";
        if (used + strlen(suffix) >= target_size) {
            return ESP_ERR_NO_MEM;
        }
        memcpy(target + used, suffix, strlen(suffix));
        used += strlen(suffix);
        target[used] = '\0';
        append_urlencoded(target, target_size, &used, artist_name);
    }

    written = snprintf(target + used, target_size - used, "&index=%lu", (unsigned long)(index == 0 ? 1UL : index));
    if (written <= 0 || written >= (int)(target_size - used)) {
        return ESP_ERR_NO_MEM;
    }
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

    char base_url[ORNAMENT_MUSIC_BASE_URL_MAX] = {0};
    ESP_RETURN_ON_ERROR(
        settings_resolve_bridge_music_base_url(settings, base_url, sizeof(base_url)),
        TAG,
        "music base url resolve failed");
    char *url = heap_caps_calloc(MUSIC_PLAYER_URL_MAX, 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (url == NULL) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = build_music_request_url(
        base_url,
        "resolve",
        request->song_name,
        request->artist_name[0] != '\0' ? request->artist_name : NULL,
        request->index,
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
        free(url);
        return ESP_ERR_NO_MEM;
    }

    size_t response_len = 0;
    int status_code = -1;
    ornament_http_request_t http_request = {
        .method = "GET",
        .url = url,
        .accept = "application/json",
        .user_agent = MUSIC_PLAYER_USER_AGENT,
        .response = response,
        .response_capacity = MUSIC_PLAYER_RESPONSE_MAX,
        .response_len = &response_len,
        .status_code = &status_code,
        .timeout_ms = CONFIG_ORNAMENT_MUSIC_REQUEST_TIMEOUT_MS,
    };
    err = ornament_http_request(&http_request);
    free(url);
    if (err != ESP_OK) {
        free(response);
        return err;
    }
    if (status_code != 200) {
        ESP_LOGW(TAG, "resolve rejected: status=%d body=%.*s", status_code, 160, response);
        free(response);
        return ESP_ERR_INVALID_RESPONSE;
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
    copy_json_string(root, "coverUrl", parsed.cover_url, sizeof(parsed.cover_url));
    copy_json_string(root, "lyrics", parsed.lyrics, sizeof(parsed.lyrics));
    cJSON *ok = cJSON_GetObjectItemCaseSensitive(root, "ok");
    cJSON *error = cJSON_GetObjectItemCaseSensitive(root, "error");
    if (cJSON_IsString(error) && error->valuestring != NULL) {
        strlcpy(error_text, error->valuestring, sizeof(error_text));
    }
    cJSON_Delete(root);

    if (ok != NULL && cJSON_IsBool(ok) && !cJSON_IsTrue(ok)) {
        ESP_LOGW(TAG, "music resolve rejected: %s", error_text[0] != '\0' ? error_text : "ok=false");
        return ESP_FAIL;
    }
    if (parsed.title[0] == '\0') {
        strlcpy(parsed.title, request->song_name, sizeof(parsed.title));
    }
    if (parsed.artist[0] == '\0') {
        strlcpy(parsed.artist, request->artist_name, sizeof(parsed.artist));
    }
    ESP_LOGI(TAG, "resolved song: title=%s artist=%s", parsed.title, parsed.artist);
    *resolved = parsed;
    return ESP_OK;
}

#if CONFIG_ORNAMENT_MUSIC_COVER_ENABLED
static esp_err_t fetch_cover_pixels(const char *cover_url, uint16_t *pixels, size_t pixel_count)
{
    if (cover_url == NULL || cover_url[0] == '\0' || pixels == NULL ||
        pixel_count != MUSIC_PLAYER_COVER_PIXELS) {
        return ESP_ERR_INVALID_ARG;
    }

    ornament_http_request_t request = {
        .method = "GET",
        .url = cover_url,
        .accept = "application/octet-stream",
        .user_agent = MUSIC_PLAYER_USER_AGENT,
        .timeout_ms = CONFIG_ORNAMENT_MUSIC_REQUEST_TIMEOUT_MS,
    };
    ornament_http_stream_t *stream = NULL;
    esp_err_t err = ornament_http_open_stream(&request, &stream);
    if (err != ESP_OK) {
        return err;
    }

    int status_code = ornament_http_stream_status_code(stream);
    if (status_code != 200) {
        ESP_LOGW(TAG, "cover rejected: status=%d", status_code);
        ornament_http_stream_close(stream);
        return ESP_ERR_INVALID_RESPONSE;
    }

    uint8_t *target = (uint8_t *)pixels;
    size_t total = 0;
    while (total < MUSIC_PLAYER_COVER_BYTES && !stop_requested()) {
        size_t read = 0;
        err = ornament_http_stream_read(stream, target + total, MUSIC_PLAYER_COVER_BYTES - total, &read);
        if (err == ESP_ERR_TIMEOUT) {
            continue;
        }
        if (err != ESP_OK) {
            break;
        }
        if (read == 0) {
            if (ornament_http_stream_is_complete(stream)) {
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(MUSIC_PLAYER_EMPTY_READ_DELAY_MS));
            continue;
        }
        total += (size_t)read;
    }

    ornament_http_stream_close(stream);

    if (err != ESP_OK) {
        return err;
    }
    if (total != MUSIC_PLAYER_COVER_BYTES) {
        ESP_LOGW(TAG, "cover size mismatch: got=%u expected=%u", (unsigned int)total, (unsigned int)MUSIC_PLAYER_COVER_BYTES);
        return ESP_ERR_INVALID_SIZE;
    }
    return ESP_OK;
}
#endif

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
    const music_player_request_t *request)
{
    if (settings == NULL || request == NULL || request->song_name[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    char base_url[ORNAMENT_MUSIC_BASE_URL_MAX] = {0};
    ESP_RETURN_ON_ERROR(
        settings_resolve_bridge_music_base_url(settings, base_url, sizeof(base_url)),
        TAG,
        "music base url resolve failed");

    char *url = heap_caps_calloc(MUSIC_PLAYER_URL_MAX, 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (url == NULL) {
        return ESP_ERR_NO_MEM;
    }
    esp_err_t err = build_music_request_url(
        base_url,
        "stream",
        request->song_name,
        request->artist_name[0] != '\0' ? request->artist_name : NULL,
        request->index,
        url,
        MUSIC_PLAYER_URL_MAX);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "stream url build failed: %s", esp_err_to_name(err));
        free(url);
        return err;
    }

    ornament_http_request_t http_request = {
        .method = "GET",
        .url = url,
        .accept = "audio/L16",
        .user_agent = MUSIC_PLAYER_USER_AGENT,
        .timeout_ms = CONFIG_ORNAMENT_MUSIC_REQUEST_TIMEOUT_MS,
    };
    ornament_http_stream_t *stream = NULL;
    err = ornament_http_open_stream(&http_request, &stream);
    free(url);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "stream open failed: %s", esp_err_to_name(err));
        return err;
    }

    int status_code = ornament_http_stream_status_code(stream);
    int64_t content_length = ornament_http_stream_content_length(stream);
    const char *content_type = ornament_http_stream_content_type(stream);
    if (status_code < 200 || status_code >= 300) {
        ornament_http_stream_close(stream);
        return ESP_ERR_INVALID_RESPONSE;
    }
    if (content_type_is_rejected(content_type)) {
        ESP_LOGW(TAG, "stream content type is not raw PCM: %s", content_type);
        ornament_http_stream_close(stream);
        return ESP_ERR_NOT_SUPPORTED;
    }
    (void)ornament_http_stream_set_timeout(stream, MUSIC_PLAYER_STREAM_READ_TIMEOUT_MS);

    uint8_t *read_buffer = heap_caps_calloc(CONFIG_ORNAMENT_MUSIC_STREAM_CHUNK_BYTES + 1, 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (read_buffer == NULL) {
        ESP_LOGW(
            TAG,
            "stream buffer alloc failed read=%p",
            (void *)read_buffer);
        free(read_buffer);
        ornament_http_stream_close(stream);
        return ESP_ERR_NO_MEM;
    }

    size_t total_read = 0;
    size_t total_frames = 0;
    size_t empty_reads = 0;
    size_t carry = 0;
    bool first_chunk_logged = false;
    bool output_acquired = false;
    err = ESP_OK;

    while (!stop_requested()) {
        size_t read = 0;
        err = ornament_http_stream_read(
            stream,
            read_buffer + carry,
            CONFIG_ORNAMENT_MUSIC_STREAM_CHUNK_BYTES - carry,
            &read);
        if (err == ESP_ERR_TIMEOUT) {
            continue;
        }
        if (err != ESP_OK) {
            break;
        }
        if (read == 0) {
            if (ornament_http_stream_is_complete(stream)) {
                err = ESP_OK;
                break;
            }
            empty_reads++;
            if (empty_reads == 1 || empty_reads % 10 == 0) {
                unsigned int content_length_log =
                    content_length >= 0 && content_length <= UINT32_MAX ? (unsigned int)content_length : 0;
                ESP_LOGW(
                    TAG,
                    "stream empty read before complete: total_read=%u content_length=%u retries=%u",
                    (unsigned int)total_read,
                    content_length_log,
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
            size_t preview_bytes = carry + read;
            if (buffer_looks_like_non_pcm(read_buffer, preview_bytes)) {
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

        size_t total = carry + read;
        size_t bytes_to_write = total & ~(size_t)(MUSIC_PLAYER_PCM_SAMPLE_BYTES - 1);
        size_t next_carry = total - bytes_to_write;
        total_read += read;
        if (bytes_to_write == 0) {
            carry = next_carry;
            continue;
        }
        size_t frames = bytes_to_write / MUSIC_PLAYER_PCM_SAMPLE_BYTES;
        err = task_audio_output_write_mono((const int16_t *)read_buffer, frames, 1000);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "audio write failed: %s", esp_err_to_name(err));
            break;
        }
        if (next_carry > 0) {
            read_buffer[0] = read_buffer[bytes_to_write];
        }
        carry = next_carry;
        total_frames += frames;
        update_playback_ms(total_frames);
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
    ornament_http_stream_close(stream);

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

    if (resolved == NULL) {
        ESP_LOGW(TAG, "resolved song alloc failed: bytes=%u", (unsigned int)sizeof(*resolved));
        err = ESP_ERR_NO_MEM;
        goto cleanup;
    }
    if (s_mutex != NULL && xSemaphoreTake(s_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        set_state_locked(MUSIC_PLAYER_STATE_RESOLVING);
        xSemaphoreGive(s_mutex);
    }

    err = resolve_song(&request->settings, request, resolved);
    if (err != ESP_OK || stop_requested()) {
        requested_stop = stop_requested();
        goto cleanup;
    }

    release_xiaozhi_session_for_music();

    if (s_mutex != NULL && xSemaphoreTake(s_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        strlcpy(s_snapshot.title, resolved->title, sizeof(s_snapshot.title));
        strlcpy(s_snapshot.album, resolved->album, sizeof(s_snapshot.album));
        strlcpy(s_snapshot.picture, resolved->picture, sizeof(s_snapshot.picture));
        strlcpy(s_snapshot.cover_url, resolved->cover_url, sizeof(s_snapshot.cover_url));
        strlcpy(s_snapshot.lyrics, resolved->lyrics, sizeof(s_snapshot.lyrics));
        if (resolved->artist[0] != '\0') {
            strlcpy(s_snapshot.artist_name, resolved->artist, sizeof(s_snapshot.artist_name));
        }
        set_state_locked(MUSIC_PLAYER_STATE_PLAYING);
        xSemaphoreGive(s_mutex);
    }

#if CONFIG_ORNAMENT_MUSIC_COVER_ENABLED
    if (resolved->cover_url[0] != '\0') {
        uint16_t *cover_pixels = heap_caps_malloc(
            MUSIC_PLAYER_COVER_BYTES,
            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (cover_pixels == NULL) {
            cover_pixels = heap_caps_malloc(MUSIC_PLAYER_COVER_BYTES, MALLOC_CAP_8BIT);
        }
        if (cover_pixels != NULL) {
            esp_err_t cover_err = fetch_cover_pixels(resolved->cover_url, cover_pixels, MUSIC_PLAYER_COVER_PIXELS);
            if (cover_err == ESP_OK && s_mutex != NULL && xSemaphoreTake(s_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
                if (s_cover_pixels != NULL) {
                    memcpy(s_cover_pixels, cover_pixels, MUSIC_PLAYER_COVER_BYTES);
                    s_snapshot.cover_pixels = s_cover_pixels;
                    s_snapshot.has_cover = true;
                }
                xSemaphoreGive(s_mutex);
            } else if (cover_err != ESP_OK) {
                ESP_LOGW(TAG, "cover fetch failed: %s", esp_err_to_name(cover_err));
            }
            free(cover_pixels);
        } else {
            ESP_LOGW(TAG, "cover buffer alloc failed");
        }
    }
#endif

    free(resolved);
    resolved = NULL;

    err = stream_song(&request->settings, request);
    requested_stop = stop_requested();

cleanup:
    ESP_LOGI(TAG, "music task finished: err=%s stop=%d", esp_err_to_name(err), requested_stop);
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
#if CONFIG_ORNAMENT_MUSIC_COVER_ENABLED
    if (s_cover_pixels == NULL) {
        s_cover_pixels = heap_caps_calloc(MUSIC_PLAYER_COVER_PIXELS, sizeof(uint16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (s_cover_pixels == NULL) {
            s_cover_pixels = heap_caps_calloc(MUSIC_PLAYER_COVER_PIXELS, sizeof(uint16_t), MALLOC_CAP_8BIT);
        }
        if (s_cover_pixels == NULL) {
            vEventGroupDelete(s_events);
            vSemaphoreDelete(s_mutex);
            s_events = NULL;
            s_mutex = NULL;
            ESP_LOGW(TAG, "cover cache allocation failed");
            return ESP_ERR_NO_MEM;
        }
    }
    s_snapshot.cover_pixels = s_cover_pixels;
#endif
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
    s_snapshot.playback_ms = 0;
    strlcpy(s_snapshot.song_name, request->song_name, sizeof(s_snapshot.song_name));
    strlcpy(s_snapshot.artist_name, request->artist_name, sizeof(s_snapshot.artist_name));
    clear_metadata_locked();
    set_state_locked(MUSIC_PLAYER_STATE_RESOLVING);
    xEventGroupClearBits(s_events, MUSIC_PLAYER_TASK_DONE);

    s_task_stack_in_spiram = true;
    s_task_stack_bytes = select_music_task_stack_bytes(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
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
        return ESP_ERR_NO_MEM;
    }

    xSemaphoreGive(s_mutex);
    ESP_LOGI(TAG, "music task started");
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

esp_err_t music_player_play_song_with_settings(
    const char *song_name,
    const char *artist_name,
    uint32_t index,
    const ornament_settings_t *settings)
{
    (void)song_name;
    (void)artist_name;
    (void)index;
    (void)settings;
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
