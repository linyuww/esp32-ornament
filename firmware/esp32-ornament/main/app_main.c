#include "asrpro_link.h"
#include "bridge_client.h"
#include "config_portal.h"
#include "display.h"
#include "ornament_state.h"
#include "system_status.h"
#include "task_audio.h"
#include "web_console.h"
#include "weather_client.h"
#include "wifi.h"
#include "xiaozhi_client.h"

#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs_flash.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#ifndef CONFIG_ORNAMENT_XIAOZHI_AUTO_START
#define CONFIG_ORNAMENT_XIAOZHI_AUTO_START 0
#endif

#ifndef CONFIG_ORNAMENT_PAGE_BUTTON_ENABLED
#define CONFIG_ORNAMENT_PAGE_BUTTON_ENABLED 0
#endif

#ifndef CONFIG_ORNAMENT_PAGE_BUTTON_GPIO
#define CONFIG_ORNAMENT_PAGE_BUTTON_GPIO 15
#endif

#ifndef CONFIG_ORNAMENT_PAGE_BUTTON_DEBOUNCE_MS
#define CONFIG_ORNAMENT_PAGE_BUTTON_DEBOUNCE_MS 50
#endif

#ifdef CONFIG_ORNAMENT_PAGE_BUTTON_ACTIVE_LOW
#define ORNAMENT_PAGE_BUTTON_ACTIVE_LOW 1
#else
#define ORNAMENT_PAGE_BUTTON_ACTIVE_LOW 0
#endif

static const char *TAG = "ornament";

#define VOICE_PAGE_HOLD_MS 12000
#define VOICE_STATUS_TEXT_MAX 40
#define ORNAMENT_MIN_VALID_EPOCH 1577836800LL

#ifndef CONFIG_ORNAMENT_EXPIRED_QUOTA_RETRY_MS
#define CONFIG_ORNAMENT_EXPIRED_QUOTA_RETRY_MS 30000
#endif

typedef struct {
    ornament_state_t state;
    esp_err_t fetch_error;
    TickType_t last_done_tick;
    bool have_state;
    bool have_last_done_tick;
} ornament_shared_state_t;

typedef enum {
    VOICE_VIEW_AUTO,
    VOICE_VIEW_STATUS,
    VOICE_VIEW_QUOTA,
    VOICE_VIEW_TASKS,
    VOICE_VIEW_CLOCK,
    VOICE_VIEW_XIAOZHI,
} voice_view_t;

typedef struct {
    voice_view_t view;
    TickType_t hold_until_tick;
    TickType_t refresh_requested_tick;
    TickType_t bridge_match_requested_tick;
    bool quiet_mode;
    char last_command[VOICE_STATUS_TEXT_MAX];
    char last_result[VOICE_STATUS_TEXT_MAX];
} voice_control_state_t;

static SemaphoreHandle_t shared_state_mutex;
static ornament_shared_state_t shared_state;
static SemaphoreHandle_t voice_control_mutex;
static voice_control_state_t voice_control;
static QueueHandle_t voice_command_queue;

static bool bridge_is_offline(int consecutive_fetch_failures, esp_err_t err)
{
    return err != ESP_OK && consecutive_fetch_failures >= CONFIG_ORNAMENT_BRIDGE_OFFLINE_FAILURES;
}

static bool standby_timer_eligible(const ornament_state_t *state)
{
    if (state == NULL || state->active_task_count > 0) {
        return false;
    }
    ornament_status_t status = ornament_state_panel_status(state);
    return status != ORNAMENT_STATUS_RUNNING && status != ORNAMENT_STATUS_ERROR;
}

static bool should_show_standby_clock(const ornament_state_t *state, TickType_t idle_since_tick, TickType_t now)
{
    if (state != NULL && state->bridge_offline && !state->has_quota && state->active_task_count == 0) {
        return true;
    }
    return CONFIG_ORNAMENT_STANDBY_CLOCK_MS > 0 &&
           standby_timer_eligible(state) &&
           !state->done_flash_active &&
           idle_since_tick != 0 &&
           (now - idle_since_tick) >= pdMS_TO_TICKS(CONFIG_ORNAMENT_STANDBY_CLOCK_MS);
}

static uint32_t ticks_to_ms(TickType_t ticks)
{
    return (uint32_t)(ticks * portTICK_PERIOD_MS);
}

static bool parse_ndigits(const char *text, int count, int *value)
{
    if (text == NULL || value == NULL || count <= 0) {
        return false;
    }

    int parsed = 0;
    for (int i = 0; i < count; i++) {
        char c = text[i];
        if (c < '0' || c > '9') {
            return false;
        }
        parsed = parsed * 10 + (c - '0');
    }

    *value = parsed;
    return true;
}

static int64_t days_from_civil(int year, unsigned month, unsigned day)
{
    year -= month <= 2U;
    const int era = (year >= 0 ? year : year - 399) / 400;
    const unsigned yoe = (unsigned)(year - era * 400);
    const int shifted_month = (int)month + (month > 2U ? -3 : 9);
    const unsigned doy = (153U * (unsigned)shifted_month + 2U) / 5U + day - 1U;
    const unsigned doe = yoe * 365U + yoe / 4U - yoe / 100U + doy;
    return (int64_t)era * 146097LL + (int64_t)doe - 719468LL;
}

