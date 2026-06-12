#pragma once

#include "esp_err.h"
#include "ornament_state.h"

#include <stdbool.h>
#include <stdint.h>

#define STANDBY_WALLPAPER_CLIENT_WIDTH 240
#define STANDBY_WALLPAPER_CLIENT_HEIGHT 240

typedef struct {
    bool ready;
    char wallpaper_id[ORNAMENT_WALLPAPER_ID_MAX];
    uint16_t width;
    uint16_t height;
} standby_wallpaper_snapshot_t;

esp_err_t standby_wallpaper_client_init(void);
void standby_wallpaper_client_refresh_if_needed(const ornament_state_t *state);
bool standby_wallpaper_client_copy_frame(uint16_t *pixels, size_t pixel_count);
void standby_wallpaper_client_snapshot(standby_wallpaper_snapshot_t *snapshot);
