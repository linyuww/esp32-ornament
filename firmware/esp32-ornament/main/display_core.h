#pragma once

#include "ornament_state.h"
#include "xiaozhi_client.h"

#include <stdint.h>

typedef struct {
    uint16_t width;
    uint16_t height;
    uint16_t center_x;
    uint16_t center_y;
    uint16_t radius;
    uint16_t *pixels;
} display_core_canvas_t;

uint16_t display_core_rgb565(uint8_t r, uint8_t g, uint8_t b);
void display_core_canvas_init(display_core_canvas_t *canvas, uint16_t width, uint16_t height, uint16_t *pixels);
void display_core_render_hud(display_core_canvas_t *canvas, const ornament_state_t *state);
void display_core_render_clock(display_core_canvas_t *canvas, const ornament_state_t *state);
void display_core_render_voice_status(
    display_core_canvas_t *canvas,
    const ornament_state_t *state,
    const char *bridge_status,
    const char *voice_status);
void display_core_render_xiaozhi(
    display_core_canvas_t *canvas,
    const ornament_state_t *state,
    const xiaozhi_client_snapshot_t *snapshot);
void display_core_render_tasks(display_core_canvas_t *canvas, const ornament_state_t *state);
void display_core_render_boot(display_core_canvas_t *canvas);
void display_core_render_status_message(display_core_canvas_t *canvas, const char *message);
void display_core_render_error_message(display_core_canvas_t *canvas, const char *message);
