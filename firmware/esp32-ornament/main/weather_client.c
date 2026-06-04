#include "weather_client.h"

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "settings.h"

#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static const char *TAG = "weather_client";
static const int WEATHER_RESPONSE_MAX = 4096;
static const int WEATHER_FETCH_TIMEOUT_MS = 5000;
static const time_t MIN_VALID_EPOCH = 1577836800;

typedef struct {
    char *data;
    int length;
    int capacity;
} response_buffer_t;

typedef struct {
    ornament_state_t weather;
    SemaphoreHandle_t mutex;
    TaskHandle_t task;
    esp_err_t last_error;
    int64_t last_success_ms;
    int64_t last_attempt_ms;
    bool have_weather;
    bool local_enabled;
    bool token_configured;
} weather_client_state_t;

static weather_client_state_t client_state;

static esp_err_t http_event_handler(esp_http_client_event_t *event)
{
    response_buffer_t *buffer = (response_buffer_t *)event->user_data;

    if (event->event_id != HTTP_EVENT_ON_DATA || buffer == NULL || event->data == NULL) {
        return ESP_OK;
    }
    if (buffer->length + event->data_len >= buffer->capacity) {
        ESP_LOGW(TAG, "weather response too large");
        return ESP_FAIL;
    }

    memcpy(buffer->data + buffer->length, event->data, event->data_len);
    buffer->length += event->data_len;
    buffer->data[buffer->length] = '\0';
    return ESP_OK;
}

static int round_double_to_int(double value)
{
    if (!isfinite(value)) {
        return 0;
    }
    return (int)(value >= 0.0 ? value + 0.5 : value - 0.5);
}

static void format_observed_at(char *out, size_t out_size)
{
    if (out_size == 0) {
        return;
    }
    time_t now = time(NULL);
    if (now < MIN_VALID_EPOCH) {
        out[0] = '\0';
        return;
    }
    struct tm local = {0};
    if (localtime_r(&now, &local) == NULL ||
        strftime(out, out_size, "%Y-%m-%dT%H:%M:%S%z", &local) == 0) {
        out[0] = '\0';
    }
}

static void format_epoch_observed_at(time_t epoch, char *out, size_t out_size)
{
    if (out_size == 0) {
        return;
    }
    if (epoch < MIN_VALID_EPOCH) {
        out[0] = '\0';
        return;
    }
    struct tm local = {0};
    if (localtime_r(&epoch, &local) == NULL ||
        strftime(out, out_size, "%Y-%m-%dT%H:%M:%S%z", &local) == 0) {
        out[0] = '\0';
    }
}

static void copy_json_string(cJSON *parent, const char *name, char *target, size_t target_size)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(parent, name);
    if (cJSON_IsString(item) && item->valuestring != NULL && target_size > 0) {
        strlcpy(target, item->valuestring, target_size);
    }
}

static const char *caiyun_summary_for_skycon(const char *skycon)
{
    if (skycon == NULL) {
        return "WEATHER";
    }
    if (strcmp(skycon, "CLEAR_DAY") == 0 || strcmp(skycon, "CLEAR_NIGHT") == 0) {
        return "CLEAR";
    }
    if (strcmp(skycon, "PARTLY_CLOUDY_DAY") == 0 || strcmp(skycon, "PARTLY_CLOUDY_NIGHT") == 0) {
        return "PARTLY CLOUDY";
    }
    if (strcmp(skycon, "CLOUDY") == 0) {
        return "CLOUDY";
    }
    if (strcmp(skycon, "LIGHT_HAZE") == 0 || strcmp(skycon, "MODERATE_HAZE") == 0 ||
        strcmp(skycon, "HEAVY_HAZE") == 0 || strcmp(skycon, "FOG") == 0) {
        return "FOG";
    }
    if (strcmp(skycon, "LIGHT_RAIN") == 0 || strcmp(skycon, "MODERATE_RAIN") == 0 ||
        strcmp(skycon, "HEAVY_RAIN") == 0 || strcmp(skycon, "STORM_RAIN") == 0) {
        return "RAIN";
    }
    if (strcmp(skycon, "LIGHT_SNOW") == 0 || strcmp(skycon, "MODERATE_SNOW") == 0 ||
        strcmp(skycon, "HEAVY_SNOW") == 0 || strcmp(skycon, "STORM_SNOW") == 0) {
        return "SNOW";
    }
    if (strcmp(skycon, "DUST") == 0 || strcmp(skycon, "SAND") == 0 || strcmp(skycon, "WIND") == 0) {
        return "WIND";
    }
    return "WEATHER";
}

