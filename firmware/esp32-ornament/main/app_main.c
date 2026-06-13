#include "bridge_client.h"
#include "config_portal.h"
#include "display.h"
#include "music_player.h"
#include "ornament_state.h"
#include "system_status.h"
#include "standby_wallpaper_client.h"
#include "task_audio.h"
#include "web_console.h"
#include "weather_client.h"
#include "wifi.h"
#include "xiaozhi_client.h"

#if CONFIG_ORNAMENT_WEB_CONSOLE_ENABLED
#include "system_diagnostics.h"
#endif

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "freertos/idf_additions.h"
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

#ifndef CONFIG_ORNAMENT_PAGE_BUTTON_MIN_HOLD_MS
#define CONFIG_ORNAMENT_PAGE_BUTTON_MIN_HOLD_MS 0
#endif

#ifndef CONFIG_ORNAMENT_PAGE_BUTTON_REPEAT_GUARD_MS
#define CONFIG_ORNAMENT_PAGE_BUTTON_REPEAT_GUARD_MS 250
#endif

#ifdef CONFIG_ORNAMENT_PAGE_BUTTON_ACTIVE_LOW
#define ORNAMENT_PAGE_BUTTON_ACTIVE_LOW 1
#else
#define ORNAMENT_PAGE_BUTTON_ACTIVE_LOW 0
#endif

#ifndef CONFIG_ORNAMENT_AI_BUTTON_ENABLED
#define CONFIG_ORNAMENT_AI_BUTTON_ENABLED 0
#endif

#ifndef CONFIG_ORNAMENT_AI_BUTTON_GPIO
#define CONFIG_ORNAMENT_AI_BUTTON_GPIO 16
#endif

#ifndef CONFIG_ORNAMENT_AI_BUTTON_DEBOUNCE_MS
#define CONFIG_ORNAMENT_AI_BUTTON_DEBOUNCE_MS 50
#endif

#ifndef CONFIG_ORNAMENT_AI_BUTTON_MIN_HOLD_MS
#define CONFIG_ORNAMENT_AI_BUTTON_MIN_HOLD_MS 0
#endif

#ifndef CONFIG_ORNAMENT_AI_BUTTON_REPEAT_GUARD_MS
#define CONFIG_ORNAMENT_AI_BUTTON_REPEAT_GUARD_MS 250
#endif

#ifdef CONFIG_ORNAMENT_AI_BUTTON_ACTIVE_LOW
#define ORNAMENT_AI_BUTTON_ACTIVE_LOW 1
#else
#define ORNAMENT_AI_BUTTON_ACTIVE_LOW 0
#endif

static const char *TAG = "ornament";

#define VOICE_PAGE_HOLD_MS 12000
#define XIAOZHI_PAGE_FOCUS_MS 12000
#define VOICE_STATUS_TEXT_MAX 40
#define ORNAMENT_MIN_VALID_EPOCH 1577836800LL
#define BRIDGE_SINGLE_RETRY_DELAY_MS 1000
#define BRIDGE_POLL_TASK_STACK 12288
#define PAGE_BUTTON_TASK_STACK 4096
#define AI_BUTTON_TASK_STACK 8192
#define UI_RENDER_TASK_STACK 24576
#define UI_RENDER_LOW_STACK_WARN_BYTES 2048
#define BUTTON_MIN_DEBOUNCE_MS 10

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
    bool manual_view;
    char last_command[VOICE_STATUS_TEXT_MAX];
    char last_result[VOICE_STATUS_TEXT_MAX];
} voice_control_state_t;

typedef enum {
    XIAOZHI_SESSION_ACTION_NONE = 0,
    XIAOZHI_SESSION_ACTION_START,
    XIAOZHI_SESSION_ACTION_STOP,
    XIAOZHI_SESSION_ACTION_TOGGLE,
} xiaozhi_session_action_t;

typedef struct {
    xiaozhi_session_action_t action;
    const char *reason;
} xiaozhi_session_request_t;

static SemaphoreHandle_t shared_state_mutex;
static ornament_shared_state_t shared_state;
static SemaphoreHandle_t voice_control_mutex;
static voice_control_state_t voice_control;
static QueueHandle_t xiaozhi_session_queue;
static music_player_snapshot_t ui_music_snapshot;

static void log_heap_status(const char *stage)
{
#if CONFIG_ORNAMENT_WEB_CONSOLE_ENABLED
    system_diagnostics_record_heap(stage);
#endif
    ESP_LOGI(
        TAG,
        "heap %s: free=%u min=%u largest8=%u internal=%u spiram=%u",
        stage,
        (unsigned int)esp_get_free_heap_size(),
        (unsigned int)esp_get_minimum_free_heap_size(),
        (unsigned int)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT),
        (unsigned int)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
        (unsigned int)heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
}

static void publish_state(
    const ornament_state_t *state,
    esp_err_t fetch_error,
    TickType_t last_done_tick,
    bool have_last_done_tick,
    bool have_state);

static bool standby_timer_eligible(const ornament_state_t *state)
{
    if (state == NULL || state->active_task_count > 0) {
        return false;
    }
    ornament_status_t status = ornament_state_panel_status(state);
    return status != ORNAMENT_STATUS_RUNNING && status != ORNAMENT_STATUS_ERROR;
}

static bool automatic_state_page_active(const ornament_state_t *state)
{
    if (state == NULL) {
        return false;
    }
    if (state->active_task_count > 0 || state->done_flash_active) {
        return true;
    }

    ornament_status_t status = ornament_state_panel_status(state);
    return status == ORNAMENT_STATUS_RUNNING || status == ORNAMENT_STATUS_ERROR;
}

