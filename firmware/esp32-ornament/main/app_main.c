#include "asrpro_link.h"
#include "bridge_client.h"
#include "config_portal.h"
#include "display.h"
#include "ornament_state.h"
#include "system_status.h"
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
    return state->status == ORNAMENT_STATUS_RUNNING || state->done_flash_active;
}

static bool should_show_standby_clock(const ornament_state_t *state, TickType_t idle_since_tick, TickType_t now)
{
    return CONFIG_ORNAMENT_STANDBY_CLOCK_MS > 0 &&
           state->status == ORNAMENT_STATUS_IDLE &&
           idle_since_tick != 0 &&
           (now - idle_since_tick) >= pdMS_TO_TICKS(CONFIG_ORNAMENT_STANDBY_CLOCK_MS);
}

static void update_local_animation(ornament_state_t *state, TickType_t now, TickType_t last_done_tick)
{
    state->active_dot_phase = (uint8_t)((now / pdMS_TO_TICKS(CONFIG_ORNAMENT_UI_FRAME_MS)) & 0x03);
    state->done_flash_active = false;
    state->done_flash_on = false;

    if (state->status == ORNAMENT_STATUS_DONE && CONFIG_ORNAMENT_DONE_FLASH_MS > 0) {
        TickType_t elapsed = now - last_done_tick;
        TickType_t flash_window = pdMS_TO_TICKS(CONFIG_ORNAMENT_DONE_FLASH_MS);
        TickType_t flash_step = pdMS_TO_TICKS(400);
        state->done_flash_active = elapsed < flash_window;
        state->done_flash_on = flash_step > 0 && ((elapsed / flash_step) % 2) == 0;
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

static bool is_new_done_event(const ornament_state_t *state, bool have_seen_state, int last_done_seq)
{
    if (!have_seen_state || state->done_seq <= 0) {
        return false;
    }
    return state->done_seq > last_done_seq;
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
    bool last_render_was_error = false;

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
                    asrpro_link_notify_done();
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

                update_local_animation(&state, now, last_done_tick);
                system_status_update(&state);
                web_console_set_last_state(&state, err);
                render_now = true;
            } else {
                ESP_LOGW(TAG, "failed to fetch bridge state: %s", esp_err_to_name(err));
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
            update_local_animation(&state, now, last_done_tick);
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

    if (wifi_connect() != ESP_OK) {
        ESP_LOGW(TAG, "provisioning mode active: SSID=%s URL=http://192.168.4.1", config_portal_ssid());
        display_render_status("Setup AP: 192.168.4.1");
        while (true) {
            vTaskDelay(pdMS_TO_TICKS(60000));
        }
    }
    display_render_status("Wi-Fi connected");
    esp_err_t asrpro_err = asrpro_link_init();
    if (asrpro_err != ESP_OK) {
        ESP_LOGW(TAG, "ASRPRO done reminder unavailable: %s", esp_err_to_name(asrpro_err));
    }
    system_status_start_time_sync();
    ESP_ERROR_CHECK(web_console_start());

    xTaskCreate(poll_task, "bridge_poll", 8192, NULL, 5, NULL);
}