static bool valid_timestamp_parts(int year, int month, int day, int hour, int minute, int second)
{
    return year >= 1970 &&
           month >= 1 && month <= 12 &&
           day >= 1 && day <= 31 &&
           hour >= 0 && hour <= 23 &&
           minute >= 0 && minute <= 59 &&
           second >= 0 && second <= 60;
}

static bool parse_iso8601_epoch(const char *timestamp, time_t *out)
{
    if (timestamp == NULL || out == NULL || strlen(timestamp) < 16 ||
        timestamp[4] != '-' || timestamp[7] != '-' ||
        (timestamp[10] != 'T' && timestamp[10] != ' ') ||
        timestamp[13] != ':') {
        return false;
    }

    int year = 0;
    int month = 0;
    int day = 0;
    int hour = 0;
    int minute = 0;
    int second = 0;
    if (!parse_ndigits(timestamp, 4, &year) ||
        !parse_ndigits(timestamp + 5, 2, &month) ||
        !parse_ndigits(timestamp + 8, 2, &day) ||
        !parse_ndigits(timestamp + 11, 2, &hour) ||
        !parse_ndigits(timestamp + 14, 2, &minute)) {
        return false;
    }

    const char *cursor = timestamp + 16;
    if (*cursor == ':') {
        if (!parse_ndigits(cursor + 1, 2, &second)) {
            return false;
        }
        cursor += 3;
    }
    if (!valid_timestamp_parts(year, month, day, hour, minute, second)) {
        return false;
    }

    while (*cursor == '.') {
        cursor++;
        while (*cursor >= '0' && *cursor <= '9') {
            cursor++;
        }
    }

    if (*cursor == '\0') {
        struct tm local = {
            .tm_year = year - 1900,
            .tm_mon = month - 1,
            .tm_mday = day,
            .tm_hour = hour,
            .tm_min = minute,
            .tm_sec = second,
            .tm_isdst = -1,
        };
        time_t local_epoch = mktime(&local);
        if (local_epoch == (time_t)-1) {
            return false;
        }
        *out = local_epoch;
        return true;
    }

    int offset_seconds = 0;
    if (*cursor == 'Z' || *cursor == 'z') {
        offset_seconds = 0;
    } else if (*cursor == '+' || *cursor == '-') {
        int offset_hour = 0;
        int offset_minute = 0;
        int sign = *cursor == '+' ? 1 : -1;
        cursor++;
        if (!parse_ndigits(cursor, 2, &offset_hour)) {
            return false;
        }
        cursor += 2;
        if (*cursor == ':') {
            cursor++;
        }
        if (!parse_ndigits(cursor, 2, &offset_minute) ||
            offset_hour > 23 || offset_minute > 59) {
            return false;
        }
        offset_seconds = sign * (offset_hour * 3600 + offset_minute * 60);
    } else {
        return false;
    }

    int64_t epoch = days_from_civil(year, (unsigned)month, (unsigned)day) * 86400LL +
                    (int64_t)hour * 3600LL +
                    (int64_t)minute * 60LL +
                    (int64_t)second -
                    (int64_t)offset_seconds;
    *out = (time_t)epoch;
    return true;
}

static bool current_real_time(time_t *now)
{
    time_t current = time(NULL);
    if (current < (time_t)ORNAMENT_MIN_VALID_EPOCH) {
        return false;
    }
    if (now != NULL) {
        *now = current;
    }
    return true;
}

static bool reset_timestamp_has_passed(const char *timestamp, time_t now)
{
    time_t reset_at = 0;
    return parse_iso8601_epoch(timestamp, &reset_at) && reset_at <= now;
}

static bool reset_timestamp_is_current(const char *timestamp, time_t now)
{
    time_t reset_at = 0;
    return parse_iso8601_epoch(timestamp, &reset_at) && reset_at > now;
}

static bool quota_reset_refresh_needed(const ornament_state_t *state)
{
    if (state == NULL || !state->has_quota) {
        return false;
    }

    time_t now = 0;
    if (!current_real_time(&now)) {
        return false;
    }

    return reset_timestamp_has_passed(state->primary_resets_at, now) ||
           reset_timestamp_has_passed(state->secondary_resets_at, now);
}

static bool quota_reset_times_current(const ornament_state_t *state)
{
    if (state == NULL || !state->has_quota) {
        return false;
    }

    time_t now = 0;
    if (!current_real_time(&now)) {
        return false;
    }

    return reset_timestamp_is_current(state->primary_resets_at, now) &&
           reset_timestamp_is_current(state->secondary_resets_at, now);
}

static void hide_expired_quota_reset_times(ornament_state_t *state)
{
    if (state == NULL || !state->has_quota) {
        return;
    }

    time_t now = 0;
    if (!current_real_time(&now)) {
        return;
    }

    if (reset_timestamp_has_passed(state->primary_resets_at, now)) {
        state->primary_resets_at[0] = '\0';
    }
    if (reset_timestamp_has_passed(state->secondary_resets_at, now)) {
        state->secondary_resets_at[0] = '\0';
    }
}