static bool should_show_standby_clock(const ornament_state_t *state, TickType_t idle_since_tick, TickType_t now)
{
    if (state != NULL && state->bridge_offline && state->active_task_count == 0) {
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

static TickType_t bridge_recovery_retry_ticks(void)
{
    int retry_ms = CONFIG_ORNAMENT_BRIDGE_AUTO_MATCH_RETRY_MS;
    if (retry_ms <= 0) {
        retry_ms = 30000;
    }
    return pdMS_TO_TICKS(retry_ms);
}

static void clear_task_snapshot(ornament_state_t *state)
{
    if (state == NULL) {
        return;
    }

    state->status = ORNAMENT_STATUS_IDLE;
    state->has_task = false;
    state->active_task_count = 0;
    state->task_title[0] = '\0';
    state->task_message[0] = '\0';
    state->task_received_at[0] = '\0';
    state->task_session_id[0] = '\0';
    state->task_turn_id[0] = '\0';

    state->has_codex_task = false;
    state->has_claude_task = false;
    state->codex_active_task_count = 0;
    state->claude_active_task_count = 0;
    state->codex_task_title[0] = '\0';
    state->codex_task_message[0] = '\0';
    state->codex_task_session_id[0] = '\0';
    state->codex_task_turn_id[0] = '\0';
    state->claude_task_title[0] = '\0';
    state->claude_task_message[0] = '\0';
    state->claude_task_session_id[0] = '\0';
    state->claude_task_turn_id[0] = '\0';
    if (state->codex_task_status == ORNAMENT_STATUS_RUNNING) {
        state->codex_task_status = ORNAMENT_STATUS_IDLE;
    }
    if (state->claude_task_status == ORNAMENT_STATUS_RUNNING) {
        state->claude_task_status = ORNAMENT_STATUS_IDLE;
    }
}

static void set_bridge_connection_state(ornament_state_t *state, bool offline, bool reconnecting)
{
    if (state == NULL) {
        return;
    }

    state->bridge_offline = offline;
    state->bridge_reconnecting = reconnecting;
    if (offline) {
        clear_task_snapshot(state);
    }
}

static void publish_bridge_fallback_state(
    const ornament_state_t *cached_state,
    bool have_cached_state,
    esp_err_t fetch_error,
    TickType_t last_done_tick,
    bool have_last_done_tick,
    bool bridge_offline)
{
    ornament_state_t fallback_state;
    if (have_cached_state && cached_state != NULL) {
        fallback_state = *cached_state;
    } else {
        ornament_state_init(&fallback_state);
    }

    set_bridge_connection_state(&fallback_state, bridge_offline, true);
    publish_state(&fallback_state, fetch_error, last_done_tick, have_last_done_tick, true);
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
        return "STANDBY";
    case VOICE_VIEW_XIAOZHI:
        return "XIAOZHI";
    default:
        return "UNKNOWN";
    }
}

static bool voice_view_in_page_button_cycle(voice_view_t view)
{
    return view == VOICE_VIEW_CLOCK ||
           view == VOICE_VIEW_QUOTA ||
           view == VOICE_VIEW_XIAOZHI;
}

static voice_view_t next_button_view(voice_view_t current)
{
    switch (current) {
    case VOICE_VIEW_AUTO:
        return VOICE_VIEW_CLOCK;
    case VOICE_VIEW_CLOCK:
        return VOICE_VIEW_QUOTA;
    case VOICE_VIEW_QUOTA:
        return VOICE_VIEW_XIAOZHI;
    case VOICE_VIEW_XIAOZHI:
        return VOICE_VIEW_CLOCK;
    case VOICE_VIEW_STATUS:
    case VOICE_VIEW_TASKS:
    default:
        return VOICE_VIEW_CLOCK;
    }
}

static bool voice_view_active(voice_view_t view, TickType_t hold_until_tick, TickType_t now)
{
    return view != VOICE_VIEW_AUTO &&
           (hold_until_tick == portMAX_DELAY || (hold_until_tick != 0 && now < hold_until_tick));
}

static bool voice_control_active_view(TickType_t now, voice_view_t *view, bool *manual_active)
{
    if (voice_control_mutex == NULL) {
        return false;
    }
    if (xSemaphoreTake(voice_control_mutex, pdMS_TO_TICKS(20)) != pdTRUE) {
        return false;
    }

    bool active = voice_view_active(voice_control.view, voice_control.hold_until_tick, now);
    if (view != NULL) {
        *view = active ? voice_control.view : VOICE_VIEW_AUTO;
    }
    if (manual_active != NULL) {
        *manual_active = voice_control.manual_view && active;
    }

    xSemaphoreGive(voice_control_mutex);
    return true;
}

static void queue_xiaozhi_session_action(xiaozhi_session_action_t action, const char *reason)
{
    if (action == XIAOZHI_SESSION_ACTION_NONE || xiaozhi_session_queue == NULL) {
        return;
    }

    const xiaozhi_session_request_t request = {
        .action = action,
        .reason = reason,
    };
    if (xQueueSend(xiaozhi_session_queue, &request, 0) == pdTRUE) {
        ESP_LOGI(
            TAG,
            "queued Xiaozhi action=%d reason=%s",
            action,
            reason != NULL ? reason : "<none>");
        return;
    }

    xiaozhi_session_request_t dropped = {0};
    if (xQueueReceive(xiaozhi_session_queue, &dropped, 0) == pdTRUE &&
        xQueueSend(xiaozhi_session_queue, &request, 0) == pdTRUE) {
        ESP_LOGW(
            TAG,
            "Xiaozhi session queue full, dropped oldest action=%d reason=%s",
            dropped.action,
            dropped.reason != NULL ? dropped.reason : "<none>");
        return;
    }

    ESP_LOGW(
        TAG,
        "Xiaozhi session action dropped action=%d reason=%s",
        action,
        reason != NULL ? reason : "<none>");
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
    voice_control.manual_view = false;
    if (command != NULL) {
        strlcpy(voice_control.last_command, command, sizeof(voice_control.last_command));
    }
    if (result != NULL) {
        strlcpy(voice_control.last_result, result, sizeof(voice_control.last_result));
    }
    xSemaphoreGive(voice_control_mutex);
}

static esp_err_t xiaozhi_start_session_if_needed(const char *reason)
{
    if (music_player_is_active()) {
        ESP_LOGI(TAG, "Xiaozhi session start ignored while music is playing: %s", reason);
        return ESP_ERR_INVALID_STATE;
    }
    if (xiaozhi_client_session_requested()) {
        ESP_LOGI(TAG, "Xiaozhi session already active: %s", reason);
        return ESP_OK;
    }

    /*
     * The websocket session can still be tearing down after a manual stop,
     * while session_requested is already false. Reconnect handles both the
     * clean idle case and the stale-runtime case.
     */
    esp_err_t err = xiaozhi_client_reconnect_session(true);
    if (err == ESP_ERR_INVALID_STATE) {
        if (xiaozhi_client_session_requested()) {
            err = ESP_OK;
        }
    }

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Xiaozhi session start requested: %s", reason);
    } else {
        ESP_LOGW(TAG, "Xiaozhi session start failed (%s): %s", reason, esp_err_to_name(err));
    }
    return err;
}

