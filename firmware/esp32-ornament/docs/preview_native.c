#include "display_core.h"
#include "music_player.h"
#include "ornament_state.h"
#include "xiaozhi_client.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PREVIEW_WIDTH 240
#define PREVIEW_HEIGHT 240

static void set_text(char *dst, size_t dst_size, const char *src)
{
    if (dst_size == 0) {
        return;
    }
    if (src == NULL) {
        dst[0] = '\0';
        return;
    }
    snprintf(dst, dst_size, "%s", src);
}

static uint8_t expand_5_to_8(uint16_t value)
{
    return (uint8_t)((value << 3) | (value >> 2));
}

static uint8_t expand_6_to_8(uint16_t value)
{
    return (uint8_t)((value << 2) | (value >> 4));
}

const char *music_player_state_name(music_player_state_t state)
{
    switch (state) {
    case MUSIC_PLAYER_STATE_RESOLVING:
        return "resolving";
    case MUSIC_PLAYER_STATE_PLAYING:
        return "playing";
    case MUSIC_PLAYER_STATE_STOPPING:
        return "stopping";
    case MUSIC_PLAYER_STATE_ERROR:
        return "error";
    case MUSIC_PLAYER_STATE_IDLE:
    default:
        return "idle";
    }
}

const char *xiaozhi_client_state_name(xiaozhi_client_state_t state)
{
    switch (state) {
    case XIAOZHI_CLIENT_STATE_CONNECTING:
        return "connecting";
    case XIAOZHI_CLIENT_STATE_LISTENING:
        return "listening";
    case XIAOZHI_CLIENT_STATE_SPEAKING:
        return "speaking";
    case XIAOZHI_CLIENT_STATE_ERROR:
        return "error";
    case XIAOZHI_CLIENT_STATE_CONFIG_MISSING:
        return "config";
    case XIAOZHI_CLIENT_STATE_IDLE:
        return "idle";
    case XIAOZHI_CLIENT_STATE_DISABLED:
    default:
        return "disabled";
    }
}

static int write_ppm(const char *path, const uint16_t *pixels)
{
    FILE *file = fopen(path, "wb");
    if (file == NULL) {
        fprintf(stderr, "open failed: %s\n", path);
        return 1;
    }

    fprintf(file, "P6\n%d %d\n255\n", PREVIEW_WIDTH, PREVIEW_HEIGHT);
    for (int i = 0; i < PREVIEW_WIDTH * PREVIEW_HEIGHT; i++) {
        uint16_t pixel = pixels[i];
        uint8_t rgb[3] = {
            expand_5_to_8((pixel >> 11) & 0x1F),
            expand_6_to_8((pixel >> 5) & 0x3F),
            expand_5_to_8(pixel & 0x1F),
        };
        if (fwrite(rgb, sizeof(rgb), 1, file) != 1) {
            fclose(file);
            fprintf(stderr, "write failed: %s\n", path);
            return 1;
        }
    }

    fclose(file);
    return 0;
}

static void make_state(
    ornament_state_t *state,
    ornament_status_t status,
    int primary,
    int weekly,
    const char *primary_reset,
    const char *weekly_reset,
    const char *local_time,
    const char *local_date,
    bool wifi_connected,
    int wifi_rssi,
    bool time_synced)
{
    ornament_state_init(state);
    state->status = status;
    state->primary_remaining_percent = primary;
    state->secondary_remaining_percent = weekly;
    set_text(state->primary_resets_at, sizeof(state->primary_resets_at), primary_reset);
    set_text(state->secondary_resets_at, sizeof(state->secondary_resets_at), weekly_reset);
    set_text(state->local_time, sizeof(state->local_time), local_time);
    set_text(state->local_date, sizeof(state->local_date), local_date);
    set_text(state->wifi_ssid, sizeof(state->wifi_ssid), wifi_connected ? "CodexLab" : "");
    state->wifi_connected = wifi_connected;
    state->wifi_rssi = wifi_rssi;
    state->time_synced = time_synced;
    state->has_quota = true;
    state->has_task = status != ORNAMENT_STATUS_IDLE;
    state->has_weather = true;
    set_text(state->weather_status, sizeof(state->weather_status), "ok");
    set_text(state->weather_label, sizeof(state->weather_label), "HAIDIAN");
    set_text(state->weather_summary, sizeof(state->weather_summary), "PARTLY CLOUDY");
    set_text(state->weather_icon, sizeof(state->weather_icon), "cloud");
    state->weather_temperature_c = 34;
    state->weather_wind_kmh = 8;
    state->weather_code = 1;
}

