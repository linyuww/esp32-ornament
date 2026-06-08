#pragma once

#include "esp_err.h"
#include "ornament_state.h"
#include "settings.h"

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    MUSIC_PLAYER_STATE_IDLE = 0,
    MUSIC_PLAYER_STATE_RESOLVING,
    MUSIC_PLAYER_STATE_PLAYING,
    MUSIC_PLAYER_STATE_STOPPING,
    MUSIC_PLAYER_STATE_ERROR,
} music_player_state_t;

typedef struct {
    bool active;
    bool stop_requested;
    music_player_state_t state;
    uint32_t index;
    char song_name[ORNAMENT_TEXT_MAX];
    char artist_name[ORNAMENT_TEXT_MAX];
    char title[ORNAMENT_TEXT_MAX];
    char album[ORNAMENT_TEXT_MAX];
    char picture[ORNAMENT_BRIDGE_URL_MAX];
    char last_error[ORNAMENT_TEXT_MAX];
} music_player_snapshot_t;

esp_err_t music_player_init(void);
esp_err_t music_player_play_song(const char *song_name, const char *artist_name, uint32_t index);
void music_player_request_stop(void);
esp_err_t music_player_stop(void);
bool music_player_is_active(void);
void music_player_status_snapshot(music_player_snapshot_t *snapshot);
const char *music_player_state_name(music_player_state_t state);
