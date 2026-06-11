#pragma once

#include "esp_err.h"
#include "ornament_state.h"
#include "sdkconfig.h"

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    esp_err_t last_fetch_error;
    esp_err_t last_auto_match_error;
    int consecutive_fetch_failures;
    int64_t last_success_ms;
    int64_t last_failure_ms;
    bool last_auto_match_ok;
    char last_auto_match_reason[48];
} web_console_bridge_debug_t;

#ifndef CONFIG_ORNAMENT_WEB_CONSOLE_ENABLED
#define CONFIG_ORNAMENT_WEB_CONSOLE_ENABLED 0
#endif

#if CONFIG_ORNAMENT_WEB_CONSOLE_ENABLED

esp_err_t web_console_start(void);
void web_console_set_last_state(const ornament_state_t *state, esp_err_t fetch_error);
void web_console_set_bridge_debug(const web_console_bridge_debug_t *debug);

#else

static inline esp_err_t web_console_start(void)
{
    return ESP_OK;
}

static inline void web_console_set_last_state(const ornament_state_t *state, esp_err_t fetch_error)
{
    (void)state;
    (void)fetch_error;
}

static inline void web_console_set_bridge_debug(const web_console_bridge_debug_t *debug)
{
    (void)debug;
}

#endif