static int render_one(const char *output_dir, const char *name, const ornament_state_t *state, bool clock_page)
{
    uint16_t *pixels = calloc(PREVIEW_WIDTH * PREVIEW_HEIGHT, sizeof(uint16_t));
    if (pixels == NULL) {
        fprintf(stderr, "preview framebuffer allocation failed\n");
        return 1;
    }

    display_core_canvas_t canvas;
    display_core_canvas_init(&canvas, PREVIEW_WIDTH, PREVIEW_HEIGHT, pixels);
    if (clock_page) {
        display_core_render_clock(&canvas, state);
    } else {
        display_core_render_hud(&canvas, state);
    }

    char path[1024];
    snprintf(path, sizeof(path), "%s/ui-preview-%s.ppm", output_dir, name);
    int rc = write_ppm(path, pixels);
    free(pixels);
    return rc;
}

static int render_music(const char *output_dir, const ornament_state_t *state)
{
    uint16_t *pixels = calloc(PREVIEW_WIDTH * PREVIEW_HEIGHT, sizeof(uint16_t));
    uint16_t *cover = calloc(MUSIC_PLAYER_COVER_PIXELS, sizeof(uint16_t));
    if (pixels == NULL || cover == NULL) {
        free(pixels);
        free(cover);
        fprintf(stderr, "music preview allocation failed\n");
        return 1;
    }

    for (int y = 0; y < MUSIC_PLAYER_COVER_SIZE; y++) {
        for (int x = 0; x < MUSIC_PLAYER_COVER_SIZE; x++) {
            uint8_t r = (uint8_t)(32 + x * 2);
            uint8_t g = (uint8_t)(28 + y * 2);
            uint8_t b = (uint8_t)(180 - (x + y) / 2);
            cover[y * MUSIC_PLAYER_COVER_SIZE + x] = display_core_rgb565(r, g, b);
        }
    }

    music_player_snapshot_t music = {0};
    music.active = true;
    music.state = MUSIC_PLAYER_STATE_PLAYING;
    music.playback_ms = 54000;
    music.has_cover = true;
    music.cover_pixels = cover;
    set_text(music.title, sizeof(music.title), "Bad Guy");
    set_text(music.artist_name, sizeof(music.artist_name), "Billie Eilish");
    set_text(
        music.lyrics,
        sizeof(music.lyrics),
        "[00:44.00]White shirt now red\n[00:54.00]Sleeping you're on your tippy toes\n[01:04.00]Creeping around like no one knows\n[01:14.00]Think you're so criminal");

    display_core_canvas_t canvas;
    display_core_canvas_init(&canvas, PREVIEW_WIDTH, PREVIEW_HEIGHT, pixels);
    display_core_render_music(&canvas, state, &music);

    char path[1024];
    snprintf(path, sizeof(path), "%s/ui-preview-music.ppm", output_dir);
    int rc = write_ppm(path, pixels);
    free(cover);
    free(pixels);
    return rc;
}

static int render_xiaozhi(
    const char *output_dir,
    const ornament_state_t *state,
    const char *name,
    xiaozhi_client_state_t xiaozhi_state,
    bool connected,
    bool configured,
    const char *last_stt,
    const char *last_tts,
    const char *last_error)
{
    uint16_t *pixels = calloc(PREVIEW_WIDTH * PREVIEW_HEIGHT, sizeof(uint16_t));
    if (pixels == NULL) {
        fprintf(stderr, "xiaozhi preview allocation failed\n");
        return 1;
    }

    xiaozhi_client_snapshot_t xiaozhi = {0};
    xiaozhi.enabled = true;
    xiaozhi.configured = configured;
    xiaozhi.connected = connected;
    xiaozhi.state = xiaozhi_state;
    set_text(xiaozhi.last_stt, sizeof(xiaozhi.last_stt), last_stt);
    set_text(xiaozhi.last_tts, sizeof(xiaozhi.last_tts), last_tts);
    set_text(xiaozhi.last_error, sizeof(xiaozhi.last_error), last_error);

    display_core_canvas_t canvas;
    display_core_canvas_init(&canvas, PREVIEW_WIDTH, PREVIEW_HEIGHT, pixels);
    display_core_render_xiaozhi(&canvas, state, &xiaozhi);

    char path[1024];
    snprintf(path, sizeof(path), "%s/ui-preview-%s.ppm", output_dir, name);
    int rc = write_ppm(path, pixels);
    free(pixels);
    return rc;
}

