#include "asrpro_link.h"
#include "bridge_client.h"
#include "config_portal.h"
#include "display.h"
#include "ornament_state.h"
#include "system_status.h"
#include "task_audio.h"
#include "web_console.h"
#include "wifi.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"

#include <string.h>

static const char *TAG = "ornament";

static bool ui_needs_animation(const ornament_state_t *state)
{
    return ornament_state_panel_status(state) == ORNAMENT_STATUS_RUNNING || state->done_flash_active;
}

static bool should_show_standby_clock(const ornament_state_t *state, TickType_t idle_since_tick, TickType_t now)
{
    return CONFIG_ORNAMENT_STANDBY_CLOCK_MS > 0 &&
           ornament_state_panel_status(state) == ORNAMENT_STATUS_IDLE &&
           idle_since_tick != 0 &&
           (now - idle_since_tick) >= pdMS_TO_TICKS(CONFIG_ORNAMENT_STANDBY_CLOCK_MS);
}

static uint32_t ticks_to_ms(TickType_t ticks)
{
    return (uint32_t)(ticks * portTICK_PERIOD_MS);
}

static void update_local_animation(
    ornament_state_t *state,
    TickType_t now,
    TickType_t last_done_tick,
    bool have_last_done_tick)
{
    ornament_state_update_display_timing(state, ticks_to_ms(now), ticks_to_ms(last_done_tick), have_last_done_tick);
}

static void render_current_state(const ornament_state_t *state, TickType_t idle_since_tick, TickType_t now)
{
    if (should_show_standby_clock(state, idle_since_tick, now)) {
        display_render_clock(state);
    } else {
        display_render_state(state);
    }
}

static bool is_new_done_event(const ornament_state_t *state, bool have_seen_state, int last_done_seq)
{
    if (!have_seen_state || state->done_seq <= 0) {
        return false;
    }
    return state->done_seq > last_done_seq;
}

static bool auto_match_bridge(bool verify_current, const char *reason)
{
    bridge_auto_match_result_t result;
    esp_err_t err = bridge_client_auto_match(verify_current, &result);
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

static void poll_task(void *arg)
{
    ornament_state_t state;
    ornament_state_init(&state);
    ornament_status_t previous_status = ORNAMENT_STATUS_IDLE;
    TickType_t last_done_tick = 0;
    TickType_t idle_since_tick = 0;
    TickType_t next_bridge_poll = 0;
    int last_done_seq = 0;
    bool have_state = false;
    bool have_seen_state = false;
    bool have_last_done_tick = false;
    bool last_render_was_error = false;
    TickType_t next_auto_match = 0;

    while (true) {
        TickType_t now = xTaskGetTickCount();
        bool render_now = false;

        if (now >= next_bridge_poll) {
            next_bridge_poll = now + pdMS_TO_TICKS(CONFIG_ORNAMENT_POLL_INTERVAL_MS);
            esp_err_t err = bridge_client_fetch_state(&state);
            if (err == ESP_OK) {
                have_state = true;
                last_render_was_error = false;

                if (state.done_seq < last_done_seq) {
                    last_done_seq = state.done_seq;
                }
                if (is_new_done_event(&state, have_seen_state, last_done_seq)) {
                    last_done_tick = now;
                    have_last_done_tick = true;
                    asrpro_link_notify_done();
                    task_audio_play_done();
                }
                if (state.done_seq > last_done_seq) {
                    last_done_seq = state.done_seq;
                }
                if (state.status == ORNAMENT_STATUS_IDLE) {
                    if (previous_status != ORNAMENT_STATUS_IDLE || idle_since_tick == 0) {
                        idle_since_tick = now;
                    }
                } else {
                    idle_since_tick = 0;
                }
                previous_status = state.status;
                have_seen_state = true;

                update_local_animation(&state, now, last_done_tick, have_last_done_tick);
                system_status_update(&state);
                web_console_set_last_state(&state, err);
                render_now = true;
            } else {
                ESP_LOGW(TAG, "failed to fetch bridge state: %s", esp_err_to_name(err));
                if (now >= next_auto_match) {
                    next_auto_match = now + pdMS_TO_TICKS(CONFIG_ORNAMENT_BRIDGE_AUTO_MATCH_RETRY_MS);
                    if (auto_match_bridge(false, "poll failure")) {
                        next_bridge_poll = now;
                        last_render_was_error = false;
                        vTaskDelay(pdMS_TO_TICKS(200));
                        continue;
                    }
                }

                idle_since_tick = 0;
                previous_status = ORNAMENT_STATUS_ERROR;
                ornament_state_t error_state;
                ornament_state_init(&error_state);
                error_state.status = ORNAMENT_STATUS_ERROR;
                system_status_update(&error_state);
                web_console_set_last_state(&error_state, err);
                if (!last_render_was_error) {
                    display_render_error("Bridge offline");
                    last_render_was_error = true;
                }
                have_state = false;
            }
        } else if (have_state) {
            bool was_animating = ui_needs_animation(&state);
            update_local_animation(&state, now, last_done_tick, have_last_done_tick);
            system_status_update(&state);
            render_now = ui_needs_animation(&state) || was_animating;
        }

        if (render_now) {
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
    task_audio_start();

    if (wifi_connect() != ESP_OK) {
        ESP_LOGW(TAG, "provisioning mode active: SSID=%s URL=http://192.168.4.1", config_portal_ssid());
        display_render_status("Setup AP: 192.168.4.1");
        while (true) {
            vTaskDelay(pdMS_TO_TICKS(60000));
        }
    }
    display_render_status("Wi-Fi connected");
    if (CONFIG_ORNAMENT_BRIDGE_AUTO_MATCH_ON_BOOT) {
        display_render_status("Matching bridge...");
        if (auto_match_bridge(true, "boot")) {
            display_render_status("Bridge matched");
        } else {
            display_render_status("Bridge search failed");
        }
        vTaskDelay(pdMS_TO_TICKS(600));
    }

    esp_err_t asrpro_err = asrpro_link_init();
    if (asrpro_err != ESP_OK) {
        ESP_LOGW(TAG, "ASRPRO done reminder unavailable: %s", esp_err_to_name(asrpro_err));
    }
    system_status_start_time_sync();
    ESP_ERROR_CHECK(web_console_start());

    xTaskCreate(poll_task, "bridge_poll", 8192, NULL, 5, NULL);
}