static esp_err_t xiaozhi_start_session_after_music_stop(const char *reason)
{
    if (music_player_is_active()) {
        ESP_LOGI(TAG, "stopping music before Xiaozhi session: %s", reason != NULL ? reason : "<none>");
        music_player_request_stop();
        esp_err_t stop_err = music_player_stop();
        if (stop_err != ESP_OK) {
            ESP_LOGW(TAG, "music stop before Xiaozhi failed: %s", esp_err_to_name(stop_err));
            return stop_err;
        }
    }

    return xiaozhi_start_session_if_needed(reason);
}

static esp_err_t xiaozhi_stop_session_if_needed(const char *reason)
{
    esp_err_t err = xiaozhi_client_stop_session();
    if (err == ESP_ERR_INVALID_STATE) {
        if (!xiaozhi_client_session_requested()) {
            err = ESP_OK;
        }
    }

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Xiaozhi session stop requested: %s", reason);
    } else {
        ESP_LOGW(TAG, "Xiaozhi session stop failed (%s): %s", reason, esp_err_to_name(err));
    }
    return err;
}

static bool xiaozhi_snapshot_active(const xiaozhi_client_snapshot_t *snapshot)
{
    if (snapshot == NULL) {
        return false;
    }
    return snapshot->session_requested ||
           snapshot->state == XIAOZHI_CLIENT_STATE_CONNECTING ||
           snapshot->state == XIAOZHI_CLIENT_STATE_LISTENING ||
           snapshot->state == XIAOZHI_CLIENT_STATE_SPEAKING;
}

static void voice_control_cycle_page(TickType_t now)
{
    xiaozhi_session_action_t xiaozhi_action = XIAOZHI_SESSION_ACTION_NONE;
    voice_view_t next_view = VOICE_VIEW_CLOCK;

    if (voice_control_mutex == NULL) {
        return;
    }
    if (xSemaphoreTake(voice_control_mutex, pdMS_TO_TICKS(50)) != pdTRUE) {
        ESP_LOGW(TAG, "page cycle skipped: mutex timeout");
        return;
    }
    voice_view_t current = voice_control.manual_view &&
                                   voice_view_active(voice_control.view, voice_control.hold_until_tick, now) &&
                                   voice_view_in_page_button_cycle(voice_control.view)
                               ? voice_control.view
                               : VOICE_VIEW_AUTO;
    next_view = next_button_view(current);
    if (current != VOICE_VIEW_XIAOZHI && next_view == VOICE_VIEW_XIAOZHI) {
        xiaozhi_action = XIAOZHI_SESSION_ACTION_START;
    } else if (current == VOICE_VIEW_XIAOZHI && next_view != VOICE_VIEW_XIAOZHI) {
        xiaozhi_action = XIAOZHI_SESSION_ACTION_STOP;
    }

    voice_control.view = next_view;
    voice_control.hold_until_tick = portMAX_DELAY;
    voice_control.manual_view = true;
    strlcpy(voice_control.last_command, "PAGE BUTTON", sizeof(voice_control.last_command));
    snprintf(
        voice_control.last_result,
        sizeof(voice_control.last_result),
        "PAGE %s",
        voice_view_name(next_view));
    ESP_LOGI(TAG, "page button switched to %s at tick %lu", voice_view_name(next_view), (unsigned long)now);
    xSemaphoreGive(voice_control_mutex);

    if (xiaozhi_action == XIAOZHI_SESSION_ACTION_START) {
        queue_xiaozhi_session_action(XIAOZHI_SESSION_ACTION_START, "page button entered Xiaozhi page");
    } else if (xiaozhi_action == XIAOZHI_SESSION_ACTION_STOP) {
        queue_xiaozhi_session_action(XIAOZHI_SESSION_ACTION_STOP, "page button left Xiaozhi page");
    }
}