int main(int argc, char **argv)
{
    const char *output_dir = argc > 1 ? argv[1] : ".";
    ornament_state_t normal;
    ornament_state_t running_bright;
    ornament_state_t running_dim;
    ornament_state_t warn;
    ornament_state_t critical;
    ornament_state_t done_flash_on;
    ornament_state_t done_flash_running;
    ornament_state_t unsynced;
    ornament_state_t clock;

    make_state(
        &normal,
        ORNAMENT_STATUS_RUNNING,
        64,
        73,
        "2026-05-22T21:59:00+08:00",
        "2026-05-18T16:14:00+08:00",
        "19:42",
        "05-23",
        true,
        -62,
        true);
    running_bright = normal;
    running_bright.active_dot_phase = 0;
    running_dim = normal;
    running_dim.active_dot_phase = 2;
    make_state(
        &warn,
        ORNAMENT_STATUS_RUNNING,
        24,
        58,
        "2026-05-22T21:59:00+08:00",
        "2026-05-18T16:14:00+08:00",
        "19:42",
        "05-23",
        true,
        -67,
        true);
    make_state(
        &critical,
        ORNAMENT_STATUS_RUNNING,
        8,
        41,
        "2026-05-22T21:59:00+08:00",
        "2026-05-18T16:14:00+08:00",
        "19:42",
        "05-23",
        true,
        -72,
        true);
    make_state(
        &done_flash_on,
        ORNAMENT_STATUS_DONE,
        82,
        61,
        "2026-05-22T21:59:00+08:00",
        "2026-05-18T16:14:00+08:00",
        "19:42",
        "05-23",
        true,
        -59,
        true);
    done_flash_on.done_flash_active = true;
    done_flash_on.done_flash_on = true;
    done_flash_running = done_flash_on;
    done_flash_running.status = ORNAMENT_STATUS_DONE;
    done_flash_running.active_task_count = 2;
    done_flash_running.codex_active_task_count = 1;
    done_flash_running.claude_active_task_count = 1;
    done_flash_running.done_seq = 3;
    make_state(
        &unsynced,
        ORNAMENT_STATUS_RUNNING,
        64,
        73,
        "2026-05-22T21:59:00+08:00",
        "2026-05-18T16:14:00+08:00",
        "--:--",
        "-- --",
        false,
        0,
        false);
    make_state(
        &clock,
        ORNAMENT_STATUS_IDLE,
        64,
        73,
        "2026-05-22T21:59:00+08:00",
        "2026-05-18T16:14:00+08:00",
        "19:42",
        "05-23",
        true,
        -62,
        true);

    if (render_one(output_dir, "normal", &normal, false) != 0) {
        return 1;
    }
    if (render_one(output_dir, "running-bright", &running_bright, false) != 0) {
        return 1;
    }
    if (render_one(output_dir, "running-dim", &running_dim, false) != 0) {
        return 1;
    }
    if (render_one(output_dir, "warn", &warn, false) != 0) {
        return 1;
    }
    if (render_one(output_dir, "critical", &critical, false) != 0) {
        return 1;
    }
    if (render_one(output_dir, "done-flash", &done_flash_on, false) != 0) {
        return 1;
    }
    if (render_one(output_dir, "done-flash-running", &done_flash_running, false) != 0) {
        return 1;
    }
    if (render_one(output_dir, "unsynced", &unsynced, false) != 0) {
        return 1;
    }
    if (render_one(output_dir, "clock", &clock, true) != 0) {
        return 1;
    }
    if (render_music(output_dir, &normal) != 0) {
        return 1;
    }
    if (render_xiaozhi(
            output_dir,
            &normal,
            "xiaozhi-idle",
            XIAOZHI_CLIENT_STATE_IDLE,
            false,
            true,
            "",
            "",
            "") != 0) {
        return 1;
    }
    if (render_xiaozhi(
            output_dir,
            &normal,
            "xiaozhi-listening",
            XIAOZHI_CLIENT_STATE_LISTENING,
            true,
            true,
            "Summarize current task?",
            "",
            "") != 0) {
        return 1;
    }
    if (render_xiaozhi(
            output_dir,
            &normal,
            "xiaozhi-speaking",
            XIAOZHI_CLIENT_STATE_SPEAKING,
            true,
            true,
            "Summarize current task?",
            "Updated UI preview is ready.",
            "") != 0) {
        return 1;
    }
    if (render_xiaozhi(
            output_dir,
            &normal,
            "xiaozhi-connecting",
            XIAOZHI_CLIENT_STATE_CONNECTING,
            false,
            true,
            "",
            "",
            "") != 0) {
        return 1;
    }
    if (render_xiaozhi(
            output_dir,
            &normal,
            "xiaozhi-error",
            XIAOZHI_CLIENT_STATE_ERROR,
            false,
            false,
            "",
            "",
            "Check config") != 0) {
        return 1;
    }
    if (render_xiaozhi(
            output_dir,
            &normal,
            "xiaozhi-done",
            XIAOZHI_CLIENT_STATE_IDLE,
            true,
            true,
            "Summarize current task?",
            "Context reply finished.",
            "") != 0) {
        return 1;
    }
    return 0;
}
