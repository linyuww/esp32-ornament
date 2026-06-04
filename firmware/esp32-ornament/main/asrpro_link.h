#pragma once

#include "esp_err.h"

typedef enum {
    ASRPRO_VOICE_COMMAND_UNKNOWN = 0,
    ASRPRO_VOICE_COMMAND_STATUS,
    ASRPRO_VOICE_COMMAND_SHOW_QUOTA,
    ASRPRO_VOICE_COMMAND_SHOW_TASKS,
    ASRPRO_VOICE_COMMAND_SHOW_CLOCK,
    ASRPRO_VOICE_COMMAND_REFRESH_STATE,
    ASRPRO_VOICE_COMMAND_BRIDGE_MATCH,
    ASRPRO_VOICE_COMMAND_QUIET_ON,
    ASRPRO_VOICE_COMMAND_QUIET_OFF,
    ASRPRO_VOICE_COMMAND_XIAOZHI_START,
    ASRPRO_VOICE_COMMAND_XIAOZHI_STOP,
} asrpro_voice_command_t;

typedef void (*asrpro_voice_command_handler_t)(asrpro_voice_command_t command, void *context);

esp_err_t asrpro_link_init(asrpro_voice_command_handler_t handler, void *context);
void asrpro_link_notify_done(void);
const char *asrpro_voice_command_name(asrpro_voice_command_t command);