static const char *caiyun_icon_for_skycon(const char *skycon)
{
    if (skycon == NULL) {
        return "unknown";
    }
    if (strcmp(skycon, "CLEAR_DAY") == 0 || strcmp(skycon, "CLEAR_NIGHT") == 0) {
        return "sun";
    }
    if (strcmp(skycon, "PARTLY_CLOUDY_DAY") == 0 || strcmp(skycon, "PARTLY_CLOUDY_NIGHT") == 0) {
        return "partly-cloudy";
    }
    if (strcmp(skycon, "CLOUDY") == 0) {
        return "cloud";
    }
    if (strcmp(skycon, "LIGHT_HAZE") == 0 || strcmp(skycon, "MODERATE_HAZE") == 0 ||
        strcmp(skycon, "HEAVY_HAZE") == 0 || strcmp(skycon, "DUST") == 0 || strcmp(skycon, "SAND") == 0) {
        return "haze";
    }
    if (strcmp(skycon, "FOG") == 0) {
        return "fog";
    }
    if (strcmp(skycon, "LIGHT_RAIN") == 0) {
        return "drizzle";
    }
    if (strcmp(skycon, "MODERATE_RAIN") == 0) {
        return "rain";
    }
    if (strcmp(skycon, "HEAVY_RAIN") == 0 || strcmp(skycon, "STORM_RAIN") == 0) {
        return "heavy-rain";
    }
    if (strcmp(skycon, "LIGHT_SNOW") == 0 || strcmp(skycon, "MODERATE_SNOW") == 0 ||
        strcmp(skycon, "HEAVY_SNOW") == 0 || strcmp(skycon, "STORM_SNOW") == 0) {
        return "snow";
    }
    if (strcmp(skycon, "WIND") == 0) {
        return "windy";
    }
    return "unknown";
}

static const char *open_meteo_summary_for_code(int code)
{
    switch (code) {
    case 0:
        return "CLEAR";
    case 1:
    case 2:
        return "PARTLY CLOUDY";
    case 3:
        return "CLOUDY";
    case 45:
    case 48:
        return "FOG";
    case 51:
    case 53:
    case 55:
    case 56:
    case 57:
        return "DRIZZLE";
    case 61:
    case 63:
    case 80:
    case 81:
        return "RAIN";
    case 65:
    case 66:
    case 67:
    case 82:
        return "HEAVY RAIN";
    case 71:
    case 73:
    case 75:
    case 77:
    case 85:
    case 86:
        return "SNOW";
    case 95:
    case 96:
    case 99:
        return "STORM";
    default:
        return "WEATHER";
    }
}

static const char *open_meteo_icon_for_code(int code)
{
    switch (code) {
    case 0:
        return "sun";
    case 1:
    case 2:
        return "partly-cloudy";
    case 3:
        return "cloud";
    case 45:
    case 48:
        return "fog";
    case 51:
    case 53:
    case 55:
    case 56:
    case 57:
        return "drizzle";
    case 61:
    case 63:
    case 80:
    case 81:
        return "rain";
    case 65:
    case 66:
    case 67:
    case 82:
        return "heavy-rain";
    case 71:
    case 73:
    case 75:
    case 77:
    case 85:
    case 86:
        return "snow";
    case 95:
    case 96:
    case 99:
        return "storm";
    default:
        return "unknown";
    }
}

static esp_err_t fetch_url_raw(const char *url, char *response, int response_capacity, int *status_code)
{
    response_buffer_t buffer = {
        .data = response,
        .length = 0,
        .capacity = response_capacity,
    };
    esp_http_client_config_t config = {
        .url = url,
        .event_handler = http_event_handler,
        .user_data = &buffer,
        .timeout_ms = WEATHER_FETCH_TIMEOUT_MS,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        return ESP_FAIL;
    }

    esp_err_t err = esp_http_client_perform(client);
    if (status_code != NULL) {
        *status_code = esp_http_client_get_status_code(client);
    }
    esp_http_client_cleanup(client);
    return err;
}