static TickType_t expired_quota_retry_ticks(void)
{
    int retry_ms = CONFIG_ORNAMENT_EXPIRED_QUOTA_RETRY_MS;
    if (retry_ms <= 0) {
        retry_ms = 30000;
    }
    return pdMS_TO_TICKS(retry_ms);
}

static void update_local_animation(
    ornament_state_t *state,
    TickType_t now,
    TickType_t last_done_tick,
    bool have_last_done_tick)
{
    ornament_state_update_display_timing(state, ticks_to_ms(now), ticks_to_ms(last_done_tick), have_last_done_tick);
}

static void apply_local_state(ornament_state_t *state, bool bridge_offline)
{
    if (state == NULL) {
        return;
    }
    system_status_update(state);
    weather_client_apply(state);
    state->bridge_offline = bridge_offline;
    if (bridge_offline && state->status == ORNAMENT_STATUS_ERROR && !state->has_task && !state->has_quota) {
        state->status = ORNAMENT_STATUS_IDLE;
    }
}

static void render_current_state(const ornament_state_t *state, TickType_t idle_since_tick, TickType_t now)
{
    if (should_show_standby_clock(state, idle_since_tick, now)) {
        display_render_clock(state);
    } else {
        display_render_state(state);
    }
}

static void voice_control_init(void)
{
    if (voice_control_mutex == NULL) {
        voice_control_mutex = xSemaphoreCreateMutex();
        ESP_ERROR_CHECK(voice_control_mutex == NULL ? ESP_ERR_NO_MEM : ESP_OK);
    }
    memset(&voice_control, 0, sizeof(voice_control));
    voice_control.view = VOICE_VIEW_AUTO;
    strlcpy(voice_control.last_command, "VOICE READY", sizeof(voice_control.last_command));
    strlcpy(voice_control.last_result, "SAY A COMMAND", sizeof(voice_control.last_result));
}

static bool voice_control_snapshot(voice_control_state_t *snapshot)
{
    if (voice_control_mutex == NULL || snapshot == NULL) {
        return false;
    }
    if (xSemaphoreTake(voice_control_mutex, pdMS_TO_TICKS(20)) != pdTRUE) {
        return false;
    }
    *snapshot = voice_control;
    xSemaphoreGive(voice_control_mutex);
    return true;
}

static const char *voice_view_name(voice_view_t view)
{
    switch (view) {
    case VOICE_VIEW_AUTO:
        return "AUTO";
    case VOICE_VIEW_STATUS:
        return "STATUS";
    case VOICE_VIEW_QUOTA:
        return "QUOTA";
    case VOICE_VIEW_TASKS:
        return "TASKS";
    case VOICE_VIEW_CLOCK:
        return "CLOCK";
    case VOICE_VIEW_XIAOZHI:
        return "XIAOZHI";
    default:
        return "UNKNOWN";
    }
}

static voice_view_t next_button_view(voice_view_t current)
{
    switch (current) {
    case VOICE_VIEW_AUTO:
        return VOICE_VIEW_QUOTA;
    case VOICE_VIEW_QUOTA:
        return VOICE_VIEW_TASKS;
    case VOICE_VIEW_TASKS:
        return VOICE_VIEW_CLOCK;
    case VOICE_VIEW_CLOCK:
        return VOICE_VIEW_XIAOZHI;
    case VOICE_VIEW_XIAOZHI:
        return VOICE_VIEW_AUTO;
    case VOICE_VIEW_STATUS:
    default:
        return VOICE_VIEW_AUTO;
    }
}

static void voice_control_set_view(voice_view_t view, TickType_t now, const char *command, const char *result)
{
    if (voice_control_mutex == NULL) {
        return;
    }
    if (xSemaphoreTake(voice_control_mutex, pdMS_TO_TICKS(50)) != pdTRUE) {
        ESP_LOGW(TAG, "voice state update skipped: mutex timeout");
        return;
    }
    voice_control.view = view;
    voice_control.hold_until_tick = now + pdMS_TO_TICKS(VOICE_PAGE_HOLD_MS);
    if (command != NULL) {
        strlcpy(voice_control.last_command, command, sizeof(voice_control.last_command));
    }
    if (result != NULL) {
        strlcpy(voice_control.last_result, result, sizeof(voice_control.last_result));
    }
    xSemaphoreGive(voice_control_mutex);
}

static void voice_control_cycle_page(TickType_t now)
{
    if (voice_control_mutex == NULL) {
        return;
    }
    if (xSemaphoreTake(voice_control_mutex, pdMS_TO_TICKS(50)) != pdTRUE) {
        ESP_LOGW(TAG, "page cycle skipped: mutex timeout");
        return;
    }
    voice_control.view = next_button_view(voice_control.view);
    voice_control.hold_until_tick = voice_control.view == VOICE_VIEW_AUTO ? 0 : portMAX_DELAY;
    strlcpy(voice_control.last_command, "PAGE BUTTON", sizeof(voice_control.last_command));
    snprintf(
        voice_control.last_result,
        sizeof(voice_control.last_result),
        "PAGE %s",
        voice_view_name(voice_control.view));
    ESP_LOGI(TAG, "page button switched to %s at tick %lu", voice_view_name(voice_control.view), (unsigned long)now);
    xSemaphoreGive(voice_control_mutex);
}

