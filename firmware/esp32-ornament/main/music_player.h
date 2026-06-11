#pragma once

#include "esp_err.h"
#include "ornament_state.h"
#include "settings.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define MUSIC_PLAYER_COVER_SIZE 96
#define MUSIC_PLAYER_COVER_PIXELS (MUSIC_PLAYER_COVER_SIZE * MUSIC_PLAYER_COVER_SIZE)
#define MUSIC_PLAYER_LYRICS_MAX 1024

#ifndef CONFIG_ORNAMENT_MUSIC_COVER_ENABLED
#define CONFIG_ORNAMENT_MUSIC_COVER_ENABLED 0
#endif

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
    uint32_t playback_ms;
    char song_name[ORNAMENT_TEXT_MAX];
    char artist_name[ORNAMENT_TEXT_MAX];
    char title[ORNAMENT_TEXT_MAX];
    char album[ORNAMENT_TEXT_MAX];
    char picture[ORNAMENT_BRIDGE_URL_MAX];
    char cover_url[ORNAMENT_BRIDGE_URL_MAX];
    char lyrics[MUSIC_PLAYER_LYRICS_MAX];
    char last_error[ORNAMENT_TEXT_MAX];
    bool has_cover;
#if CONFIG_ORNAMENT_MUSIC_COVER_ENABLED
    const uint16_t *cover_pixels;
#endif
} music_player_snapshot_t;

esp_err_t music_player_init(void);
esp_err_t music_player_play_song(const char *song_name, const char *artist_name, uint32_t index);
esp_err_t music_player_play_song_with_settings(
    const char *song_name,
    const char *artist_name,
    uint32_t index,
    const ornament_settings_t *settings);
void music_player_request_stop(void);
esp_err_t music_player_stop(void);
bool music_player_is_active(void);
void music_player_status_snapshot(music_player_snapshot_t *snapshot);
const char *music_player_state_name(music_player_state_t state);