static void init_weather_state(const ornament_settings_t *settings, ornament_state_t *state, const char *source)
{
    ornament_state_init(state);
    state->has_weather = true;
    strlcpy(state->weather_status, "ok", sizeof(state->weather_status));
    strlcpy(state->weather_label, settings_weather_label_or_default(settings), sizeof(state->weather_label));
    strlcpy(state->weather_source, source, sizeof(state->weather_source));
    state->weather_code = -1;
    format_observed_at(state->weather_observed_at, sizeof(state->weather_observed_at));
}

static esp_err_t parse_caiyun_json(const char *json_text, const ornament_settings_t *settings, ornament_state_t *state)
{
    cJSON *root = cJSON_Parse(json_text);
    if (root == NULL) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    char status[16] = {0};
    copy_json_string(root, "status", status, sizeof(status));
    if (strcmp(status, "ok") != 0) {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_RESPONSE;
    }

    cJSON *result = cJSON_GetObjectItemCaseSensitive(root, "result");
    cJSON *realtime = cJSON_IsObject(result) ? cJSON_GetObjectItemCaseSensitive(result, "realtime") : NULL;
    if (!cJSON_IsObject(realtime)) {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_RESPONSE;
    }

    init_weather_state(settings, state, ORNAMENT_WEATHER_SOURCE_CAIYUN);
    cJSON *server_time = cJSON_GetObjectItemCaseSensitive(root, "server_time");
    if (cJSON_IsNumber(server_time)) {
        format_epoch_observed_at((time_t)server_time->valuedouble, state->weather_observed_at, sizeof(state->weather_observed_at));
    }

    cJSON *temperature = cJSON_GetObjectItemCaseSensitive(realtime, "temperature");
    if (cJSON_IsNumber(temperature)) {
        state->weather_temperature_c = round_double_to_int(temperature->valuedouble);
    }

    char skycon[32] = {0};
    copy_json_string(realtime, "skycon", skycon, sizeof(skycon));
    strlcpy(state->weather_summary, caiyun_summary_for_skycon(skycon), sizeof(state->weather_summary));
    strlcpy(state->weather_icon, caiyun_icon_for_skycon(skycon), sizeof(state->weather_icon));

    cJSON *wind = cJSON_GetObjectItemCaseSensitive(realtime, "wind");
    cJSON *speed = cJSON_IsObject(wind) ? cJSON_GetObjectItemCaseSensitive(wind, "speed") : NULL;
    if (cJSON_IsNumber(speed)) {
        state->weather_wind_kmh = round_double_to_int(speed->valuedouble);
    }

    cJSON_Delete(root);
    return ESP_OK;
}

static esp_err_t parse_open_meteo_json(const char *json_text, const ornament_settings_t *settings, ornament_state_t *state)
{
    cJSON *root = cJSON_Parse(json_text);
    if (root == NULL) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    cJSON *current = cJSON_GetObjectItemCaseSensitive(root, "current");
    if (!cJSON_IsObject(current)) {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_RESPONSE;
    }

    cJSON *temperature = cJSON_GetObjectItemCaseSensitive(current, "temperature_2m");
    cJSON *weather_code = cJSON_GetObjectItemCaseSensitive(current, "weather_code");
    cJSON *wind_speed = cJSON_GetObjectItemCaseSensitive(current, "wind_speed_10m");
    if (!cJSON_IsNumber(temperature) || !cJSON_IsNumber(weather_code)) {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_RESPONSE;
    }

    init_weather_state(settings, state, ORNAMENT_WEATHER_SOURCE_OPEN_METEO);
    copy_json_string(current, "time", state->weather_observed_at, sizeof(state->weather_observed_at));
    int code = round_double_to_int(weather_code->valuedouble);
    state->weather_temperature_c = round_double_to_int(temperature->valuedouble);
    state->weather_code = code;
    strlcpy(state->weather_summary, open_meteo_summary_for_code(code), sizeof(state->weather_summary));
    strlcpy(state->weather_icon, open_meteo_icon_for_code(code), sizeof(state->weather_icon));
    if (cJSON_IsNumber(wind_speed)) {
        state->weather_wind_kmh = round_double_to_int(wind_speed->valuedouble);
    }

    cJSON_Delete(root);
    return ESP_OK;
}

