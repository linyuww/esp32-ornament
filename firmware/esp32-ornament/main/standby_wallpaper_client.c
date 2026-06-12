#include "standby_wallpaper_client.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "ornament_http_client.h"

#include <string.h>

static const char *TAG = "standby_wallpaper";
static const int WALLPAPER_HTTP_TIMEOUT_MS = 15000;

typedef struct {
    SemaphoreHandle_t mutex;
    uint16_t *pixels;
    char wallpaper_id[ORNAMENT_WALLPAPER_ID_MAX];
    uint16_t width;
    uint16_t height;
    bool ready;
} standby_wallpaper_client_state_t;

static standby_wallpaper_client_state_t client_state;

static size_t wallpaper_pixel_count(void)
{
    return (size_t)STANDBY_WALLPAPER_CLIENT_WIDTH * (size_t)STANDBY_WALLPAPER_CLIENT_HEIGHT;
}

static size_t wallpaper_buffer_bytes(void)
{
    return wallpaper_pixel_count() * sizeof(uint16_t);
}

static bool wallpaper_id_matches(const ornament_state_t *state)
{
    return state != NULL &&
           state->has_standby_wallpaper &&
           state->standby_wallpaper_id[0] != '\0' &&
           client_state.ready &&
           strcmp(client_state.wallpaper_id, state->standby_wallpaper_id) == 0;
}

static esp_err_t wallpaper_buffer_ensure(void)
{
    if (client_state.pixels != NULL) {
        return ESP_OK;
    }

    client_state.pixels = heap_caps_malloc(
        wallpaper_buffer_bytes(),
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (client_state.pixels == NULL) {
        client_state.pixels = heap_caps_malloc(
            wallpaper_buffer_bytes(),
            MALLOC_CAP_8BIT);
    }
    return client_state.pixels != NULL ? ESP_OK : ESP_ERR_NO_MEM;
}

esp_err_t standby_wallpaper_client_init(void)
{
    if (client_state.mutex != NULL) {
        return ESP_OK;
    }

    client_state.mutex = xSemaphoreCreateMutex();
    if (client_state.mutex == NULL) {
        return ESP_ERR_NO_MEM;
    }
    client_state.width = STANDBY_WALLPAPER_CLIENT_WIDTH;
    client_state.height = STANDBY_WALLPAPER_CLIENT_HEIGHT;
    return ESP_OK;
}

void standby_wallpaper_client_refresh_if_needed(const ornament_state_t *state)
{
    if (state == NULL || !state->has_standby_wallpaper || state->standby_wallpaper_url[0] == '\0') {
        return;
    }
    if (client_state.mutex == NULL) {
        return;
    }

    if (xSemaphoreTake(client_state.mutex, pdMS_TO_TICKS(25)) != pdTRUE) {
        return;
    }
    bool up_to_date = wallpaper_id_matches(state);
    xSemaphoreGive(client_state.mutex);
    if (up_to_date) {
        return;
    }

    if (wallpaper_buffer_ensure() != ESP_OK) {
        ESP_LOGW(TAG, "wallpaper buffer allocation failed");
        return;
    }

    size_t response_len = 0;
    int status_code = 0;
    ornament_http_request_t request = {
        .method = "GET",
        .url = state->standby_wallpaper_url,
        .accept = "application/octet-stream",
        .response = client_state.pixels,
        .response_capacity = wallpaper_buffer_bytes(),
        .response_len = &response_len,
        .status_code = &status_code,
        .timeout_ms = WALLPAPER_HTTP_TIMEOUT_MS,
    };

    esp_err_t err = ornament_http_request(&request);
    if (err != ESP_OK || status_code != 200 || response_len != wallpaper_buffer_bytes()) {
        ESP_LOGW(
            TAG,
            "wallpaper fetch failed id=%s err=%s http=%d bytes=%u",
            state->standby_wallpaper_id,
            esp_err_to_name(err),
            status_code,
            (unsigned int)response_len);
        return;
    }

    if (xSemaphoreTake(client_state.mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        return;
    }
    strlcpy(client_state.wallpaper_id, state->standby_wallpaper_id, sizeof(client_state.wallpaper_id));
    client_state.ready = true;
    xSemaphoreGive(client_state.mutex);
    ESP_LOGI(TAG, "wallpaper updated id=%s name=%s", state->standby_wallpaper_id, state->standby_wallpaper_name);
}

bool standby_wallpaper_client_copy_frame(uint16_t *pixels, size_t pixel_count)
{
    if (pixels == NULL || pixel_count < wallpaper_pixel_count() || client_state.mutex == NULL) {
        return false;
    }
    if (xSemaphoreTake(client_state.mutex, pdMS_TO_TICKS(25)) != pdTRUE) {
        return false;
    }
    bool ready = client_state.ready && client_state.pixels != NULL;
    if (ready) {
        memcpy(pixels, client_state.pixels, wallpaper_buffer_bytes());
    }
    xSemaphoreGive(client_state.mutex);
    return ready;
}

void standby_wallpaper_client_snapshot(standby_wallpaper_snapshot_t *snapshot)
{
    if (snapshot == NULL) {
        return;
    }
    memset(snapshot, 0, sizeof(*snapshot));
    snapshot->width = STANDBY_WALLPAPER_CLIENT_WIDTH;
    snapshot->height = STANDBY_WALLPAPER_CLIENT_HEIGHT;
    if (client_state.mutex == NULL) {
        return;
    }
    if (xSemaphoreTake(client_state.mutex, pdMS_TO_TICKS(25)) != pdTRUE) {
        return;
    }
    snapshot->ready = client_state.ready;
    snapshot->width = client_state.width;
    snapshot->height = client_state.height;
    strlcpy(snapshot->wallpaper_id, client_state.wallpaper_id, sizeof(snapshot->wallpaper_id));
    xSemaphoreGive(client_state.mutex);
}
