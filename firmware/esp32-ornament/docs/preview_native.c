#include "display_core.h"
#include "ornament_state.h"

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

int main(int argc, char **argv)
{
    const char *output_dir = argc > 1 ? argv[1] : ".";
    ornament_state_t normal;
    ornament_state_t running_bright;
    ornament_state_t running_dim;
    ornament_state_t warn;
    ornament_state_t critical;
    ornament_state_t done_flash_on;
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
    if (render_one(output_dir, "unsynced", &unsynced, false) != 0) {
        return 1;
    }
    if (render_one(output_dir, "clock", &clock, true) != 0) {
        return 1;
    }
    return 0;
}