static esp_err_t fetch_caiyun_weather(const ornament_settings_t *settings, ornament_state_t *state)
{
    if (settings == NULL || state == NULL || !settings->has_caiyun_token || settings->caiyun_token[0] == '\0') {
        return ESP_ERR_INVALID_STATE;
    }

    double lon = (double)settings->weather_lon_e6 / 1000000.0;
    double lat = (double)settings->weather_lat_e6 / 1000000.0;
    char url[256];
    snprintf(
        url,
        sizeof(url),
        "https://api.caiyunapp.com/v2.6/%s/%.6f,%.6f/realtime",
        settings->caiyun_token,
        lon,
        lat);

    char *response = calloc(WEATHER_RESPONSE_MAX, 1);
    if (response == NULL) {
        return ESP_ERR_NO_MEM;
    }

    int status_code = 0;
    esp_err_t err = fetch_url_raw(url, response, WEATHER_RESPONSE_MAX, &status_code);
    if (err == ESP_OK && status_code == 200) {
        err = parse_caiyun_json(response, settings, state);
    } else if (err == ESP_OK) {
        err = ESP_ERR_HTTP_BASE + status_code;
    }

    free(response);
    return err;
}

static esp_err_t fetch_open_meteo_weather(const ornament_settings_t *settings, ornament_state_t *state)
{
    if (settings == NULL || state == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    double lon = (double)settings->weather_lon_e6 / 1000000.0;
    double lat = (double)settings->weather_lat_e6 / 1000000.0;
    char url[256];
    snprintf(
        url,
        sizeof(url),
        "https://api.open-meteo.com/v1/forecast?latitude=%.6f&longitude=%.6f&current=temperature_2m,weather_code,wind_speed_10m&wind_speed_unit=kmh&timezone=auto&forecast_days=1",
        lat,
        lon);

    char *response = calloc(WEATHER_RESPONSE_MAX, 1);
    if (response == NULL) {
        return ESP_ERR_NO_MEM;
    }

    int status_code = 0;
    esp_err_t err = fetch_url_raw(url, response, WEATHER_RESPONSE_MAX, &status_code);
    if (err == ESP_OK && status_code == 200) {
        err = parse_open_meteo_json(response, settings, state);
    } else if (err == ESP_OK) {
        err = ESP_ERR_HTTP_BASE + status_code;
    }

    free(response);
    return err;
}

static esp_err_t fetch_weather(const ornament_settings_t *settings, ornament_state_t *state)
{
    const char *source = settings_weather_source_or_default(settings);
    if (strcmp(source, ORNAMENT_WEATHER_SOURCE_OPEN_METEO) == 0) {
        return fetch_open_meteo_weather(settings, state);
    }
    return fetch_caiyun_weather(settings, state);
}

static bool local_weather_enabled(const ornament_settings_t *settings)
{
    const char *source = settings_weather_source_or_default(settings);
    if (strcmp(source, ORNAMENT_WEATHER_SOURCE_OPEN_METEO) == 0) {
        return true;
    }
    return settings != NULL && settings->has_caiyun_token;
}

static void publish_weather(const ornament_state_t *weather, esp_err_t err)
{
    if (client_state.mutex == NULL) {
        return;
    }
    if (xSemaphoreTake(client_state.mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        return;
    }

    client_state.last_error = err;
    client_state.last_attempt_ms = esp_timer_get_time() / 1000;
    if (err == ESP_OK && weather != NULL && weather->has_weather) {
        client_state.weather = *weather;
        client_state.have_weather = true;
        client_state.last_success_ms = client_state.last_attempt_ms;
    }

    xSemaphoreGive(client_state.mutex);
}

static void wait_for_next_attempt(uint32_t delay_ms)
{
    (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(delay_ms));
}

static void weather_task(void *arg)
{
    (void)arg;
    while (true) {
        ornament_settings_t settings;
        esp_err_t err = settings_load(&settings);
        if (err != ESP_OK) {
            publish_weather(NULL, err);
            wait_for_next_attempt(CONFIG_ORNAMENT_WEATHER_RETRY_INTERVAL_MS);
            continue;
        }

        if (client_state.mutex != NULL && xSemaphoreTake(client_state.mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            client_state.token_configured = settings.has_caiyun_token;
            client_state.local_enabled = local_weather_enabled(&settings);
            xSemaphoreGive(client_state.mutex);
        }

        if (!local_weather_enabled(&settings)) {
            publish_weather(NULL, ESP_ERR_INVALID_STATE);
            wait_for_next_attempt(CONFIG_ORNAMENT_WEATHER_RETRY_INTERVAL_MS);
            continue;
        }

        ornament_state_t weather;
        err = fetch_weather(&settings, &weather);
        if (err == ESP_OK) {
            ESP_LOGI(
                TAG,
                "weather updated source=%s label=%s temp=%d icon=%s wind=%d",
                weather.weather_source,
                weather.weather_label,
                weather.weather_temperature_c,
                weather.weather_icon,
                weather.weather_wind_kmh);
            publish_weather(&weather, ESP_OK);
            wait_for_next_attempt(CONFIG_ORNAMENT_WEATHER_FETCH_INTERVAL_MS);
        } else {
            ESP_LOGW(TAG, "weather fetch failed: %s", esp_err_to_name(err));
            publish_weather(NULL, err);
            wait_for_next_attempt(CONFIG_ORNAMENT_WEATHER_RETRY_INTERVAL_MS);
        }
    }
}

esp_err_t weather_client_start(void)
{
    if (client_state.task != NULL) {
        return ESP_OK;
    }
    if (client_state.mutex == NULL) {
        client_state.mutex = xSemaphoreCreateMutex();
        if (client_state.mutex == NULL) {
            return ESP_ERR_NO_MEM;
        }
        ornament_state_init(&client_state.weather);
        client_state.last_error = ESP_ERR_INVALID_STATE;
    }

    BaseType_t created = xTaskCreate(weather_task, "weather", 8192, NULL, 4, &client_state.task);
    return created == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

void weather_client_apply(ornament_state_t *state)
{
    if (state == NULL || client_state.mutex == NULL) {
        return;
    }
    if (xSemaphoreTake(client_state.mutex, pdMS_TO_TICKS(20)) != pdTRUE) {
        return;
    }
    if (client_state.local_enabled && client_state.have_weather) {
        state->has_weather = true;
        strlcpy(state->weather_status, client_state.weather.weather_status, sizeof(state->weather_status));
        strlcpy(state->weather_label, client_state.weather.weather_label, sizeof(state->weather_label));
        strlcpy(state->weather_source, client_state.weather.weather_source, sizeof(state->weather_source));
        strlcpy(state->weather_summary, client_state.weather.weather_summary, sizeof(state->weather_summary));
        strlcpy(state->weather_icon, client_state.weather.weather_icon, sizeof(state->weather_icon));
        strlcpy(state->weather_observed_at, client_state.weather.weather_observed_at, sizeof(state->weather_observed_at));
        state->weather_temperature_c = client_state.weather.weather_temperature_c;
        state->weather_wind_kmh = client_state.weather.weather_wind_kmh;
        state->weather_code = client_state.weather.weather_code;
    }
    xSemaphoreGive(client_state.mutex);
}

bool weather_client_token_configured(void)
{
    if (client_state.mutex == NULL) {
        return false;
    }
    bool configured = false;
    if (xSemaphoreTake(client_state.mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
        configured = client_state.token_configured;
        xSemaphoreGive(client_state.mutex);
    }
    return configured;
}

void weather_client_settings_changed(void)
{
    if (client_state.mutex != NULL && xSemaphoreTake(client_state.mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        client_state.have_weather = false;
        xSemaphoreGive(client_state.mutex);
    }
    if (client_state.task != NULL) {
        xTaskNotifyGive(client_state.task);
    }
}