static bool xiaozhi_text_changed(const char *current, const char *previous)
{
    const char *current_text = current != NULL ? current : "";
    const char *previous_text = previous != NULL ? previous : "";
    return current_text[0] != '\0' && strcmp(current_text, previous_text) != 0;
}

static bool xiaozhi_should_refresh_page_focus(
    const xiaozhi_client_snapshot_t *snapshot,
    const xiaozhi_client_snapshot_t *previous_snapshot,
    bool have_previous_snapshot)
{
    if (snapshot == NULL) {
        return false;
    }

    if (!have_previous_snapshot) {
        return snapshot->session_requested ||
               snapshot->activation_pending ||
               snapshot->state == XIAOZHI_CLIENT_STATE_CONNECTING ||
               snapshot->state == XIAOZHI_CLIENT_STATE_SPEAKING;
    }

    if ((snapshot->activation_pending && !previous_snapshot->activation_pending) ||
        (snapshot->session_requested && !previous_snapshot->session_requested)) {
        return true;
    }

    if (snapshot->state != previous_snapshot->state) {
        switch (snapshot->state) {
        case XIAOZHI_CLIENT_STATE_CONNECTING:
        case XIAOZHI_CLIENT_STATE_SPEAKING:
        case XIAOZHI_CLIENT_STATE_ERROR:
            return true;
        case XIAOZHI_CLIENT_STATE_LISTENING:
            return previous_snapshot->state == XIAOZHI_CLIENT_STATE_CONNECTING ||
                   previous_snapshot->state == XIAOZHI_CLIENT_STATE_SPEAKING;
        case XIAOZHI_CLIENT_STATE_DISABLED:
        case XIAOZHI_CLIENT_STATE_IDLE:
        case XIAOZHI_CLIENT_STATE_CONFIG_MISSING:
        default:
            break;
        }
    }

    return xiaozhi_text_changed(snapshot->last_stt, previous_snapshot->last_stt) ||
           xiaozhi_text_changed(snapshot->last_tts, previous_snapshot->last_tts);
}

static bool xiaozhi_session_page_active(
    const xiaozhi_client_snapshot_t *snapshot,
    TickType_t focus_until_tick,
    TickType_t now)
{
    if (snapshot == NULL) {
        return false;
    }
    if (snapshot->activation_pending) {
        return true;
    }
    switch (snapshot->state) {
    case XIAOZHI_CLIENT_STATE_CONNECTING:
    case XIAOZHI_CLIENT_STATE_SPEAKING:
        return true;
    case XIAOZHI_CLIENT_STATE_LISTENING:
        if (snapshot->session_requested || snapshot->connected) {
            return true;
        }
        return focus_until_tick != 0 && now < focus_until_tick;
    case XIAOZHI_CLIENT_STATE_ERROR:
        return focus_until_tick != 0 && now < focus_until_tick;
    case XIAOZHI_CLIENT_STATE_DISABLED:
    case XIAOZHI_CLIENT_STATE_IDLE:
    case XIAOZHI_CLIENT_STATE_CONFIG_MISSING:
    default:
        return false;
    }
}

