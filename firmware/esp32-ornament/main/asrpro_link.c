#include "asrpro_link.h"

#include "driver/uart.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <stdbool.h>
#include <string.h>

#ifndef CONFIG_ORNAMENT_ASRPRO_UART_ENABLED
#define CONFIG_ORNAMENT_ASRPRO_UART_ENABLED 1
#endif

#ifndef CONFIG_ORNAMENT_ASRPRO_DONE_NOTIFY
#define CONFIG_ORNAMENT_ASRPRO_DONE_NOTIFY 0
#endif

#ifndef CONFIG_ORNAMENT_ASRPRO_VOICE_COMMANDS
#define CONFIG_ORNAMENT_ASRPRO_VOICE_COMMANDS 1
#endif

#ifndef CONFIG_ORNAMENT_ASRPRO_UART_NUM
#define CONFIG_ORNAMENT_ASRPRO_UART_NUM 1
#endif

#ifndef CONFIG_ORNAMENT_ASRPRO_UART_TX_GPIO
#define CONFIG_ORNAMENT_ASRPRO_UART_TX_GPIO 17
#endif

#ifndef CONFIG_ORNAMENT_ASRPRO_UART_RX_GPIO
#define CONFIG_ORNAMENT_ASRPRO_UART_RX_GPIO 18
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

#define ASRPRO_RX_BUFFER_BYTES 512
#define ASRPRO_RX_READ_BYTES 64
#define ASRPRO_RX_TOKEN_MAX 40
#define ASRPRO_RX_IDLE_FLUSH_MS 250

static const char *TAG = "asrpro_link";
static bool link_ready;
static TaskHandle_t rx_task_handle;
static asrpro_voice_command_handler_t voice_handler;
static void *voice_handler_context;

typedef struct {
    char token[ASRPRO_RX_TOKEN_MAX];
    size_t length;
    bool overflow;
    TickType_t last_rx_tick;
} asrpro_rx_parser_t;

const char *asrpro_voice_command_name(asrpro_voice_command_t command)
{
    switch (command) {
    case ASRPRO_VOICE_COMMAND_STATUS:
        return "voice_status";
    case ASRPRO_VOICE_COMMAND_SHOW_QUOTA:
        return "show_quota";
    case ASRPRO_VOICE_COMMAND_SHOW_TASKS:
        return "show_tasks";
    case ASRPRO_VOICE_COMMAND_SHOW_CLOCK:
        return "show_clock";
    case ASRPRO_VOICE_COMMAND_REFRESH_STATE:
        return "refresh_state";
    case ASRPRO_VOICE_COMMAND_BRIDGE_MATCH:
        return "bridge_match";
    case ASRPRO_VOICE_COMMAND_QUIET_ON:
        return "quiet_on";
    case ASRPRO_VOICE_COMMAND_QUIET_OFF:
        return "quiet_off";
    case ASRPRO_VOICE_COMMAND_XIAOZHI_START:
        return "xiaozhi_start";
    case ASRPRO_VOICE_COMMAND_XIAOZHI_STOP:
        return "xiaozhi_stop";
    case ASRPRO_VOICE_COMMAND_UNKNOWN:
    default:
        return "unknown";
    }
}

static asrpro_voice_command_t command_from_token(const char *token)
{
    if (strcmp(token, "voice_status") == 0 || strcmp(token, "status") == 0) {
        return ASRPRO_VOICE_COMMAND_STATUS;
    }
    if (strcmp(token, "show_quota") == 0 || strcmp(token, "quota") == 0) {
        return ASRPRO_VOICE_COMMAND_SHOW_QUOTA;
    }
    if (strcmp(token, "show_tasks") == 0 || strcmp(token, "tasks") == 0) {
        return ASRPRO_VOICE_COMMAND_SHOW_TASKS;
    }
    if (strcmp(token, "show_clock") == 0 || strcmp(token, "clock") == 0) {
        return ASRPRO_VOICE_COMMAND_SHOW_CLOCK;
    }
    if (strcmp(token, "refresh_state") == 0 || strcmp(token, "refresh") == 0) {
        return ASRPRO_VOICE_COMMAND_REFRESH_STATE;
    }
    if (strcmp(token, "bridge_match") == 0 || strcmp(token, "match_bridge") == 0) {
        return ASRPRO_VOICE_COMMAND_BRIDGE_MATCH;
    }
    if (strcmp(token, "quiet_on") == 0) {
        return ASRPRO_VOICE_COMMAND_QUIET_ON;
    }
    if (strcmp(token, "quiet_off") == 0) {
        return ASRPRO_VOICE_COMMAND_QUIET_OFF;
    }
    if (strcmp(token, "xiaozhi_start") == 0 || strcmp(token, "ai_start") == 0) {
        return ASRPRO_VOICE_COMMAND_XIAOZHI_START;
    }
    if (strcmp(token, "xiaozhi_stop") == 0 || strcmp(token, "ai_stop") == 0) {
        return ASRPRO_VOICE_COMMAND_XIAOZHI_STOP;
    }
    return ASRPRO_VOICE_COMMAND_UNKNOWN;
}