static void voice_control_set_result(const char *result)
{
    if (voice_control_mutex == NULL || result == NULL) {
        return;
    }
    if (xSemaphoreTake(voice_control_mutex, pdMS_TO_TICKS(50)) != pdTRUE) {
        return;
    }
    strlcpy(voice_control.last_result, result, sizeof(voice_control.last_result));
    xSemaphoreGive(voice_control_mutex);
}

static void voice_control_request_refresh(TickType_t now)
{
    if (voice_control_mutex == NULL) {
        return;
    }
    if (xSemaphoreTake(voice_control_mutex, pdMS_TO_TICKS(50)) != pdTRUE) {
        return;
    }
    voice_control.refresh_requested_tick = now;
    xSemaphoreGive(voice_control_mutex);
}

static void voice_control_request_bridge_match(TickType_t now)
{
    if (voice_control_mutex == NULL) {
        return;
    }
    if (xSemaphoreTake(voice_control_mutex, pdMS_TO_TICKS(50)) != pdTRUE) {
        return;
    }
    voice_control.bridge_match_requested_tick = now;
    xSemaphoreGive(voice_control_mutex);
}

static bool voice_control_take_refresh(TickType_t *request_tick)
{
    if (voice_control_mutex == NULL || request_tick == NULL) {
        return false;
    }
    if (xSemaphoreTake(voice_control_mutex, pdMS_TO_TICKS(20)) != pdTRUE) {
        return false;
    }
    bool requested = voice_control.refresh_requested_tick != 0;
    *request_tick = voice_control.refresh_requested_tick;
    voice_control.refresh_requested_tick = 0;
    xSemaphoreGive(voice_control_mutex);
    return requested;
}

static bool voice_control_take_bridge_match(TickType_t *request_tick)
{
    if (voice_control_mutex == NULL || request_tick == NULL) {
        return false;
    }
    if (xSemaphoreTake(voice_control_mutex, pdMS_TO_TICKS(20)) != pdTRUE) {
        return false;
    }
    bool requested = voice_control.bridge_match_requested_tick != 0;
    *request_tick = voice_control.bridge_match_requested_tick;
    voice_control.bridge_match_requested_tick = 0;
    xSemaphoreGive(voice_control_mutex);
    return requested;
}

static bool voice_control_quiet_mode(void)
{
    if (voice_control_mutex == NULL) {
        return false;
    }
    if (xSemaphoreTake(voice_control_mutex, pdMS_TO_TICKS(20)) != pdTRUE) {
        return false;
    }
    bool quiet = voice_control.quiet_mode;
    xSemaphoreGive(voice_control_mutex);
    return quiet;
}

static void voice_control_set_quiet(bool enabled)
{
    if (voice_control_mutex == NULL) {
        return;
    }
    if (xSemaphoreTake(voice_control_mutex, pdMS_TO_TICKS(50)) != pdTRUE) {
        return;
    }
    voice_control.quiet_mode = enabled;
    strlcpy(voice_control.last_result, enabled ? "QUIET ON" : "QUIET OFF", sizeof(voice_control.last_result));
    xSemaphoreGive(voice_control_mutex);
}

static bool voice_view_active(voice_view_t view, TickType_t hold_until_tick, TickType_t now)
{
    return view != VOICE_VIEW_AUTO &&
           (hold_until_tick == portMAX_DELAY || (hold_until_tick != 0 && now < hold_until_tick));
}

static bool render_voice_override(
    const ornament_state_t *state,
    esp_err_t fetch_error,
    const voice_control_state_t *voice_state,
    TickType_t now)
{
    if (state == NULL || voice_state == NULL ||
        !voice_view_active(voice_state->view, voice_state->hold_until_tick, now)) {
        return false;
    }

    char bridge_status[VOICE_STATUS_TEXT_MAX];
    snprintf(bridge_status, sizeof(bridge_status), "BRIDGE %s", esp_err_to_name(fetch_error));

    switch (voice_state->view) {
    case VOICE_VIEW_STATUS:
        display_render_voice_status(state, bridge_status, voice_state->last_result);
        break;
    case VOICE_VIEW_TASKS:
        display_render_tasks(state);
        break;
    case VOICE_VIEW_CLOCK:
        display_render_clock(state);
        break;
    case VOICE_VIEW_QUOTA:
        display_render_state(state);
        break;
    case VOICE_VIEW_XIAOZHI: {
        xiaozhi_client_snapshot_t snapshot;
        xiaozhi_client_status_snapshot(&snapshot);
        display_render_xiaozhi(state, &snapshot);
        break;
    }
    case VOICE_VIEW_AUTO:
    default:
        return false;
    }
    return true;
}

static bool is_new_done_event(const ornament_state_t *state, bool have_seen_state, int last_done_seq)
{
    if (!have_seen_state || state->done_seq <= 0) {
        return false;
    }
    return state->done_seq > last_done_seq;
}

