#pragma once

#include "ornament_state.h"

void display_init(void);
void display_render_boot(void);
void display_render_status(const char *message);
void display_render_error(const char *message);
void display_render_state(const ornament_state_t *state);
void display_render_clock(const ornament_state_t *state);