static bool is_token_delimiter(char ch)
{
    return ch <= ' ' || ch == ',' || ch == ';';
}

static char normalize_token_char(char ch)
{
    if (ch >= 'A' && ch <= 'Z') {
        return (char)(ch - 'A' + 'a');
    }
    return ch;
}

static void reset_parser(asrpro_rx_parser_t *parser)
{
    parser->length = 0;
    parser->overflow = false;
    parser->token[0] = '\0';
}

static void dispatch_token(asrpro_rx_parser_t *parser)
{
    if (parser->length == 0) {
        reset_parser(parser);
        return;
    }
    parser->token[parser->length] = '\0';

    if (parser->overflow) {
        ESP_LOGW(TAG, "ASRPRO voice token too long, dropped");
        reset_parser(parser);
        return;
    }

    asrpro_voice_command_t command = command_from_token(parser->token);
    if (command == ASRPRO_VOICE_COMMAND_UNKNOWN) {
        ESP_LOGW(TAG, "unknown ASRPRO voice token: %s", parser->token);
        reset_parser(parser);
        return;
    }

    ESP_LOGI(TAG, "ASRPRO voice command: %s", asrpro_voice_command_name(command));
    if (voice_handler != NULL) {
        voice_handler(command, voice_handler_context);
    }
    reset_parser(parser);
}

static void append_rx_char(asrpro_rx_parser_t *parser, char ch)
{
    if (is_token_delimiter(ch)) {
        dispatch_token(parser);
        return;
    }

    ch = normalize_token_char(ch);
    if (!((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '_' || ch == '-')) {
        return;
    }

    if (parser->length + 1 >= sizeof(parser->token)) {
        parser->overflow = true;
        return;
    }
    parser->token[parser->length++] = ch;
}

static void rx_task(void *arg)
{
    (void)arg;
    const uart_port_t uart_num = (uart_port_t)CONFIG_ORNAMENT_ASRPRO_UART_NUM;
    asrpro_rx_parser_t parser = {0};
    uint8_t bytes[ASRPRO_RX_READ_BYTES];

    while (true) {
        int read = uart_read_bytes(uart_num, bytes, sizeof(bytes), pdMS_TO_TICKS(100));
        TickType_t now = xTaskGetTickCount();
        if (read > 0) {
            parser.last_rx_tick = now;
            for (int i = 0; i < read; i++) {
                append_rx_char(&parser, (char)bytes[i]);
            }
        } else if (parser.length > 0 &&
                   (now - parser.last_rx_tick) >= pdMS_TO_TICKS(ASRPRO_RX_IDLE_FLUSH_MS)) {
            dispatch_token(&parser);
        }
    }
}

esp_err_t asrpro_link_init(asrpro_voice_command_handler_t handler, void *context)
{
    voice_handler = handler;
    voice_handler_context = context;
#if CONFIG_ORNAMENT_ASRPRO_UART_ENABLED
    if (link_ready) {
        return ESP_OK;
    }
    if (CONFIG_ORNAMENT_ASRPRO_DONE_NOTIFY && CONFIG_ORNAMENT_ASRPRO_DONE_TRIGGER[0] == '\0') {
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
    ESP_RETURN_ON_ERROR(uart_driver_install(uart_num, ASRPRO_RX_BUFFER_BYTES, 0, 0, NULL, 0), TAG, "uart_driver_install failed");

    link_ready = true;
    ESP_LOGI(
        TAG,
        "ASRPRO UART ready uart=%d baud=%d tx=%d rx=%d done_notify=%d voice_commands=%d trigger=%s",
        CONFIG_ORNAMENT_ASRPRO_UART_NUM,
        CONFIG_ORNAMENT_ASRPRO_UART_BAUD,
        CONFIG_ORNAMENT_ASRPRO_UART_TX_GPIO,
        CONFIG_ORNAMENT_ASRPRO_UART_RX_GPIO,
        CONFIG_ORNAMENT_ASRPRO_DONE_NOTIFY,
        CONFIG_ORNAMENT_ASRPRO_VOICE_COMMANDS,
        CONFIG_ORNAMENT_ASRPRO_DONE_TRIGGER);

#if CONFIG_ORNAMENT_ASRPRO_VOICE_COMMANDS
    if (CONFIG_ORNAMENT_ASRPRO_UART_RX_GPIO >= 0) {
        BaseType_t created = xTaskCreate(rx_task, "asrpro_rx", 3072, NULL, 5, &rx_task_handle);
        if (created != pdPASS) {
            return ESP_ERR_NO_MEM;
        }
    } else {
        ESP_LOGW(TAG, "ASRPRO voice commands disabled: RX GPIO is not configured");
    }
#endif
#else
    ESP_LOGI(TAG, "ASRPRO UART link disabled");
#endif
    return ESP_OK;
}

void asrpro_link_notify_done(void)
{
#if CONFIG_ORNAMENT_ASRPRO_UART_ENABLED && CONFIG_ORNAMENT_ASRPRO_DONE_NOTIFY
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