static bool state_has_displayable_snapshot(const ornament_state_t *state)
{
    return state != NULL && (state->has_task || state->has_quota || state->has_weather || state->active_task_count > 0);
}

static bool shared_state_has_displayable_snapshot(void)
{
    if (shared_state_mutex == NULL) {
        return false;
    }
    bool displayable = false;
    if (xSemaphoreTake(shared_state_mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
        displayable = shared_state.have_state && state_has_displayable_snapshot(&shared_state.state);
        xSemaphoreGive(shared_state_mutex);
    }
    return displayable;
}

static bool should_announce_done_event(
    const ornament_state_t *state,
    bool have_seen_state,
    int last_done_seq,
    int previous_active_task_count)
{
    if (!is_new_done_event(state, have_seen_state, last_done_seq)) {
        return false;
    }
    if (previous_active_task_count > 0 && state->active_task_count == 0) {
        return true;
    }
    ESP_LOGW(
        TAG,
        "suppress done alert: done_seq advanced but active task count is not idle %d -> %d",
        previous_active_task_count,
        state->active_task_count);
    return false;
}

static void publish_state(
    const ornament_state_t *state,
    esp_err_t fetch_error,
    TickType_t last_done_tick,
    bool have_last_done_tick,
    bool have_state)
{
    if (shared_state_mutex == NULL || state == NULL) {
        return;
    }
    if (xSemaphoreTake(shared_state_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        ESP_LOGW(TAG, "shared state publish skipped: mutex timeout");
        return;
    }
    shared_state.state = *state;
    shared_state.fetch_error = fetch_error;
    shared_state.last_done_tick = last_done_tick;
    shared_state.have_last_done_tick = have_last_done_tick;
    shared_state.have_state = have_state;
    xSemaphoreGive(shared_state_mutex);
}

static bool snapshot_state(
    ornament_state_t *state,
    esp_err_t *fetch_error,
    TickType_t *last_done_tick,
    bool *have_last_done_tick)
{
    if (shared_state_mutex == NULL || state == NULL || fetch_error == NULL ||
        last_done_tick == NULL || have_last_done_tick == NULL) {
        return false;
    }
    if (xSemaphoreTake(shared_state_mutex, pdMS_TO_TICKS(20)) != pdTRUE) {
        return false;
    }
    bool have_state = shared_state.have_state;
    *state = shared_state.state;
    *fetch_error = shared_state.fetch_error;
    *last_done_tick = shared_state.last_done_tick;
    *have_last_done_tick = shared_state.have_last_done_tick;
    xSemaphoreGive(shared_state_mutex);
    return have_state;
}

static bool auto_match_bridge(bool verify_current, const char *reason)
{
    bridge_auto_match_result_t result;
    esp_err_t err = bridge_client_auto_match(verify_current, &result);
    web_console_bridge_debug_t debug = {0};
    debug.last_auto_match_error = err;
    debug.last_auto_match_ok = (err == ESP_OK);
    strlcpy(debug.last_auto_match_reason, reason, sizeof(debug.last_auto_match_reason));
    web_console_set_bridge_debug(&debug);
    if (err == ESP_OK) {
        ESP_LOGI(
            TAG,
            "bridge auto-match ok: source=%s url=%s saved=%s tested=%d reason=%s",
            result.source,
            result.bridge_url,
            result.saved ? "yes" : "no",
            result.tested_count,
            reason);
        return true;
    }

    ESP_LOGW(
        TAG,
        "bridge auto-match failed: err=%s tested=%d reason=%s",
        esp_err_to_name(err),
        result.tested_count,
        reason);
    return false;
}

static void asrpro_voice_command_received(asrpro_voice_command_t command, void *context)
{
    (void)context;
    if (voice_command_queue == NULL) {
        ESP_LOGW(TAG, "voice command dropped before queue ready: %s", asrpro_voice_command_name(command));
        return;
    }
    if (xQueueSend(voice_command_queue, &command, 0) != pdTRUE) {
        asrpro_voice_command_t dropped;
        if (xQueueReceive(voice_command_queue, &dropped, 0) == pdTRUE) {
            ESP_LOGW(
                TAG,
                "voice command queue full, dropped oldest: %s",
                asrpro_voice_command_name(dropped));
            (void)xQueueSend(voice_command_queue, &command, 0);
        } else {
            ESP_LOGW(TAG, "voice command dropped: %s", asrpro_voice_command_name(command));
        }
    }
}

static void handle_voice_command(asrpro_voice_command_t command)
{
    TickType_t now = xTaskGetTickCount();
    ESP_LOGI(TAG, "handling voice command: %s", asrpro_voice_command_name(command));
    switch (command) {
    case ASRPRO_VOICE_COMMAND_STATUS:
        voice_control_set_view(VOICE_VIEW_STATUS, now, "VOICE STATUS", "STATUS READY");
        break;
    case ASRPRO_VOICE_COMMAND_SHOW_QUOTA:
        voice_control_set_view(VOICE_VIEW_QUOTA, now, "SHOW QUOTA", "QUOTA PAGE");
        break;
    case ASRPRO_VOICE_COMMAND_SHOW_TASKS:
        voice_control_set_view(VOICE_VIEW_TASKS, now, "SHOW TASKS", "TASK PAGE");
        break;
    case ASRPRO_VOICE_COMMAND_SHOW_CLOCK:
        voice_control_set_view(VOICE_VIEW_CLOCK, now, "SHOW CLOCK", "CLOCK PAGE");
        break;
    case ASRPRO_VOICE_COMMAND_REFRESH_STATE:
        voice_control_request_refresh(now);
        voice_control_set_view(VOICE_VIEW_STATUS, now, "REFRESH STATE", "REFRESHING");
        break;
    case ASRPRO_VOICE_COMMAND_BRIDGE_MATCH:
        voice_control_request_bridge_match(now);
        voice_control_set_view(VOICE_VIEW_STATUS, now, "BRIDGE MATCH", "MATCHING");
        break;
    case ASRPRO_VOICE_COMMAND_QUIET_ON:
        voice_control_set_quiet(true);
        voice_control_set_view(VOICE_VIEW_STATUS, now, "QUIET ON", "QUIET ON");
        break;
    case ASRPRO_VOICE_COMMAND_QUIET_OFF:
        voice_control_set_quiet(false);
        voice_control_set_view(VOICE_VIEW_STATUS, now, "QUIET OFF", "QUIET OFF");
        break;
    case ASRPRO_VOICE_COMMAND_XIAOZHI_START: {
        esp_err_t err = xiaozhi_client_start_session();
        voice_control_set_view(
            VOICE_VIEW_STATUS,
            now,
            "XIAOZHI START",
            err == ESP_OK ? "AI LISTEN" : esp_err_to_name(err));
        break;
    }
    case ASRPRO_VOICE_COMMAND_XIAOZHI_STOP: {
        esp_err_t err = xiaozhi_client_stop_session();
        voice_control_set_view(
            VOICE_VIEW_STATUS,
            now,
            "XIAOZHI STOP",
            err == ESP_OK ? "AI STOP" : esp_err_to_name(err));
        break;
    }
    case ASRPRO_VOICE_COMMAND_UNKNOWN:
    default:
        voice_control_set_view(VOICE_VIEW_STATUS, now, "UNKNOWN", "UNKNOWN CMD");
        break;
    }
}

static bool page_button_pressed_level(int level)
{
    return ORNAMENT_PAGE_BUTTON_ACTIVE_LOW ? level == 0 : level != 0;
}

static void page_button_task(void *arg)
{
    (void)arg;
#if CONFIG_ORNAMENT_PAGE_BUTTON_ENABLED
    const gpio_num_t gpio = (gpio_num_t)CONFIG_ORNAMENT_PAGE_BUTTON_GPIO;
    const gpio_config_t config = {
        .pin_bit_mask = 1ULL << gpio,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = ORNAMENT_PAGE_BUTTON_ACTIVE_LOW ? GPIO_PULLUP_ENABLE : GPIO_PULLUP_DISABLE,
        .pull_down_en = ORNAMENT_PAGE_BUTTON_ACTIVE_LOW ? GPIO_PULLDOWN_DISABLE : GPIO_PULLDOWN_ENABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&config));

    const TickType_t poll_ticks = pdMS_TO_TICKS(20);
    const TickType_t debounce_ticks = pdMS_TO_TICKS(CONFIG_ORNAMENT_PAGE_BUTTON_DEBOUNCE_MS);
    bool stable_pressed = page_button_pressed_level(gpio_get_level(gpio));
    bool last_sample_pressed = stable_pressed;
    TickType_t changed_tick = xTaskGetTickCount();

    ESP_LOGI(
        TAG,
        "page button enabled: gpio=%d active_%s debounce=%dms",
        CONFIG_ORNAMENT_PAGE_BUTTON_GPIO,
        ORNAMENT_PAGE_BUTTON_ACTIVE_LOW ? "low" : "high",
        CONFIG_ORNAMENT_PAGE_BUTTON_DEBOUNCE_MS);

    while (true) {
        TickType_t now = xTaskGetTickCount();
        bool sample_pressed = page_button_pressed_level(gpio_get_level(gpio));
        if (sample_pressed != last_sample_pressed) {
            last_sample_pressed = sample_pressed;
            changed_tick = now;
        }
        if (sample_pressed != stable_pressed && (now - changed_tick) >= debounce_ticks) {
            stable_pressed = sample_pressed;
            if (stable_pressed) {
                voice_control_cycle_page(now);
            }
        }
        vTaskDelay(poll_ticks);
    }
#else
    vTaskDelete(NULL);
#endif
}

static void voice_command_task(void *arg)
{
    (void)arg;
    asrpro_voice_command_t command;
    while (true) {
        if (xQueueReceive(voice_command_queue, &command, portMAX_DELAY) == pdTRUE) {
            handle_voice_command(command);
        }
    }
}

static void create_app_task(
    TaskFunction_t task_fn,
    const char *name,
    uint32_t stack_depth,
    UBaseType_t priority)
{
    BaseType_t created = xTaskCreate(task_fn, name, stack_depth, NULL, priority, NULL);
    ESP_ERROR_CHECK(created == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);
}

static void poll_task(void *arg)
{
    (void)arg;
    ornament_state_t fetched_state;
    ornament_state_init(&fetched_state);
    TickType_t last_done_tick = 0;
    TickType_t next_bridge_poll = 0;
    int last_done_seq = 0;
    int previous_active_task_count = 0;
    int consecutive_fetch_failures = 0;
    bool have_seen_state = false;
    bool have_last_done_tick = false;
    bool quota_refresh_pending = false;
    TickType_t next_auto_match = 0;

    while (true) {
        TickType_t now = xTaskGetTickCount();
        TickType_t request_tick = 0;

        if (voice_control_take_bridge_match(&request_tick)) {
            (void)request_tick;
            voice_control_set_result("MATCHING");
            if (auto_match_bridge(true, "voice command")) {
                voice_control_set_result("BRIDGE MATCHED");
                next_bridge_poll = now;
            } else {
                voice_control_set_result("MATCH FAILED");
            }
        }

        bool refresh_requested = voice_control_take_refresh(&request_tick);
        if (refresh_requested) {
            voice_control_set_result("REFRESHING");
        }

        if (!quota_refresh_pending && quota_reset_refresh_needed(&fetched_state)) {
            quota_refresh_pending = true;
            next_bridge_poll = now;
        }

        if (refresh_requested || now >= next_bridge_poll) {
            bool was_refresh_pending = quota_refresh_pending;
            next_bridge_poll = now + pdMS_TO_TICKS(CONFIG_ORNAMENT_POLL_INTERVAL_MS);
            esp_err_t err = bridge_client_fetch_state(&fetched_state);
            if (quota_reset_refresh_needed(&fetched_state)) {
                quota_refresh_pending = true;
            } else if (err == ESP_OK && quota_refresh_pending && quota_reset_times_current(&fetched_state)) {
                quota_refresh_pending = false;
            }
            if (quota_refresh_pending) {
                hide_expired_quota_reset_times(&fetched_state);
                next_bridge_poll = now + expired_quota_retry_ticks();
            }
            web_console_bridge_debug_t debug = {
                .last_fetch_error = err,
                .last_auto_match_error = ESP_ERR_INVALID_STATE,
                .consecutive_fetch_failures = consecutive_fetch_failures,
                .last_success_ms = 0,
                .last_failure_ms = 0,
                .last_auto_match_ok = false,
            };
            if (err == ESP_OK) {
                if (consecutive_fetch_failures > 0) {
                    ESP_LOGI(TAG, "bridge fetch recovered after %d failure(s)", consecutive_fetch_failures);
                }
                consecutive_fetch_failures = 0;
                debug.consecutive_fetch_failures = 0;
                debug.last_success_ms = ticks_to_ms(now);
                if (refresh_requested) {
                    voice_control_set_result("REFRESH OK");
                }
                if (fetched_state.done_seq < last_done_seq) {
                    last_done_seq = fetched_state.done_seq;
                }
                if (should_announce_done_event(
                        &fetched_state,
                        have_seen_state,
                        last_done_seq,
                        previous_active_task_count)) {
                    last_done_tick = now;
                    have_last_done_tick = true;
                    if (!voice_control_quiet_mode()) {
                        asrpro_link_notify_done();
                        task_audio_play_done();
                    }
                }
                if (fetched_state.done_seq > last_done_seq) {
                    last_done_seq = fetched_state.done_seq;
                }
                previous_active_task_count = fetched_state.active_task_count;
                have_seen_state = true;
                fetched_state.bridge_offline = false;
                web_console_set_bridge_debug(&debug);
                publish_state(&fetched_state, err, last_done_tick, have_last_done_tick, true);
            } else {
                consecutive_fetch_failures++;
                debug.consecutive_fetch_failures = consecutive_fetch_failures;
                debug.last_failure_ms = ticks_to_ms(now);
                if (refresh_requested) {
                    voice_control_set_result("REFRESH FAIL");
                }
                ESP_LOGW(
                    TAG,
                    "failed to fetch bridge state (%d/%d): %s",
                    consecutive_fetch_failures,
                    CONFIG_ORNAMENT_BRIDGE_OFFLINE_FAILURES,
                    esp_err_to_name(err));
                if (now >= next_auto_match) {
                    next_auto_match = now + pdMS_TO_TICKS(CONFIG_ORNAMENT_BRIDGE_AUTO_MATCH_RETRY_MS);
                    if (auto_match_bridge(false, "poll failure")) {
                        next_bridge_poll = now;
                        vTaskDelay(pdMS_TO_TICKS(200));
                        continue;
                    }
                }

                web_console_set_bridge_debug(&debug);
                fetched_state.bridge_offline = bridge_is_offline(consecutive_fetch_failures, err);
                if (was_refresh_pending || quota_refresh_pending) {
                    hide_expired_quota_reset_times(&fetched_state);
                }
                if (have_seen_state &&
                    consecutive_fetch_failures < CONFIG_ORNAMENT_BRIDGE_OFFLINE_FAILURES &&
                    shared_state_has_displayable_snapshot()) {
                    publish_state(&fetched_state, ESP_OK, last_done_tick, have_last_done_tick, true);
                } else if (have_seen_state) {
                    publish_state(&fetched_state, err, last_done_tick, have_last_done_tick, true);
                } else {
                    ornament_state_t error_state;
                    ornament_state_init(&error_state);
                    error_state.status = ORNAMENT_STATUS_IDLE;
                    error_state.bridge_offline = bridge_is_offline(consecutive_fetch_failures, err);
                    publish_state(&error_state, err, last_done_tick, have_last_done_tick, true);
                }
            }
        }

        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

static void ui_render_task(void *arg)
{
    (void)arg;
    ornament_state_t state;
    TickType_t idle_since_tick = 0;
    TickType_t last_done_tick = 0;
    bool have_last_done_tick = false;
    bool previous_standby_eligible = false;
    esp_err_t fetch_error = ESP_OK;

    while (true) {
        TickType_t now = xTaskGetTickCount();
        if (!snapshot_state(&state, &fetch_error, &last_done_tick, &have_last_done_tick)) {
            vTaskDelay(pdMS_TO_TICKS(CONFIG_ORNAMENT_UI_FRAME_MS));
            continue;
        }

        update_local_animation(&state, now, last_done_tick, have_last_done_tick);
        apply_local_state(&state, state.bridge_offline);
        web_console_set_last_state(&state, fetch_error);
        voice_control_state_t voice_state;
        bool have_voice_state = voice_control_snapshot(&voice_state);

        bool standby_eligible = standby_timer_eligible(&state);
        if (standby_eligible) {
            if (!previous_standby_eligible || idle_since_tick == 0) {
                idle_since_tick = now;
            }
        } else {
            idle_since_tick = 0;
        }
        previous_standby_eligible = standby_eligible;

        if (!(have_voice_state && render_voice_override(&state, fetch_error, &voice_state, now))) {
            render_current_state(&state, idle_since_tick, now);
        }

        vTaskDelay(pdMS_TO_TICKS(CONFIG_ORNAMENT_UI_FRAME_MS));
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "starting Codex ornament");
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    display_init();
    display_render_boot();
    esp_err_t audio_err = task_audio_start();
    if (audio_err != ESP_OK) {
        ESP_LOGW(TAG, "task done audio unavailable: %s", esp_err_to_name(audio_err));
    }

    if (wifi_connect() != ESP_OK) {
        ESP_LOGW(TAG, "provisioning mode active: SSID=%s URL=http://192.168.4.1", config_portal_ssid());
        display_render_status("Setup AP: 192.168.4.1");
        while (true) {
            vTaskDelay(pdMS_TO_TICKS(60000));
        }
    }
    display_render_status("Wi-Fi connected");
    voice_control_init();
    voice_command_queue = xQueueCreate(4, sizeof(asrpro_voice_command_t));
    ESP_ERROR_CHECK(voice_command_queue == NULL ? ESP_ERR_NO_MEM : ESP_OK);
    if (CONFIG_ORNAMENT_BRIDGE_AUTO_MATCH_ON_BOOT) {
        display_render_status("Matching bridge...");
        if (auto_match_bridge(true, "boot")) {
            display_render_status("Bridge matched");
        } else {
            display_render_status("Bridge search failed");
        }
        vTaskDelay(pdMS_TO_TICKS(600));
    }

    esp_err_t asrpro_err = asrpro_link_init(asrpro_voice_command_received, NULL);
    if (asrpro_err != ESP_OK) {
        ESP_LOGW(TAG, "ASRPRO UART link unavailable: %s", esp_err_to_name(asrpro_err));
    }
    esp_err_t xiaozhi_err = xiaozhi_client_init();
    if (xiaozhi_err != ESP_OK) {
        ESP_LOGW(TAG, "Xiaozhi client unavailable: %s", esp_err_to_name(xiaozhi_err));
    } else if (CONFIG_ORNAMENT_XIAOZHI_AUTO_START) {
        xiaozhi_err = xiaozhi_client_start_session();
        if (xiaozhi_err != ESP_OK) {
            ESP_LOGW(TAG, "Xiaozhi auto-start skipped: %s", esp_err_to_name(xiaozhi_err));
        }
    }
    system_status_start_time_sync();
    ESP_ERROR_CHECK(weather_client_start());
    ESP_ERROR_CHECK(web_console_start());

    shared_state_mutex = xSemaphoreCreateMutex();
    ESP_ERROR_CHECK(shared_state_mutex == NULL ? ESP_ERR_NO_MEM : ESP_OK);
    memset(&shared_state, 0, sizeof(shared_state));
    ornament_state_init(&shared_state.state);
    shared_state.fetch_error = ESP_ERR_INVALID_STATE;

    create_app_task(voice_command_task, "voice_cmd", 4096, 5);
    create_app_task(page_button_task, "page_button", 3072, 5);
    create_app_task(poll_task, "bridge_poll", 8192, 5);
    create_app_task(ui_render_task, "ui_render", 8192, 4);

    /* Main task must never return in ESP-IDF — returning tears down
     * FreeRTOS resources (mutexes, queues, semaphores) that child tasks
     * still depend on, causing StoreProhibited panics.
     */
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(60000));
    }
}