static bool render_voice_override(
    const ornament_state_t *state,
    esp_err_t fetch_error,
    const xiaozhi_client_snapshot_t *xiaozhi_snapshot,
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
    case VOICE_VIEW_XIAOZHI:
        display_render_xiaozhi(state, xiaozhi_snapshot);
        break;
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

static bool should_announce_done_event(
    const ornament_state_t *state,
    bool have_seen_state,
    int last_done_seq,
    int previous_active_task_count)
{
    if (!is_new_done_event(state, have_seen_state, last_done_seq)) {
        return false;
    }
    ESP_LOGI(
        TAG,
        "announce done alert: done_seq %d -> %d active_task_count %d -> %d",
        last_done_seq,
        state->done_seq,
        previous_active_task_count,
        state->active_task_count);
    return true;
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

static bool voice_manual_xiaozhi_page_active(const voice_control_state_t *voice_state, TickType_t now)
{
    return voice_state != NULL &&
           voice_state->manual_view &&
           voice_state->view == VOICE_VIEW_XIAOZHI &&
           voice_view_active(voice_state->view, voice_state->hold_until_tick, now);
}

#if CONFIG_ORNAMENT_PAGE_BUTTON_ENABLED || CONFIG_ORNAMENT_AI_BUTTON_ENABLED
static bool button_pressed_level(int level, bool active_low)
{
    return active_low ? level == 0 : level != 0;
}

static uint32_t button_effective_debounce_ms(int configured_ms)
{
    if (configured_ms < BUTTON_MIN_DEBOUNCE_MS) {
        return BUTTON_MIN_DEBOUNCE_MS;
    }
    return (uint32_t)configured_ms;
}

typedef struct {
    const char *name;
    gpio_num_t gpio;
    bool active_low;
    uint32_t debounce_ms;
    uint32_t min_hold_ms;
    uint32_t repeat_guard_ms;
    TickType_t debounce_ticks;
    TickType_t min_hold_ticks;
    TickType_t repeat_guard_ticks;
    bool stable_pressed;
    bool last_sample_pressed;
    bool armed;
    bool press_consumed;
    TickType_t changed_tick;
    TickType_t pressed_tick;
    TickType_t last_action_tick;
} button_filter_t;

static esp_err_t configure_button_gpio(gpio_num_t gpio, bool active_low)
{
    const gpio_config_t config = {
        .pin_bit_mask = 1ULL << gpio,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = active_low ? GPIO_PULLUP_ENABLE : GPIO_PULLUP_DISABLE,
        .pull_down_en = active_low ? GPIO_PULLDOWN_DISABLE : GPIO_PULLDOWN_ENABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    return gpio_config(&config);
}

static void button_filter_init(
    button_filter_t *button,
    const char *name,
    gpio_num_t gpio,
    bool active_low,
    uint32_t debounce_ms,
    uint32_t min_hold_ms,
    uint32_t repeat_guard_ms)
{
    const TickType_t now = xTaskGetTickCount();
    bool pressed = button_pressed_level(gpio_get_level(gpio), active_low);

    *button = (button_filter_t) {
        .name = name,
        .gpio = gpio,
        .active_low = active_low,
        .debounce_ms = debounce_ms,
        .min_hold_ms = min_hold_ms,
        .repeat_guard_ms = repeat_guard_ms,
        .debounce_ticks = pdMS_TO_TICKS(debounce_ms),
        .min_hold_ticks = pdMS_TO_TICKS(min_hold_ms),
        .repeat_guard_ticks = pdMS_TO_TICKS(repeat_guard_ms),
        .stable_pressed = pressed,
        .last_sample_pressed = pressed,
        .armed = !pressed,
        .press_consumed = false,
        .changed_tick = now,
        .pressed_tick = pressed ? now : 0,
        .last_action_tick = 0,
    };

    ESP_LOGI(
        TAG,
        "%s button enabled: gpio=%d active_%s debounce=%lums hold=%lums guard=%lums",
        button->name,
        (int)button->gpio,
        button->active_low ? "low" : "high",
        (unsigned long)button->debounce_ms,
        (unsigned long)button->min_hold_ms,
        (unsigned long)button->repeat_guard_ms);
    if (!button->armed) {
        ESP_LOGW(TAG, "%s button starts pressed; waiting for release before accepting input", button->name);
    }
}

static bool button_filter_poll(button_filter_t *button, TickType_t now)
{
    bool sample_pressed = button_pressed_level(gpio_get_level(button->gpio), button->active_low);
    if (sample_pressed != button->last_sample_pressed) {
        button->last_sample_pressed = sample_pressed;
        button->changed_tick = now;
    }

    if (sample_pressed != button->stable_pressed && (now - button->changed_tick) >= button->debounce_ticks) {
        button->stable_pressed = sample_pressed;
        if (button->stable_pressed) {
            button->pressed_tick = button->changed_tick;
            button->press_consumed = false;
        } else {
            if (!button->press_consumed && button->pressed_tick != 0) {
                ESP_LOGD(TAG, "%s button released before minimum hold", button->name);
            }
            button->pressed_tick = 0;
            button->press_consumed = false;
            button->armed = true;
        }
    }

    if (!button->stable_pressed || !button->armed || button->press_consumed) {
        return false;
    }
    TickType_t held_ticks = now - button->pressed_tick;
    if (held_ticks < button->min_hold_ticks) {
        return false;
    }

    button->press_consumed = true;
    button->armed = false;
    if (button->last_action_tick != 0 && (now - button->last_action_tick) < button->repeat_guard_ticks) {
        ESP_LOGD(TAG, "%s button press ignored within guard interval", button->name);
        return false;
    }

    button->last_action_tick = now;
    ESP_LOGI(TAG, "%s button accepted after %lums hold", button->name, (unsigned long)ticks_to_ms(held_ticks));
    return true;
}
#endif

#if CONFIG_ORNAMENT_PAGE_BUTTON_ENABLED
static void page_button_task(void *arg)
{
    (void)arg;
    const gpio_num_t gpio = (gpio_num_t)CONFIG_ORNAMENT_PAGE_BUTTON_GPIO;
    ESP_ERROR_CHECK(configure_button_gpio(gpio, ORNAMENT_PAGE_BUTTON_ACTIVE_LOW));

    const TickType_t poll_ticks = pdMS_TO_TICKS(20);
    const uint32_t debounce_ms = button_effective_debounce_ms(CONFIG_ORNAMENT_PAGE_BUTTON_DEBOUNCE_MS);
    button_filter_t button;
    button_filter_init(
        &button,
        "page",
        gpio,
        ORNAMENT_PAGE_BUTTON_ACTIVE_LOW,
        debounce_ms,
        CONFIG_ORNAMENT_PAGE_BUTTON_MIN_HOLD_MS,
        CONFIG_ORNAMENT_PAGE_BUTTON_REPEAT_GUARD_MS);
    ESP_LOGI(TAG, "page button cycle=standby->quota->xiaozhi");

    while (true) {
        TickType_t now = xTaskGetTickCount();
        if (button_filter_poll(&button, now)) {
            voice_control_cycle_page(now);
        }
        vTaskDelay(poll_ticks);
    }
}
#endif

#if CONFIG_ORNAMENT_AI_BUTTON_ENABLED
static void ai_button_task(void *arg)
{
    (void)arg;
    const gpio_num_t gpio = (gpio_num_t)CONFIG_ORNAMENT_AI_BUTTON_GPIO;
    ESP_ERROR_CHECK(configure_button_gpio(gpio, ORNAMENT_AI_BUTTON_ACTIVE_LOW));

    const TickType_t poll_ticks = pdMS_TO_TICKS(20);
    const uint32_t debounce_ms = button_effective_debounce_ms(CONFIG_ORNAMENT_AI_BUTTON_DEBOUNCE_MS);
    button_filter_t button;
    button_filter_init(
        &button,
        "AI",
        gpio,
        ORNAMENT_AI_BUTTON_ACTIVE_LOW,
        debounce_ms,
        CONFIG_ORNAMENT_AI_BUTTON_MIN_HOLD_MS,
        CONFIG_ORNAMENT_AI_BUTTON_REPEAT_GUARD_MS);

    while (true) {
        TickType_t now = xTaskGetTickCount();
        if (button_filter_poll(&button, now)) {
            xiaozhi_client_snapshot_t xiaozhi_snapshot = {0};
            xiaozhi_client_status_snapshot(&xiaozhi_snapshot);
            if (xiaozhi_snapshot_active(&xiaozhi_snapshot)) {
                ESP_LOGI(
                    TAG,
                    "AI button interrupting Xiaozhi conversation: state=%s requested=%d",
                    xiaozhi_client_state_name(xiaozhi_snapshot.state),
                    xiaozhi_snapshot.session_requested);
                queue_xiaozhi_session_action(XIAOZHI_SESSION_ACTION_STOP, "AI button interrupt");
                continue;
            }

            voice_view_t active_view = VOICE_VIEW_AUTO;
            bool manual_active = false;
            if (!voice_control_active_view(now, &active_view, &manual_active) ||
                !manual_active ||
                active_view != VOICE_VIEW_XIAOZHI) {
                if (music_player_is_active()) {
                    ESP_LOGI(TAG, "AI button stopping music and returning to Xiaozhi");
                    music_player_request_stop();
                    voice_control_set_view(VOICE_VIEW_XIAOZHI, now, "AI BUTTON", "STOP MUSIC");
                    queue_xiaozhi_session_action(XIAOZHI_SESSION_ACTION_START, "AI button stopped music");
                } else {
                    ESP_LOGD(
                        TAG,
                        "AI button ignored on page=%s manual=%d",
                        voice_view_name(active_view),
                        manual_active);
                }
            } else {
                if (music_player_is_active()) {
                    ESP_LOGI(TAG, "AI button stopping music on Xiaozhi page");
                    music_player_request_stop();
                    voice_control_set_view(VOICE_VIEW_XIAOZHI, now, "AI BUTTON", "STOP MUSIC");
                    queue_xiaozhi_session_action(XIAOZHI_SESSION_ACTION_START, "AI button stopped music on Xiaozhi page");
                    continue;
                }
                ESP_LOGI(TAG, "AI button pressed on Xiaozhi page");
                queue_xiaozhi_session_action(XIAOZHI_SESSION_ACTION_TOGGLE, "AI button toggle");
            }
        }
        vTaskDelay(poll_ticks);
    }
}
#endif

static void xiaozhi_session_task(void *arg)
{
    (void)arg;
    xiaozhi_session_request_t request = {0};
    while (true) {
        if (xQueueReceive(xiaozhi_session_queue, &request, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        ESP_LOGI(
            TAG,
            "processing Xiaozhi action=%d reason=%s requested=%d",
            request.action,
            request.reason != NULL ? request.reason : "<none>",
            xiaozhi_client_session_requested());

        switch (request.action) {
        case XIAOZHI_SESSION_ACTION_START:
            (void)xiaozhi_start_session_after_music_stop(request.reason != NULL ? request.reason : "queued start");
            break;
        case XIAOZHI_SESSION_ACTION_STOP:
            (void)xiaozhi_stop_session_if_needed(request.reason != NULL ? request.reason : "queued stop");
            break;
        case XIAOZHI_SESSION_ACTION_TOGGLE:
            if (xiaozhi_client_session_requested()) {
                (void)xiaozhi_stop_session_if_needed(request.reason != NULL ? request.reason : "queued toggle off");
            } else if (music_player_is_active()) {
                (void)xiaozhi_start_session_after_music_stop(request.reason != NULL ? request.reason : "queued toggle on after music");
            } else {
                (void)xiaozhi_start_session_if_needed(request.reason != NULL ? request.reason : "queued toggle on");
            }
            break;
        case XIAOZHI_SESSION_ACTION_NONE:
        default:
            break;
        }

        ESP_LOGI(TAG, "finished Xiaozhi action=%d requested=%d", request.action, xiaozhi_client_session_requested());
    }
}

static void create_app_task(
    TaskFunction_t task_fn,
    const char *name,
    uint32_t stack_depth,
    UBaseType_t priority)
{
    TaskHandle_t handle = NULL;
    BaseType_t created = xTaskCreate(task_fn, name, stack_depth, NULL, priority, &handle);
    ESP_ERROR_CHECK(created == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);
#if CONFIG_ORNAMENT_WEB_CONSOLE_ENABLED
    system_diagnostics_register_task(name, handle, stack_depth, priority, false);
#endif
}

static void create_app_task_psram(
    TaskFunction_t task_fn,
    const char *name,
    uint32_t stack_depth,
    UBaseType_t priority)
{
    TaskHandle_t handle = NULL;
    BaseType_t created = xTaskCreateWithCaps(
        task_fn,
        name,
        stack_depth,
        NULL,
        priority,
        &handle,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    ESP_ERROR_CHECK(created == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);
#if CONFIG_ORNAMENT_WEB_CONSOLE_ENABLED
    system_diagnostics_register_task(name, handle, stack_depth, priority, true);
#endif
}

static void poll_task(void *arg)
{
    (void)arg;
    ornament_state_t fetched_state;
    ornament_state_init(&fetched_state);
    TickType_t last_done_tick = 0;
    TickType_t next_bridge_poll = 0;
    TickType_t next_bridge_recovery = 0;
    int last_done_seq = 0;
    int previous_active_task_count = 0;
    int consecutive_fetch_failures = 0;
    bool have_seen_state = false;
    bool have_last_done_tick = false;
    bool quota_refresh_pending = false;
    bool single_retry_pending = false;

    publish_bridge_fallback_state(NULL, false, ESP_ERR_INVALID_STATE, last_done_tick, have_last_done_tick, false);

    while (true) {
        TickType_t now = xTaskGetTickCount();

        if (!quota_refresh_pending && quota_reset_refresh_needed(&fetched_state)) {
            quota_refresh_pending = true;
            next_bridge_poll = now;
        }

        bool recovery_due = next_bridge_recovery != 0 && now >= next_bridge_recovery;
        if (recovery_due || now >= next_bridge_poll) {
            bool was_refresh_pending = quota_refresh_pending;
            bool retrying_bridge = single_retry_pending;
            bool recovering_bridge = recovery_due;

            if (retrying_bridge || recovering_bridge) {
                (void)auto_match_bridge(false, retrying_bridge ? "single retry" : "offline recovery");
            }

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
                single_retry_pending = false;
                next_bridge_recovery = 0;
                if (consecutive_fetch_failures > 0) {
                    ESP_LOGI(TAG, "bridge fetch recovered after %d failure(s)", consecutive_fetch_failures);
                }
                consecutive_fetch_failures = 0;
                debug.consecutive_fetch_failures = 0;
                debug.last_success_ms = ticks_to_ms(now);
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
                    task_audio_play_done();
                }
                if (fetched_state.done_seq > last_done_seq) {
                    last_done_seq = fetched_state.done_seq;
                }
                previous_active_task_count = fetched_state.active_task_count;
                have_seen_state = true;
                set_bridge_connection_state(&fetched_state, false, false);
                web_console_set_bridge_debug(&debug);
                publish_state(&fetched_state, err, last_done_tick, have_last_done_tick, true);
            } else {
                consecutive_fetch_failures++;
                debug.consecutive_fetch_failures = consecutive_fetch_failures;
                debug.last_failure_ms = ticks_to_ms(now);
                ESP_LOGW(
                    TAG,
                    "failed to fetch bridge state (%d/%d): %s",
                    consecutive_fetch_failures,
                    CONFIG_ORNAMENT_BRIDGE_OFFLINE_FAILURES,
                    esp_err_to_name(err));

                web_console_set_bridge_debug(&debug);
                if (was_refresh_pending || quota_refresh_pending) {
                    hide_expired_quota_reset_times(&fetched_state);
                }

                if (!retrying_bridge && !recovering_bridge) {
                    single_retry_pending = true;
                    next_bridge_poll = now + pdMS_TO_TICKS(BRIDGE_SINGLE_RETRY_DELAY_MS);
                    ESP_LOGW(TAG, "bridge fetch failed, scheduling a single retry in %d ms", BRIDGE_SINGLE_RETRY_DELAY_MS);
                    publish_bridge_fallback_state(
                        &fetched_state,
                        have_seen_state,
                        err,
                        last_done_tick,
                        have_last_done_tick,
                        false);
                } else {
                    single_retry_pending = false;
                    next_bridge_recovery = now + bridge_recovery_retry_ticks();
                    ESP_LOGW(
                        TAG,
                        "bridge retry exhausted, entering standby; next recovery in %lu ms",
                        (unsigned long)(bridge_recovery_retry_ticks() * portTICK_PERIOD_MS));
                    publish_bridge_fallback_state(
                        &fetched_state,
                        have_seen_state,
                        err,
                        last_done_tick,
                        have_last_done_tick,
                        true);
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
    xiaozhi_client_snapshot_t xiaozhi_snapshot;
    xiaozhi_client_snapshot_t previous_xiaozhi_snapshot = {0};
    voice_control_state_t voice_state;
    TickType_t idle_since_tick = 0;
    TickType_t last_done_tick = 0;
    TickType_t xiaozhi_page_focus_until_tick = 0;
    bool have_last_done_tick = false;
    bool have_previous_xiaozhi_snapshot = false;
    bool previous_standby_eligible = false;
    bool previous_xiaozhi_page_active = false;
    esp_err_t fetch_error = ESP_OK;

    while (true) {
        TickType_t now = xTaskGetTickCount();
        if (!snapshot_state(&state, &fetch_error, &last_done_tick, &have_last_done_tick)) {
            vTaskDelay(pdMS_TO_TICKS(CONFIG_ORNAMENT_UI_FRAME_MS));
            continue;
        }

        update_local_animation(&state, now, last_done_tick, have_last_done_tick);
        apply_local_state(&state, state.bridge_offline);
        standby_wallpaper_client_refresh_if_needed(&state);
        web_console_set_last_state(&state, fetch_error);
        xiaozhi_client_status_snapshot(&xiaozhi_snapshot);
        music_player_status_snapshot(&ui_music_snapshot);
        bool have_voice_state = voice_control_snapshot(&voice_state);
        if (xiaozhi_should_refresh_page_focus(
                &xiaozhi_snapshot,
                &previous_xiaozhi_snapshot,
                have_previous_xiaozhi_snapshot)) {
            xiaozhi_page_focus_until_tick = now + pdMS_TO_TICKS(XIAOZHI_PAGE_FOCUS_MS);
        }
        if (have_previous_xiaozhi_snapshot &&
            !xiaozhi_snapshot.session_requested &&
            previous_xiaozhi_snapshot.session_requested &&
            xiaozhi_snapshot.state == XIAOZHI_CLIENT_STATE_IDLE &&
            !xiaozhi_snapshot.activation_pending) {
            xiaozhi_page_focus_until_tick = 0;
        }
        bool xiaozhi_page_active = xiaozhi_session_page_active(
            &xiaozhi_snapshot,
            xiaozhi_page_focus_until_tick,
            now);
        bool manual_page_active = have_voice_state &&
                                  voice_state.manual_view &&
                                  voice_view_active(voice_state.view, voice_state.hold_until_tick, now);
        bool manual_xiaozhi_page = have_voice_state &&
                                   voice_manual_xiaozhi_page_active(&voice_state, now);
        bool automatic_state_page = automatic_state_page_active(&state);
        bool xiaozhi_page_visible = !automatic_state_page &&
                                    (manual_xiaozhi_page || (xiaozhi_page_active && !manual_page_active));

        bool standby_eligible = standby_timer_eligible(&state);
        if (standby_eligible) {
            if (!previous_standby_eligible || idle_since_tick == 0) {
                idle_since_tick = now;
            }
        } else {
            idle_since_tick = 0;
        }
        previous_standby_eligible = standby_eligible;

        if (xiaozhi_page_visible && !previous_xiaozhi_page_active) {
            size_t stack_hwm_bytes = uxTaskGetStackHighWaterMark(NULL) * sizeof(StackType_t);
            ESP_LOGI(TAG, "ui render entering xiaozhi page: stack_hwm=%u bytes", (unsigned int)stack_hwm_bytes);
            if (stack_hwm_bytes < UI_RENDER_LOW_STACK_WARN_BYTES) {
                ESP_LOGW(TAG, "ui render stack is low entering xiaozhi page: %u bytes", (unsigned int)stack_hwm_bytes);
            }
        }
        previous_xiaozhi_page_active = xiaozhi_page_visible;

        bool page_rendered = false;
        if (ui_music_snapshot.active || ui_music_snapshot.state == MUSIC_PLAYER_STATE_ERROR) {
            display_render_music(&state, &ui_music_snapshot);
            page_rendered = true;
        } else if (manual_page_active) {
            page_rendered = render_voice_override(&state, fetch_error, &xiaozhi_snapshot, &voice_state, now);
        } else if (automatic_state_page) {
            display_render_state(&state);
            page_rendered = true;
        } else if (xiaozhi_page_visible) {
            display_render_xiaozhi(&state, &xiaozhi_snapshot);
            page_rendered = true;
        }
        if (!page_rendered) {
            render_current_state(&state, idle_since_tick, now);
        }

        previous_xiaozhi_snapshot = xiaozhi_snapshot;
        have_previous_xiaozhi_snapshot = true;

        vTaskDelay(pdMS_TO_TICKS(CONFIG_ORNAMENT_UI_FRAME_MS));
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "starting Codex ornament");
#if CONFIG_ORNAMENT_WEB_CONSOLE_ENABLED
    esp_reset_reason_t reset_reason = esp_reset_reason();
    ESP_LOGW(
        TAG,
        "last reset reason: %s (%d)",
        system_diagnostics_reset_reason_name(reset_reason),
        (int)reset_reason);
#else
    ESP_LOGW(TAG, "last reset reason code: %d", (int)esp_reset_reason());
#endif
    log_heap_status("boot");
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    log_heap_status("after_nvs");

    display_init();
    display_render_boot();
    log_heap_status("after_display");
    esp_err_t wallpaper_err = standby_wallpaper_client_init();
    if (wallpaper_err != ESP_OK) {
        ESP_LOGW(TAG, "standby wallpaper client unavailable: %s", esp_err_to_name(wallpaper_err));
    }
    esp_err_t audio_err = task_audio_start();
    if (audio_err != ESP_OK) {
        ESP_LOGW(TAG, "task done audio unavailable: %s", esp_err_to_name(audio_err));
    }
    log_heap_status("after_audio");

    if (wifi_connect() != ESP_OK) {
        ESP_LOGW(TAG, "provisioning mode active: SSID=%s URL=http://192.168.4.1", config_portal_ssid());
        display_render_status("Setup AP: 192.168.4.1");
        while (true) {
            vTaskDelay(pdMS_TO_TICKS(60000));
        }
    }
    display_render_status("Wi-Fi connected");
    log_heap_status("after_wifi");
    voice_control_init();
    xiaozhi_session_queue = xQueueCreate(4, sizeof(xiaozhi_session_request_t));
    ESP_ERROR_CHECK(xiaozhi_session_queue == NULL ? ESP_ERR_NO_MEM : ESP_OK);

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
    log_heap_status("after_services");

    shared_state_mutex = xSemaphoreCreateMutex();
    ESP_ERROR_CHECK(shared_state_mutex == NULL ? ESP_ERR_NO_MEM : ESP_OK);
    memset(&shared_state, 0, sizeof(shared_state));
    ornament_state_init(&shared_state.state);
    shared_state.fetch_error = ESP_ERR_INVALID_STATE;
    shared_state.have_state = true;

    create_app_task(xiaozhi_session_task, "xiaozhi_ctl", 8192, 5);
#if CONFIG_ORNAMENT_PAGE_BUTTON_ENABLED
    create_app_task(page_button_task, "page_button", PAGE_BUTTON_TASK_STACK, 5);
#endif
#if CONFIG_ORNAMENT_AI_BUTTON_ENABLED
    create_app_task(ai_button_task, "ai_button", AI_BUTTON_TASK_STACK, 5);
#endif
    create_app_task(poll_task, "bridge_poll", BRIDGE_POLL_TASK_STACK, 5);
    create_app_task_psram(ui_render_task, "ui_render", UI_RENDER_TASK_STACK, 4);
    log_heap_status("after_tasks");

    /* Main task must never return in ESP-IDF — returning tears down
     * FreeRTOS resources (mutexes, queues, semaphores) that child tasks
     * still depend on, causing StoreProhibited panics.
     */
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(60000));
    }
}
