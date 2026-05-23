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

static const char *TAG = "ornament";

static void poll_task(void *arg)
{
    ornament_state_t state;
    ornament_state_init(&state);
    ornament_status_t previous_status = ORNAMENT_STATUS_IDLE;
    TickType_t last_done_tick = 0;
    TickType_t idle_since_tick = 0;

    TickType_t last_wake = xTaskGetTickCount();

    while (true) {
        esp_err_t err = bridge_client_fetch_state(&state);
        if (err == ESP_OK) {
            TickType_t now = xTaskGetTickCount();
            if (state.status == ORNAMENT_STATUS_DONE && previous_status != ORNAMENT_STATUS_DONE) {
                last_done_tick = now;
            }
            if (state.status == ORNAMENT_STATUS_IDLE) {
                if (previous_status != ORNAMENT_STATUS_IDLE || idle_since_tick == 0) {
                    idle_since_tick = now;
                }
            } else {
                idle_since_tick = 0;
            }
            previous_status = state.status;

            if (state.status == ORNAMENT_STATUS_DONE && CONFIG_ORNAMENT_DONE_FLASH_MS > 0) {
                TickType_t elapsed = now - last_done_tick;
                TickType_t flash_window = pdMS_TO_TICKS(CONFIG_ORNAMENT_DONE_FLASH_MS);
                TickType_t flash_step = pdMS_TO_TICKS(400);
                state.done_flash_active = elapsed < flash_window;
                state.done_flash_on = flash_step > 0 && ((elapsed / flash_step) % 2) == 0;
            }

            system_status_update(&state);
            web_console_set_last_state(&state, err);
            bool standby_clock = false;
            if (CONFIG_ORNAMENT_STANDBY_CLOCK_MS > 0 && state.status == ORNAMENT_STATUS_IDLE && idle_since_tick != 0) {
                standby_clock = (now - idle_since_tick) >= pdMS_TO_TICKS(CONFIG_ORNAMENT_STANDBY_CLOCK_MS);
            }
            if (standby_clock) {
                display_render_clock(&state);
            } else {
                display_render_state(&state);
            }
        } else {
            ESP_LOGW(TAG, "failed to fetch bridge state: %s", esp_err_to_name(err));
            idle_since_tick = 0;
            ornament_state_t error_state;
            ornament_state_init(&error_state);
            error_state.status = ORNAMENT_STATUS_ERROR;
            system_status_update(&error_state);
            web_console_set_last_state(&error_state, err);
            display_render_error("Bridge offline");
        }

        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(CONFIG_ORNAMENT_POLL_INTERVAL_MS));
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
    system_status_start_time_sync();
    ESP_ERROR_CHECK(web_console_start());

    xTaskCreate(poll_task, "bridge_poll", 8192, NULL, 5, NULL);
}
