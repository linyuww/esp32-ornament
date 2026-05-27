#include "asrpro_link.h"

#include "driver/uart.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"

#include <stdbool.h>
#include <string.h>

#ifndef CONFIG_ORNAMENT_ASRPRO_DONE_NOTIFY
#define CONFIG_ORNAMENT_ASRPRO_DONE_NOTIFY 0
#endif

#ifndef CONFIG_ORNAMENT_ASRPRO_UART_NUM
#define CONFIG_ORNAMENT_ASRPRO_UART_NUM 1
#endif

#ifndef CONFIG_ORNAMENT_ASRPRO_UART_TX_GPIO
#define CONFIG_ORNAMENT_ASRPRO_UART_TX_GPIO 17
#endif

#ifndef CONFIG_ORNAMENT_ASRPRO_UART_RX_GPIO
#define CONFIG_ORNAMENT_ASRPRO_UART_RX_GPIO -1
#endif

#ifndef CONFIG_ORNAMENT_ASRPRO_UART_BAUD
#define CONFIG_ORNAMENT_ASRPRO_UART_BAUD 9600
#endif

#ifndef CONFIG_ORNAMENT_ASRPRO_DONE_TRIGGER
#define CONFIG_ORNAMENT_ASRPRO_DONE_TRIGGER "codex_done"
#endif

#ifndef CONFIG_ORNAMENT_ASRPRO_DONE_SUFFIX
#define CONFIG_ORNAMENT_ASRPRO_DONE_SUFFIX ""
#endif

#ifndef CONFIG_ORNAMENT_ASRPRO_TX_WAIT_MS
#define CONFIG_ORNAMENT_ASRPRO_TX_WAIT_MS 100
#endif

static const char *TAG = "asrpro_link";
static bool link_ready;

esp_err_t asrpro_link_init(void)
{
#if CONFIG_ORNAMENT_ASRPRO_DONE_NOTIFY
    if (link_ready) {
        return ESP_OK;
    }
    if (CONFIG_ORNAMENT_ASRPRO_DONE_TRIGGER[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    const uart_port_t uart_num = (uart_port_t)CONFIG_ORNAMENT_ASRPRO_UART_NUM;
    const uart_config_t uart_config = {
        .baud_rate = CONFIG_ORNAMENT_ASRPRO_UART_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    ESP_RETURN_ON_ERROR(uart_param_config(uart_num, &uart_config), TAG, "uart_param_config failed");
    ESP_RETURN_ON_ERROR(
        uart_set_pin(
            uart_num,
            CONFIG_ORNAMENT_ASRPRO_UART_TX_GPIO,
            CONFIG_ORNAMENT_ASRPRO_UART_RX_GPIO,
            UART_PIN_NO_CHANGE,
            UART_PIN_NO_CHANGE),
        TAG,
        "uart_set_pin failed");
    ESP_RETURN_ON_ERROR(uart_driver_install(uart_num, 256, 0, 0, NULL, 0), TAG, "uart_driver_install failed");

    link_ready = true;
    ESP_LOGI(
        TAG,
        "ASRPRO done reminder enabled uart=%d baud=%d tx=%d rx=%d trigger=%s",
        CONFIG_ORNAMENT_ASRPRO_UART_NUM,
        CONFIG_ORNAMENT_ASRPRO_UART_BAUD,
        CONFIG_ORNAMENT_ASRPRO_UART_TX_GPIO,
        CONFIG_ORNAMENT_ASRPRO_UART_RX_GPIO,
        CONFIG_ORNAMENT_ASRPRO_DONE_TRIGGER);
#else
    ESP_LOGI(TAG, "ASRPRO done reminder disabled");
#endif
    return ESP_OK;
}

void asrpro_link_notify_done(void)
{
#if CONFIG_ORNAMENT_ASRPRO_DONE_NOTIFY
    if (!link_ready) {
        ESP_LOGW(TAG, "ASRPRO done reminder skipped: UART not initialized");
        return;
    }

    const uart_port_t uart_num = (uart_port_t)CONFIG_ORNAMENT_ASRPRO_UART_NUM;
    const char *trigger = CONFIG_ORNAMENT_ASRPRO_DONE_TRIGGER;
    const char *suffix = CONFIG_ORNAMENT_ASRPRO_DONE_SUFFIX;
    const size_t trigger_len = strlen(trigger);
    const int trigger_written = uart_write_bytes(uart_num, trigger, trigger_len);
    if (trigger_written != (int)trigger_len) {
        ESP_LOGW(TAG, "ASRPRO done trigger short write: %d/%u", trigger_written, (unsigned int)trigger_len);
        return;
    }

    const size_t suffix_len = strlen(suffix);
    if (suffix_len > 0) {
        const int suffix_written = uart_write_bytes(uart_num, suffix, suffix_len);
        if (suffix_written != (int)suffix_len) {
            ESP_LOGW(TAG, "ASRPRO done suffix short write: %d/%u", suffix_written, (unsigned int)suffix_len);
            return;
        }
    }

    esp_err_t err = uart_wait_tx_done(uart_num, pdMS_TO_TICKS(CONFIG_ORNAMENT_ASRPRO_TX_WAIT_MS));
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "ASRPRO done trigger sent");
    } else {
        ESP_LOGW(TAG, "ASRPRO done trigger wait failed: %s", esp_err_to_name(err));
    }
#endif
}
