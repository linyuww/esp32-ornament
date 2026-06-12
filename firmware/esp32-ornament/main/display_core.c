#include "display_core.h"
#include "standby_wallpaper_client.h"

#ifndef CONFIG_ORNAMENT_STANDBY_WALLPAPER_ENABLED
#define CONFIG_ORNAMENT_STANDBY_WALLPAPER_ENABLED 0
#endif

#if CONFIG_ORNAMENT_STANDBY_WALLPAPER_ENABLED
#include "standby_wallpaper.h"
#endif

#ifndef CONFIG_ORNAMENT_RICH_XIAOZHI_DISPLAY_ENABLED
#define CONFIG_ORNAMENT_RICH_XIAOZHI_DISPLAY_ENABLED 0
#endif

#if CONFIG_ORNAMENT_RICH_XIAOZHI_DISPLAY_ENABLED
#include "lvgl.h"
#include "misc/lv_text_private.h"
#endif

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if CONFIG_ORNAMENT_RICH_XIAOZHI_DISPLAY_ENABLED
LV_FONT_DECLARE(font_puhui_16_4);
LV_FONT_DECLARE(font_puhui_basic_16_4);
#endif

#ifndef CONFIG_ORNAMENT_QUOTA_CRITICAL_PERCENT
#define CONFIG_ORNAMENT_QUOTA_CRITICAL_PERCENT 10
#endif

#ifndef CONFIG_ORNAMENT_QUOTA_WARN_PERCENT
#define CONFIG_ORNAMENT_QUOTA_WARN_PERCENT 25
#endif

#define HUD_BLACK display_core_rgb565(0, 0, 0)
#define HUD_WHITE display_core_rgb565(255, 255, 255)
#define HUD_MUTED display_core_rgb565(118, 146, 170)
#define HUD_BLUE display_core_rgb565(16, 152, 255)
#define HUD_CYAN display_core_rgb565(23, 191, 255)
#define HUD_CODEX display_core_rgb565(121, 232, 255)
#define HUD_GREEN display_core_rgb565(57, 255, 20)
#define HUD_AMBER display_core_rgb565(255, 184, 46)
#define HUD_RED display_core_rgb565(255, 64, 64)
#define HUD_DIM_AMBER display_core_rgb565(86, 46, 0)
#define HUD_DIM_RED display_core_rgb565(76, 0, 0)
#define HUD_DIM_BLUE display_core_rgb565(0, 28, 66)
#define HUD_DIM_CYAN display_core_rgb565(0, 64, 78)
#define HUD_DIM_GREEN display_core_rgb565(14, 70, 16)
#define HUD_DEEP_BLUE display_core_rgb565(0, 12, 32)
#define HUD_PANEL_BLUE display_core_rgb565(0, 66, 82)
#define HUD_PANEL_GREEN display_core_rgb565(0, 74, 42)
#define HUD_TEAL display_core_rgb565(44, 232, 196)
#define HUD_SOFT_BG display_core_rgb565(7, 11, 16)
#define HUD_SLATE display_core_rgb565(148, 163, 184)
#define HUD_HUD_BG display_core_rgb565(0, 2, 4)
#define HUD_HUD_CYAN display_core_rgb565(0, 238, 255)
#define HUD_HUD_DIM display_core_rgb565(0, 58, 66)
#define MUSIC_COVER_BG display_core_rgb565(10, 15, 20)
#define MUSIC_COVER_DIM display_core_rgb565(25, 36, 48)

static display_core_canvas_t *active_canvas;
static music_player_snapshot_t music_fallback_snapshot;

static void draw_text_center_fit(int y, const char *text, int preferred_scale, uint16_t color);

uint16_t display_core_rgb565(uint8_t r, uint8_t g, uint8_t b)
{
    return ((uint16_t)(r & 0xF8) << 8) | ((uint16_t)(g & 0xFC) << 3) | (uint16_t)(b >> 3);
}

void display_core_canvas_init(display_core_canvas_t *canvas, uint16_t width, uint16_t height, uint16_t *pixels)
{
    if (canvas == NULL) {
        return;
    }
    canvas->width = width;
    canvas->height = height;
    canvas->center_x = width / 2;
    canvas->center_y = height / 2;
    canvas->radius = (width < height ? width : height) / 2 - 4;
    canvas->pixels = pixels;
}

static bool canvas_ready(const display_core_canvas_t *canvas)
{
    return canvas != NULL && canvas->pixels != NULL && canvas->width > 0 && canvas->height > 0;
}

static bool begin_render(display_core_canvas_t *canvas)
{
    if (!canvas_ready(canvas)) {
        return false;
    }
    active_canvas = canvas;
    return true;
}

static void draw_pixel(int x, int y, uint16_t color)
{
    if (x < 0 || y < 0 || x >= active_canvas->width || y >= active_canvas->height) {
        return;
    }
    active_canvas->pixels[y * active_canvas->width + x] = color;
}

static void clear_canvas(uint16_t color)
{
    for (int y = 0; y < active_canvas->height; y++) {
        for (int x = 0; x < active_canvas->width; x++) {
            active_canvas->pixels[y * active_canvas->width + x] = color;
        }
    }
}

static void fill_rect(int x, int y, int w, int h, uint16_t color)
{
    for (int yy = y; yy < y + h; yy++) {
        for (int xx = x; xx < x + w; xx++) {
            draw_pixel(xx, yy, color);
        }
    }
}

static uint16_t blend_rgb565(uint16_t source, uint16_t overlay, uint8_t overlay_alpha)
{
    uint8_t inverse_alpha = (uint8_t)(255U - overlay_alpha);
    uint8_t source_r = (uint8_t)(((source >> 11) & 0x1F) << 3);
    uint8_t source_g = (uint8_t)(((source >> 5) & 0x3F) << 2);
    uint8_t source_b = (uint8_t)((source & 0x1F) << 3);
    uint8_t overlay_r = (uint8_t)(((overlay >> 11) & 0x1F) << 3);
    uint8_t overlay_g = (uint8_t)(((overlay >> 5) & 0x3F) << 2);
    uint8_t overlay_b = (uint8_t)((overlay & 0x1F) << 3);
    uint8_t r = (uint8_t)(((uint16_t)source_r * inverse_alpha + (uint16_t)overlay_r * overlay_alpha) / 255U);
    uint8_t g = (uint8_t)(((uint16_t)source_g * inverse_alpha + (uint16_t)overlay_g * overlay_alpha) / 255U);
    uint8_t b = (uint8_t)(((uint16_t)source_b * inverse_alpha + (uint16_t)overlay_b * overlay_alpha) / 255U);
    return display_core_rgb565(r, g, b);
}

static void blend_rect(int x, int y, int w, int h, uint16_t color, uint8_t alpha)
{
    if (active_canvas == NULL || active_canvas->pixels == NULL || alpha == 0) {
        return;
    }

    int x0 = x < 0 ? 0 : x;
    int y0 = y < 0 ? 0 : y;
    int x1 = x + w;
    int y1 = y + h;
    if (x1 > active_canvas->width) {
        x1 = active_canvas->width;
    }
    if (y1 > active_canvas->height) {
        y1 = active_canvas->height;
    }
    for (int yy = y0; yy < y1; yy++) {
        for (int xx = x0; xx < x1; xx++) {
            uint16_t *pixel = &active_canvas->pixels[yy * active_canvas->width + xx];
            *pixel = blend_rgb565(*pixel, color, alpha);
        }
    }
}

static int sx(int value)
{
    return value * (int)active_canvas->width / 360;
}

static int sy(int value)
{
    return value * (int)active_canvas->height / 360;
}

static int ss(int value)
{
    int x_scaled = sx(value);
    int y_scaled = sy(value);
    return x_scaled < y_scaled ? x_scaled : y_scaled;
}

static int ts(int value)
{
    int scaled = ss(value) / 2;
    return scaled > 0 ? scaled : 1;
}

static void draw_hline(int x0, int x1, int y, int thickness, uint16_t color)
{
    fill_rect(x0, y, x1 - x0 + 1, thickness, color);
}

static void draw_vline(int x, int y0, int y1, int thickness, uint16_t color)
{
    fill_rect(x, y0, thickness, y1 - y0 + 1, color);
}

static void draw_rect_outline(int x, int y, int w, int h, int thickness, uint16_t color)
{
    draw_hline(x, x + w - 1, y, thickness, color);
    draw_hline(x, x + w - 1, y + h - thickness, thickness, color);
    draw_vline(x, y, y + h - 1, thickness, color);
    draw_vline(x + w - thickness, y, y + h - 1, thickness, color);
}

static void draw_line(int x0, int y0, int x1, int y1, int thickness, uint16_t color)
{
    int dx = abs(x1 - x0);
    int sx_dir = x0 < x1 ? 1 : -1;
    int dy = -abs(y1 - y0);
    int sy_dir = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    int half = thickness > 1 ? thickness / 2 : 0;

    while (true) {
        fill_rect(x0 - half, y0 - half, thickness, thickness, color);
        if (x0 == x1 && y0 == y1) {
            break;
        }
        int e2 = err * 2;
        if (e2 >= dy) {
            err += dy;
            x0 += sx_dir;
        }
        if (e2 <= dx) {
            err += dx;
            y0 += sy_dir;
        }
    }
}

static void fill_circle(int cx, int cy, int radius, uint16_t color)
{
    int radius_sq = radius * radius;
    for (int y = cy - radius; y <= cy + radius; y++) {
        for (int x = cx - radius; x <= cx + radius; x++) {
            int dx = x - cx;
            int dy = y - cy;
            if (dx * dx + dy * dy <= radius_sq) {
                draw_pixel(x, y, color);
            }
        }
    }
}

typedef enum {
    HUD_ALERT_NORMAL,
    HUD_ALERT_WARN,
    HUD_ALERT_CRITICAL,
    HUD_ALERT_DONE_FLASH,
    HUD_ALERT_ERROR,
} hud_alert_level_t;

static int min_known_percent(const ornament_state_t *state)
{
    int value = 101;
    if (state->primary_remaining_percent >= 0) {
        value = state->primary_remaining_percent;
    }
    if (state->secondary_remaining_percent >= 0 && state->secondary_remaining_percent < value) {
        value = state->secondary_remaining_percent;
    }
    return value == 101 ? -1 : value;
}

static hud_alert_level_t alert_level_for(const ornament_state_t *state)
{
    if (state->status == ORNAMENT_STATUS_ERROR) {
        return HUD_ALERT_ERROR;
    }
    if (state->done_flash_active) {
        return HUD_ALERT_DONE_FLASH;
    }

    int percent = min_known_percent(state);
    if (percent >= 0 && percent < CONFIG_ORNAMENT_QUOTA_CRITICAL_PERCENT) {
        return HUD_ALERT_CRITICAL;
    }
    if (percent >= 0 && percent < CONFIG_ORNAMENT_QUOTA_WARN_PERCENT) {
        return HUD_ALERT_WARN;
    }
    return HUD_ALERT_NORMAL;
}

static uint16_t alert_color_for(const ornament_state_t *state)
{
    switch (alert_level_for(state)) {
    case HUD_ALERT_ERROR:
        return HUD_RED;
    case HUD_ALERT_DONE_FLASH:
        return state->done_flash_on ? HUD_GREEN : HUD_DIM_GREEN;
    case HUD_ALERT_CRITICAL:
        return HUD_RED;
    case HUD_ALERT_WARN:
        return HUD_AMBER;
    case HUD_ALERT_NORMAL:
    default:
        return HUD_TEAL;
    }
}

static uint16_t ui_accent_color_for(const ornament_state_t *state)
{
    if (state->status == ORNAMENT_STATUS_ERROR) {
        return HUD_RED;
    }

    int percent = min_known_percent(state);
    if (percent >= 0 && percent < CONFIG_ORNAMENT_QUOTA_CRITICAL_PERCENT) {
        return HUD_RED;
    }
    if (percent >= 0 && percent < CONFIG_ORNAMENT_QUOTA_WARN_PERCENT) {
        return HUD_AMBER;
    }
    return HUD_TEAL;
}

static uint16_t alert_dim_color_for(const ornament_state_t *state)
{
    switch (alert_level_for(state)) {
    case HUD_ALERT_ERROR:
    case HUD_ALERT_CRITICAL:
        return HUD_DIM_RED;
    case HUD_ALERT_WARN:
        return HUD_DIM_AMBER;
    case HUD_ALERT_DONE_FLASH:
        return state->done_flash_on ? HUD_DIM_GREEN : HUD_DEEP_BLUE;
    case HUD_ALERT_NORMAL:
    default:
        return HUD_DEEP_BLUE;
    }
}

static void draw_ring_ticks(const ornament_state_t *state)
{
    ornament_status_t panel_status = ornament_state_panel_status(state);
    bool marquee = panel_status == ORNAMENT_STATUS_RUNNING && !state->done_flash_active;
    uint16_t ring_color = alert_color_for(state);
    uint16_t dim_color = alert_dim_color_for(state);
    uint16_t trail_color = marquee ? dim_color : ring_color;

    draw_rect_outline(sx(9), sy(9), sx(342), sy(342), ss(2), dim_color);
    const int tick = ss(5);
    const int step = sx(16);
    const int x_start = sx(20);
    const int x_end = x_start + ((sx(335) - x_start) / step) * step;
    const int y_start = sy(24);
    const int y_end = y_start + ((sy(330) - y_start) / step) * step;
    int index = 0;
    int phase = (state->active_dot_phase & 0x03) * 2;
    for (int x = x_start; x <= x_end; x += step) {
        uint16_t top_color = marquee && ((index + phase) % 8) >= 3 ? trail_color : ring_color;
        fill_rect(x, sy(9), tick, tick, top_color);
        index++;
    }
    for (int y = y_start; y <= y_end; y += step) {
        uint16_t right_color = marquee && ((index + phase) % 8) >= 3 ? trail_color : ring_color;
        fill_rect(sx(346), y, tick, tick, right_color);
        index++;
    }
    for (int x = x_end; x >= x_start; x -= step) {
        uint16_t bottom_color = marquee && ((index + phase) % 8) >= 3 ? trail_color : ring_color;
        fill_rect(x, sy(346), tick, tick, bottom_color);
        index++;
    }
    for (int y = y_end; y >= y_start; y -= step) {
        uint16_t left_color = marquee && ((index + phase) % 8) >= 3 ? trail_color : ring_color;
        fill_rect(sx(9), y, tick, tick, left_color);
        index++;
    }
}

static const uint8_t *glyph_for(char ch)
{
    static const uint8_t blank[7] = {0, 0, 0, 0, 0, 0, 0};
    static const uint8_t glyphs[][7] = {
        ['0'] = {0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E},
        ['1'] = {0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E},
        ['2'] = {0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F},
        ['3'] = {0x1E, 0x01, 0x01, 0x0E, 0x01, 0x01, 0x1E},
        ['4'] = {0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02},
        ['5'] = {0x1F, 0x10, 0x10, 0x1E, 0x01, 0x01, 0x1E},
        ['6'] = {0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E},
        ['7'] = {0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08},
        ['8'] = {0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E},
        ['9'] = {0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C},
        ['A'] = {0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11},
        ['B'] = {0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E},
        ['C'] = {0x0E, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0E},
        ['D'] = {0x1E, 0x12, 0x11, 0x11, 0x11, 0x12, 0x1E},
        ['E'] = {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F},
        ['F'] = {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10},
        ['G'] = {0x0E, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0F},
        ['H'] = {0x11, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11},
        ['I'] = {0x0E, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0E},
        ['J'] = {0x07, 0x02, 0x02, 0x02, 0x12, 0x12, 0x0C},
        ['K'] = {0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11},
        ['L'] = {0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F},
        ['M'] = {0x11, 0x1B, 0x15, 0x15, 0x11, 0x11, 0x11},
        ['N'] = {0x11, 0x19, 0x19, 0x15, 0x13, 0x13, 0x11},
        ['O'] = {0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E},
        ['P'] = {0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10},
        ['Q'] = {0x0E, 0x11, 0x11, 0x11, 0x15, 0x12, 0x0D},
        ['R'] = {0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11},
        ['S'] = {0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E},
        ['T'] = {0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04},
        ['U'] = {0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E},
        ['V'] = {0x11, 0x11, 0x11, 0x11, 0x11, 0x0A, 0x04},
        ['W'] = {0x11, 0x11, 0x11, 0x15, 0x15, 0x15, 0x0A},
        ['X'] = {0x11, 0x11, 0x0A, 0x04, 0x0A, 0x11, 0x11},
        ['Y'] = {0x11, 0x11, 0x0A, 0x04, 0x04, 0x04, 0x04},
        ['Z'] = {0x1F, 0x01, 0x02, 0x04, 0x08, 0x10, 0x1F},
        ['%'] = {0x19, 0x19, 0x02, 0x04, 0x08, 0x13, 0x13},
        [':'] = {0x00, 0x0C, 0x0C, 0x00, 0x0C, 0x0C, 0x00},
        ['.'] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x0C, 0x0C},
        ['-'] = {0x00, 0x00, 0x00, 0x1F, 0x00, 0x00, 0x00},
        ['/'] = {0x01, 0x01, 0x02, 0x04, 0x08, 0x10, 0x10},
        ['?'] = {0x0E, 0x11, 0x01, 0x02, 0x04, 0x00, 0x04},
        ['_'] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x1F},
        ['+'] = {0x00, 0x04, 0x04, 0x1F, 0x04, 0x04, 0x00},
        ['='] = {0x00, 0x00, 0x1F, 0x00, 0x1F, 0x00, 0x00},
        ['('] = {0x02, 0x04, 0x08, 0x08, 0x08, 0x04, 0x02},
        [')'] = {0x08, 0x04, 0x02, 0x02, 0x02, 0x04, 0x08},
        [','] = {0x00, 0x00, 0x00, 0x00, 0x0C, 0x04, 0x08},
        ['!'] = {0x04, 0x04, 0x04, 0x04, 0x04, 0x00, 0x04},
        [' '] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
    };
    unsigned char index = (unsigned char)ch;
    if (index >= sizeof(glyphs) / sizeof(glyphs[0])) {
        return blank;
    }
    return glyphs[index];
}

static void draw_char(int x, int y, char ch, int scale, uint16_t color)
{
    if (ch >= 'a' && ch <= 'z') {
        ch = (char)(ch - 'a' + 'A');
    }
    const uint8_t *glyph = glyph_for(ch);
    for (int row = 0; row < 7; row++) {
        for (int col = 0; col < 5; col++) {
            if ((glyph[row] >> (4 - col)) & 1) {
                fill_rect(x + col * scale, y + row * scale, scale, scale, color);
            }
        }
    }
}

static void draw_text(int x, int y, const char *text, int scale, uint16_t color)
{
    int cursor = x;
    for (const char *ch = text; *ch != '\0'; ch++) {
        draw_char(cursor, y, *ch, scale, color);
        cursor += 6 * scale;
    }
}

static int text_width(const char *text, int scale)
{
    return (int)strlen(text) * 6 * scale;
}

static void draw_char_xy(int x, int y, char ch, int x_scale, int y_scale, uint16_t color)
{
    if (ch >= 'a' && ch <= 'z') {
        ch = (char)(ch - 'a' + 'A');
    }
    const uint8_t *glyph = glyph_for(ch);
    for (int row = 0; row < 7; row++) {
        for (int col = 0; col < 5; col++) {
            if ((glyph[row] >> (4 - col)) & 1) {
                fill_rect(x + col * x_scale, y + row * y_scale, x_scale, y_scale, color);
            }
        }
    }
}

static void draw_text_xy(int x, int y, const char *text, int x_scale, int y_scale, uint16_t color)
{
    int cursor = x;
    for (const char *ch = text; *ch != '\0'; ch++) {
        draw_char_xy(cursor, y, *ch, x_scale, y_scale, color);
        cursor += 6 * x_scale;
    }
}

static int text_width_xy(const char *text, int x_scale)
{
    return (int)strlen(text) * 6 * x_scale;
}

static void draw_circle_outline(int cx, int cy, int radius, int thickness, uint16_t color)
{
    if (radius <= 0 || thickness <= 0) {
        return;
    }

    int outer_sq = radius * radius;
    int inner = radius - thickness;
    int inner_sq = inner > 0 ? inner * inner : 0;
    for (int y = cy - radius; y <= cy + radius; y++) {
        for (int x = cx - radius; x <= cx + radius; x++) {
            int dx = x - cx;
            int dy = y - cy;
            int dist_sq = dx * dx + dy * dy;
            if (dist_sq <= outer_sq && dist_sq >= inner_sq) {
                draw_pixel(x, y, color);
            }
        }
    }
}

#if CONFIG_ORNAMENT_RICH_XIAOZHI_DISPLAY_ENABLED

static uint32_t utf8_next(const char *text, uint32_t *offset)
{
    if (text == NULL || offset == NULL || text[*offset] == '\0') {
        return 0;
    }
    return lv_text_encoded_next(text, offset);
}

static uint8_t lv_alpha_from_bitmap(const uint8_t *bitmap, uint32_t index, lv_font_glyph_format_t format)
{
    switch (format) {
    case LV_FONT_GLYPH_FORMAT_A1:
        return (bitmap[index / 8] & (uint8_t)(0x80 >> (index % 8))) ? 255 : 0;
    case LV_FONT_GLYPH_FORMAT_A2:
        return (uint8_t)(((bitmap[index / 4] >> (6 - (index % 4) * 2)) & 0x03) * 85);
    case LV_FONT_GLYPH_FORMAT_A4:
        return (uint8_t)(((index & 1) == 0 ? (bitmap[index / 2] >> 4) : bitmap[index / 2] & 0x0F) * 17);
    case LV_FONT_GLYPH_FORMAT_A8:
        return bitmap[index];
    default:
        return 0;
    }
}

static uint8_t glyph_alpha_at(
    const uint8_t *bitmap,
    uint32_t row,
    uint32_t col,
    uint32_t stride,
    bool bitmap_is_a8,
    lv_font_glyph_format_t format)
{
    uint32_t bitmap_index = row * stride + col;
    if (bitmap_is_a8) {
        return bitmap[bitmap_index];
    }
    return lv_alpha_from_bitmap(bitmap, bitmap_index, format);
}

static const uint8_t *glyph_bitmap_bytes(
    const void *bitmap_ref,
    const lv_draw_buf_t *draw_buf,
    uint32_t *stride,
    bool *bitmap_is_a8)
{
    if (bitmap_ref == NULL) {
        return NULL;
    }
    if (draw_buf != NULL &&
        (bitmap_ref == (const void *)draw_buf || bitmap_ref == (const void *)draw_buf->data)) {
        if (stride != NULL) {
            *stride = draw_buf->header.stride;
        }
        if (bitmap_is_a8 != NULL) {
            *bitmap_is_a8 = true;
        }
        return draw_buf->data;
    }
    if (bitmap_is_a8 != NULL) {
        *bitmap_is_a8 = false;
    }
    return (const uint8_t *)bitmap_ref;
}

static void blend_pixel(int x, int y, uint16_t color, uint8_t alpha)
{
    if (x < 0 || y < 0 || x >= active_canvas->width || y >= active_canvas->height || alpha == 0) {
        return;
    }
    if (alpha == 255) {
        draw_pixel(x, y, color);
        return;
    }
    uint16_t *pixel = &active_canvas->pixels[y * active_canvas->width + x];
    *pixel = blend_rgb565(*pixel, color, alpha);
}

static int draw_utf8_text(int x, int y, const lv_font_t *font, const char *text, uint16_t color, int max_width)
{
    if (font == NULL || text == NULL || text[0] == '\0') {
        return 0;
    }
    int cursor = x;
    uint32_t offset = 0;
    while (text[offset] != '\0') {
        uint32_t letter_offset = offset;
        uint32_t letter = utf8_next(text, &offset);
        uint32_t next_offset = offset;
        uint32_t letter_next = utf8_next(text, &next_offset);
        if (letter == 0 || offset == letter_offset) {
            offset++;
            continue;
        }

        uint16_t adv = lv_font_get_glyph_width(font, letter, letter_next);
        if (max_width > 0 && cursor + (int)adv > x + max_width) {
            break;
        }

        lv_font_glyph_dsc_t glyph = {0};
        if (!lv_font_get_glyph_dsc(font, &glyph, letter, letter_next)) {
            cursor += adv > 0 ? adv : font->line_height / 2;
            continue;
        }
        uint8_t glyph_bitmap_storage[384];
        uint8_t *glyph_bitmap_buffer = glyph_bitmap_storage;
        lv_draw_buf_t glyph_draw_buf;
        lv_draw_buf_t *glyph_draw_buf_ptr = NULL;
        uint32_t glyph_stride = lv_draw_buf_width_to_stride(glyph.box_w, LV_COLOR_FORMAT_A8);
        uint32_t glyph_bitmap_size = LV_DRAW_BUF_SIZE(glyph.box_w, glyph.box_h, LV_COLOR_FORMAT_A8);
        bool bitmap_is_a8 = false;
        if (glyph.box_w <= 0 || glyph.box_h <= 0) {
            cursor += adv;
            continue;
        }
        if (glyph_bitmap_size > sizeof(glyph_bitmap_storage)) {
            glyph_bitmap_buffer = lv_malloc(glyph_bitmap_size);
            if (glyph_bitmap_buffer == NULL) {
                lv_font_glyph_release_draw_data(&glyph);
                cursor += adv;
                continue;
            }
        }
        if (lv_draw_buf_init(
                &glyph_draw_buf,
                glyph.box_w,
                glyph.box_h,
                LV_COLOR_FORMAT_A8,
                glyph_stride,
                glyph_bitmap_buffer,
                glyph_bitmap_size) == LV_RESULT_OK) {
            glyph_draw_buf_ptr = &glyph_draw_buf;
        }
        const void *bitmap_ref = lv_font_get_glyph_bitmap(&glyph, glyph_draw_buf_ptr);
        const uint8_t *bitmap = glyph_bitmap_bytes(bitmap_ref, glyph_draw_buf_ptr, &glyph_stride, &bitmap_is_a8);
        if (bitmap != NULL) {
            int glyph_x = cursor + glyph.ofs_x;
            int glyph_y = y + font->line_height - font->base_line - glyph.box_h - glyph.ofs_y;
            for (int row = 0; row < glyph.box_h; row++) {
                for (int col = 0; col < glyph.box_w; col++) {
                    uint8_t alpha = glyph_alpha_at(bitmap, (uint32_t)row, (uint32_t)col, glyph_stride, bitmap_is_a8, glyph.format);
                    blend_pixel(glyph_x + col, glyph_y + row, color, alpha);
                }
            }
        }
        lv_font_glyph_release_draw_data(&glyph);
        if (glyph_bitmap_buffer != glyph_bitmap_storage) {
            lv_free(glyph_bitmap_buffer);
        }
        cursor += adv;
    }
    return cursor - x;
}

static void draw_utf8_text_line(
    int x,
    int y,
    const lv_font_t *font,
    const char *text,
    uint32_t start,
    uint32_t end,
    uint16_t color,
    int max_width)
{
    char line[XIAOZHI_STATUS_TEXT_MAX + 1];
    uint32_t length = end > start ? end - start : 0;
    if (length >= sizeof(line)) {
        length = sizeof(line) - 1;
    }
    memcpy(line, text + start, length);
    line[length] = '\0';
    draw_utf8_text(x, y, font, line, color, max_width);
}

static void draw_utf8_text_wrapped(
    int x,
    int y,
    const lv_font_t *font,
    const char *text,
    uint16_t color,
    int max_width,
    int max_lines)
{
    if (font == NULL || max_lines <= 0) {
        return;
    }
    if (text == NULL || text[0] == '\0') {
        text = "--";
    }

    uint32_t line_start = 0;
    for (int line = 0; line < max_lines && text[line_start] != '\0'; line++) {
        uint32_t offset = line_start;
        uint32_t line_end = line_start;
        int line_width = 0;
        while (text[offset] != '\0') {
            uint32_t letter_offset = offset;
            uint32_t letter = utf8_next(text, &offset);
            uint32_t next_offset = offset;
            uint32_t letter_next = utf8_next(text, &next_offset);
            if (letter == 0 || offset == letter_offset) {
                offset++;
                line_end = offset;
                continue;
            }

            uint16_t adv = lv_font_get_glyph_width(font, letter, letter_next);
            if (line_end > line_start && max_width > 0 && line_width + (int)adv > max_width) {
                offset = letter_offset;
                break;
            }
            line_width += adv;
            line_end = offset;
        }

        if (line_end == line_start) {
            break;
        }

        draw_utf8_text_line(
            x,
            y + line * (font->line_height + ss(2)),
            font,
            text,
            line_start,
            line_end,
            color,
            max_width);

        line_start = line_end;
        while (text[line_start] == ' ') {
            line_start++;
        }
    }
}

static int utf8_text_width(const lv_font_t *font, const char *text, int max_width)
{
    if (font == NULL || text == NULL || text[0] == '\0') {
        return 0;
    }

    int width = 0;
    uint32_t offset = 0;
    while (text[offset] != '\0') {
        uint32_t letter_offset = offset;
        uint32_t letter = utf8_next(text, &offset);
        uint32_t next_offset = offset;
        uint32_t letter_next = utf8_next(text, &next_offset);
        if (letter == 0 || offset == letter_offset) {
            offset++;
            continue;
        }

        int adv = lv_font_get_glyph_width(font, letter, letter_next);
        if (max_width > 0 && width + adv > max_width) {
            break;
        }
        width += adv;
    }
    return width;
}

#endif

static int min_int(int a, int b)
{
    return a < b ? a : b;
}

static bool ascii_preview(const char *text, const char *fallback, char *out, size_t out_size)
{
    if (out == NULL || out_size == 0) {
        return false;
    }

    size_t out_len = 0;
    bool copied = false;
    bool pending_space = false;
    if (text == NULL || text[0] == '\0') {
        text = fallback;
    }

    for (const unsigned char *ch = (const unsigned char *)text;
         ch != NULL && *ch != '\0' && out_len + 1 < out_size;
         ch++) {
        if (*ch >= 0x80) {
            pending_space = copied;
            continue;
        }
        if (*ch == '\r' || *ch == '\n' || *ch == '\t' || *ch == ' ') {
            pending_space = copied;
            continue;
        }
        if (*ch < 0x20 || *ch > 0x7e) {
            continue;
        }
        if (pending_space && out_len + 1 < out_size) {
            out[out_len++] = ' ';
        }
        out[out_len++] = (char)*ch;
        copied = true;
        pending_space = false;
    }

    if (!copied && fallback != NULL && fallback[0] != '\0' && fallback != text) {
        for (const char *ch = fallback; *ch != '\0' && out_len + 1 < out_size; ch++) {
            if (*ch >= 0x20 && *ch <= 0x7e) {
                out[out_len++] = *ch;
            }
        }
        copied = out_len > 0;
    }
    if (!copied && out_len + 3 < out_size) {
        out[out_len++] = '-';
        out[out_len++] = '-';
    }
    out[out_len] = '\0';
    return copied;
}

static void draw_ascii_text_wrapped_xy(
    int x,
    int y,
    const char *text,
    uint16_t color,
    int max_width,
    int max_lines,
    int x_scale,
    int y_scale)
{
    if (max_lines <= 0 || x_scale <= 0 || y_scale <= 0) {
        return;
    }

    char clean[128];
    ascii_preview(text, "--", clean, sizeof(clean));

    int max_chars = max_width / (6 * x_scale);
    if (max_chars < 1) {
        max_chars = 1;
    }
    if (max_chars >= 64) {
        max_chars = 63;
    }

    const char *cursor = clean;
    const int line_h = 8 * y_scale + ss(2);
    for (int line_no = 0; line_no < max_lines && cursor[0] != '\0'; line_no++) {
        while (cursor[0] == ' ') {
            cursor++;
        }
        if (cursor[0] == '\0') {
            break;
        }

        int count = (int)strlen(cursor);
        if (count > max_chars) {
            count = max_chars;
            int break_at = -1;
            for (int i = count; i > 0; i--) {
                if (cursor[i] == ' ') {
                    break_at = i;
                    break;
                }
            }
            if (break_at > max_chars / 3) {
                count = break_at;
            }
        }

        char line[64];
        memcpy(line, cursor, (size_t)count);
        line[count] = '\0';
        draw_text_xy(x, y + line_no * line_h, line, x_scale, y_scale, color);

        cursor += count;
        while (cursor[0] == ' ') {
            cursor++;
        }
    }
}

static void draw_hud_wrapped_text(
    int x,
    int y,
    const char *text,
    const char *fallback,
    uint16_t color,
    int max_width,
    int max_lines,
    int x_scale,
    int y_scale)
{
    const char *render_text;
#if CONFIG_ORNAMENT_RICH_XIAOZHI_DISPLAY_ENABLED
    render_text = text != NULL && text[0] != '\0' ? text : fallback;
#else
    render_text = fallback != NULL ? fallback : text;
#endif
    if (render_text == NULL || render_text[0] == '\0') {
        return;
    }

#if CONFIG_ORNAMENT_RICH_XIAOZHI_DISPLAY_ENABLED
    (void)fallback;
    (void)x_scale;
    (void)y_scale;
    draw_utf8_text_wrapped(x, y, &font_puhui_16_4, render_text, color, max_width, max_lines);
#else
    draw_ascii_text_wrapped_xy(
        x,
        y,
        render_text,
        color,
        max_width,
        max_lines,
        x_scale,
        y_scale);
#endif
}

static void draw_hud_center_text(
    int y,
    const char *text,
    const char *fallback,
    uint16_t color,
    int preferred_scale)
{
#if CONFIG_ORNAMENT_RICH_XIAOZHI_DISPLAY_ENABLED
    (void)fallback;
    (void)preferred_scale;
    const int max_width = active_canvas->width - sx(32);
    int width = utf8_text_width(&font_puhui_16_4, text, max_width);
    int x = ((int)active_canvas->width - width) / 2;
    if (x < sx(8)) {
        x = sx(8);
    }
    draw_utf8_text(x, y, &font_puhui_16_4, text, color, max_width);
#else
    draw_text_center_fit(y, fallback != NULL && fallback[0] != '\0' ? fallback : text, preferred_scale, color);
#endif
}

static bool line_band_bounds(int y, int height, int margin, int *left, int *right)
{
    if (y < 0 || y + height > active_canvas->height || margin * 2 >= active_canvas->width) {
        return false;
    }
    *left = margin;
    *right = (int)active_canvas->width - 1 - margin;
    return true;
}

static int fit_text_scale(const char *text, int y, int preferred_scale, int margin)
{
    for (int scale = preferred_scale; scale > 1; scale--) {
        int left = 0;
        int right = 0;
        if (line_band_bounds(y, 7 * scale, margin, &left, &right) &&
            text_width(text, scale) <= right - left + 1) {
            return scale;
        }
    }
    return 1;
}

static void fit_text_xy(
    const char *text,
    int y,
    int preferred_x_scale,
    int preferred_y_scale,
    int min_x_scale,
    int min_y_scale,
    int margin,
    int *x_scale,
    int *y_scale,
    int *left,
    int *right)
{
    int best_x = min_x_scale;
    int best_y = min_y_scale;
    int best_left = 0;
    int best_right = (int)active_canvas->width - 1;

    for (int ys = preferred_y_scale; ys >= min_y_scale; ys--) {
        int candidate_left = 0;
        int candidate_right = 0;
        if (!line_band_bounds(y, 7 * ys, margin, &candidate_left, &candidate_right)) {
            continue;
        }
        int available = candidate_right - candidate_left + 1;
        for (int xs = preferred_x_scale; xs >= min_x_scale; xs--) {
            if (text_width_xy(text, xs) <= available) {
                *x_scale = xs;
                *y_scale = ys;
                *left = candidate_left;
                *right = candidate_right;
                return;
            }
        }
        best_x = min_x_scale;
        best_y = ys;
        best_left = candidate_left;
        best_right = candidate_right;
    }

    *x_scale = best_x;
    *y_scale = best_y;
    *left = best_left;
    *right = best_right;
}

static void draw_text_center_fit(int y, const char *text, int preferred_scale, uint16_t color)
{
    int margin = ss(8);
    int scale = fit_text_scale(text, y, preferred_scale, margin);
    int left = 0;
    int right = (int)active_canvas->width - 1;
    if (!line_band_bounds(y, 7 * scale, margin, &left, &right)) {
        return;
    }
    int width = text_width(text, scale);
    int x = left + (right - left + 1 - width) / 2;
    draw_text(x, y, text, scale, color);
}

static void draw_text_right_fit_xy(
    int right_edge,
    int y,
    const char *text,
    int preferred_x_scale,
    int preferred_y_scale,
    uint16_t color)
{
    int x_scale = 1;
    int y_scale = 1;
    int left = 0;
    int right = (int)active_canvas->width - 1;
    fit_text_xy(
        text,
        y,
        preferred_x_scale,
        preferred_y_scale,
        1,
        1,
        ss(8),
        &x_scale,
        &y_scale,
        &left,
        &right);

    right = min_int(right, right_edge);
    int width = text_width_xy(text, x_scale);
    int x = right - width;
    if (x < left) {
        x = left;
    }
    draw_text_xy(x, y, text, x_scale, y_scale, color);
}

static int percent_or_zero(int value)
{
    if (value < 0) {
        return 0;
    }
    if (value > 100) {
        return 100;
    }
    return value;
}

static void draw_segment_bar(int x, int y, int w, int h, int percent, uint16_t active, uint16_t inactive)
{
    const int gap = ss(4);
    const int segments = 26;
    int segment_w = (w - gap * (segments - 1)) / segments;
    if (segment_w < 1) {
        segment_w = 1;
    }
    int active_segments = percent * segments / 100;
    for (int i = 0; i < segments; i++) {
        uint16_t color = i < active_segments ? active : inactive;
        fill_rect(x + i * (segment_w + gap), y, segment_w, h, color);
    }
}

static const char *time_tail(const char *timestamp)
{
    if (timestamp == NULL || timestamp[0] == '\0') {
        return "--:--";
    }
    const char *t = strchr(timestamp, 'T');
    return t != NULL && strlen(t) >= 6 ? t + 1 : timestamp;
}

static void reset_in_text(char *out, size_t out_size, const char *timestamp)
{
    const char *tail = time_tail(timestamp);
    if (strlen(tail) < 5 || tail[2] != ':') {
        snprintf(out, out_size, "RESET --:--");
        return;
    }
    snprintf(out, out_size, "RESET %c%c:%c%c", tail[0], tail[1], tail[3], tail[4]);
}

static void reset_date_text(char *out, size_t out_size, const char *timestamp)
{
    const char *tail = time_tail(timestamp);
    if (strlen(tail) < 5 || tail[2] != ':') {
        snprintf(out, out_size, "RESET --:--");
        return;
    }
    snprintf(out, out_size, "RESET MON %c%c:%c%c", tail[0], tail[1], tail[3], tail[4]);
}

static uint16_t status_color(ornament_status_t status)
{
    switch (status) {
    case ORNAMENT_STATUS_RUNNING:
        return HUD_CYAN;
    case ORNAMENT_STATUS_DONE:
        return HUD_GREEN;
    case ORNAMENT_STATUS_ERROR:
        return HUD_RED;
    case ORNAMENT_STATUS_EVENT:
        return HUD_AMBER;
    case ORNAMENT_STATUS_IDLE:
    default:
        return HUD_MUTED;
    }
}

static const char *status_text_from_enum(ornament_status_t status)
{
    switch (status) {
    case ORNAMENT_STATUS_RUNNING:
        return "RUNNING";
    case ORNAMENT_STATUS_DONE:
        return "DONE";
    case ORNAMENT_STATUS_ERROR:
        return "ERROR";
    case ORNAMENT_STATUS_EVENT:
        return "EVENT";
    case ORNAMENT_STATUS_IDLE:
    default:
        return "IDLE";
    }
}

static void draw_standby_wallpaper(void)
{
    if (standby_wallpaper_client_copy_frame(
            active_canvas->pixels,
            (size_t)active_canvas->width * active_canvas->height)) {
        return;
    }
#if CONFIG_ORNAMENT_STANDBY_WALLPAPER_ENABLED
    if (active_canvas->width != STANDBY_WALLPAPER_WIDTH || active_canvas->height != STANDBY_WALLPAPER_HEIGHT) {
        clear_canvas(HUD_BLACK);
        return;
    }
    memcpy(
        active_canvas->pixels,
        standby_wallpaper_rgb565,
        (size_t)STANDBY_WALLPAPER_WIDTH * STANDBY_WALLPAPER_HEIGHT * sizeof(uint16_t));
#else
    clear_canvas(HUD_BLACK);
    for (int y = 0; y < active_canvas->height; y++) {
        uint8_t blue = (uint8_t)(18 + (uint32_t)y * 24U / active_canvas->height);
        uint8_t green = (uint8_t)(10 + (uint32_t)y * 10U / active_canvas->height);
        uint16_t color = display_core_rgb565(2, green, blue);
        for (int x = 0; x < active_canvas->width; x++) {
            active_canvas->pixels[y * active_canvas->width + x] = color;
        }
    }
    for (int i = 0; i < 6; i++) {
        int x = sx(28 + i * 62);
        int h = sy(34 + (i % 3) * 18);
        fill_rect(x, active_canvas->height - h, ss(3), h, HUD_DIM_CYAN);
    }
    draw_circle_outline(active_canvas->center_x, active_canvas->center_y, ss(92), ss(1), HUD_DIM_BLUE);
    draw_circle_outline(active_canvas->center_x, active_canvas->center_y, ss(118), ss(1), HUD_DIM_CYAN);
    draw_hline(sx(34), sx(326), sy(222), ss(1), HUD_DIM_CYAN);
#endif
}

static void standby_temperature_text(const ornament_state_t *state, char *out, size_t out_size)
{
    if (out_size == 0) {
        return;
    }
    if (state->has_weather && state->weather_temperature_c != INT32_MIN) {
        snprintf(out, out_size, "%d", state->weather_temperature_c);
        return;
    }
    snprintf(out, out_size, "--");
}

static void draw_standby_temperature(int x, int y, const char *value, int x_scale, int y_scale, uint16_t color)
{
    draw_text_xy(x, y, value, x_scale, y_scale, color);
    int value_width = text_width_xy(value, x_scale);
    int degree_x = x + value_width + ss(4);
    int degree_y = y + ss(5);
    fill_circle(degree_x, degree_y, ss(3), color);
    draw_text_xy(degree_x + ss(8), y, "C", x_scale, y_scale, color);
}

static int standby_temperature_width(const char *value, int x_scale)
{
    return text_width_xy(value, x_scale) + ss(8) + text_width_xy("C", x_scale);
}

static void standby_reset_text(const ornament_state_t *state, char *out, size_t out_size)
{
    if (out_size == 0) {
        return;
    }
    if (state == NULL || !state->has_quota) {
        snprintf(out, out_size, "RESET --:--");
        return;
    }
    reset_in_text(out, out_size, state->primary_resets_at);
}

static void draw_weather_cloud(int cx, int cy, int size, uint16_t color)
{
    fill_circle(cx - size / 2, cy + size / 8, size / 3, color);
    fill_circle(cx - size / 10, cy - size / 7, size / 2, color);
    fill_circle(cx + size / 2, cy + size / 10, size / 3, color);
    fill_rect(cx - size * 3 / 4, cy + size / 10, size * 3 / 2, size / 3, color);
}

static void draw_weather_sun(int cx, int cy, int size)
{
    int r = size / 3;
    fill_circle(cx, cy, r, HUD_AMBER);
    fill_rect(cx - ss(2), cy - size / 2, ss(4), size / 6, HUD_AMBER);
    fill_rect(cx - ss(2), cy + size / 3, ss(4), size / 6, HUD_AMBER);
    fill_rect(cx - size / 2, cy - ss(2), size / 6, ss(4), HUD_AMBER);
    fill_rect(cx + size / 3, cy - ss(2), size / 6, ss(4), HUD_AMBER);
    fill_rect(cx - size / 3, cy - size / 3, ss(5), ss(5), HUD_AMBER);
    fill_rect(cx + size / 4, cy - size / 3, ss(5), ss(5), HUD_AMBER);
    fill_rect(cx - size / 3, cy + size / 4, ss(5), ss(5), HUD_AMBER);
    fill_rect(cx + size / 4, cy + size / 4, ss(5), ss(5), HUD_AMBER);
}

static void draw_weather_rain(int cx, int cy, int size)
{
    draw_weather_cloud(cx, cy - size / 5, size, HUD_WHITE);
    for (int i = -1; i <= 1; i++) {
        int x = cx + i * size / 4;
        fill_rect(x, cy + size / 4, ss(3), size / 4, HUD_CYAN);
    }
}

static void draw_weather_snow(int cx, int cy, int size)
{
    draw_weather_cloud(cx, cy - size / 5, size, HUD_WHITE);
    for (int i = -1; i <= 1; i++) {
        int x = cx + i * size / 4;
        int y = cy + size / 3;
        fill_rect(x - ss(4), y, ss(9), ss(2), HUD_CYAN);
        fill_rect(x, y - ss(4), ss(2), ss(9), HUD_CYAN);
    }
}

static void draw_weather_fog(int cx, int cy, int size)
{
    draw_weather_cloud(cx, cy - size / 4, size, HUD_MUTED);
    for (int i = 0; i < 3; i++) {
        int y = cy + size / 5 + i * ss(8);
        draw_hline(cx - size / 2, cx + size / 2, y, ss(2), HUD_CYAN);
    }
}

static void draw_weather_storm(int cx, int cy, int size)
{
    draw_weather_cloud(cx, cy - size / 5, size, HUD_WHITE);
    fill_rect(cx - ss(3), cy + size / 6, ss(7), size / 5, HUD_AMBER);
    fill_rect(cx + ss(2), cy + size / 3, ss(7), size / 5, HUD_AMBER);
    fill_rect(cx - ss(8), cy + size / 3, ss(13), ss(4), HUD_AMBER);
}

static void draw_weather_unknown(int cx, int cy, int size)
{
    draw_rect_outline(cx - size / 2, cy - size / 2, size, size, ss(2), HUD_MUTED);
    draw_text(cx - ss(7), cy - ss(12), "?", ss(3), HUD_WHITE);
}

static void draw_weather_partly_cloudy(int cx, int cy, int size)
{
    int sun_r = size * 2 / 7;
    int sun_cx = cx - size / 3;
    int sun_cy = cy - size / 8;
    /* small sun peeking from behind cloud */
    fill_circle(sun_cx, sun_cy, sun_r, HUD_AMBER);
    fill_rect(sun_cx - ss(1), sun_cy - sun_r - ss(2), ss(3), ss(4), HUD_AMBER);
    fill_rect(sun_cx - ss(1), sun_cy + sun_r - ss(2), ss(3), ss(4), HUD_AMBER);
    fill_rect(sun_cx - sun_r - ss(2), sun_cy - ss(1), ss(4), ss(3), HUD_AMBER);
    fill_rect(sun_cx + sun_r - ss(2), sun_cy - ss(1), ss(4), ss(3), HUD_AMBER);
    /* cloud overlapping sun */
    int cl_cx = cx + size / 6;
    int cl_cy = cy;
    int cl_sz = size * 4 / 5;
    fill_circle(cl_cx - cl_sz / 2, cl_cy + cl_sz / 8, cl_sz / 3, HUD_WHITE);
    fill_circle(cl_cx - cl_sz / 10, cl_cy - cl_sz / 7, cl_sz / 2, HUD_WHITE);
    fill_circle(cl_cx + cl_sz / 2, cl_cy + cl_sz / 10, cl_sz / 3, HUD_WHITE);
    fill_rect(cl_cx - cl_sz * 3 / 4, cl_cy + cl_sz / 10, cl_sz * 3 / 2, cl_sz / 3, HUD_WHITE);
}

static void draw_weather_drizzle(int cx, int cy, int size)
{
    draw_weather_cloud(cx, cy - size / 5, size, HUD_WHITE);
    for (int i = -1; i <= 1; i++) {
        int x = cx + i * size / 4;
        fill_rect(x, cy + size / 4, ss(2), size / 7, HUD_CYAN);
    }
}

static void draw_weather_heavy_rain(int cx, int cy, int size)
{
    draw_weather_cloud(cx, cy - size / 5, size, HUD_WHITE);
    for (int i = -2; i <= 2; i++) {
        int x = cx + i * size / 5;
        fill_rect(x, cy + size / 5, ss(3), size / 3, HUD_CYAN);
    }
}

static void draw_weather_sleet(int cx, int cy, int size)
{
    draw_weather_cloud(cx, cy - size / 5, size, HUD_WHITE);
    /* rain drop left */
    fill_rect(cx - size / 4, cy + size / 5, ss(3), size / 5, HUD_CYAN);
    /* snow asterisk right */
    int sx = cx + size / 4;
    int sy = cy + size / 3;
    fill_rect(sx - ss(3), sy, ss(7), ss(2), HUD_CYAN);
    fill_rect(sx, sy - ss(3), ss(2), ss(7), HUD_CYAN);
    /* rain drop center */
    fill_rect(cx, cy + size / 4, ss(3), size / 6, HUD_CYAN);
}

static void draw_weather_haze(int cx, int cy, int size)
{
    /* dim sun */
    int r = size / 4;
    fill_circle(cx, cy - size / 5, r, HUD_MUTED);
    /* horizontal haze bands */
    for (int i = 0; i < 4; i++) {
        int y = cy - size / 3 + i * ss(10);
        int w = size - i * ss(6);
        draw_hline(cx - w / 2, cx + w / 2, y, ss(2), HUD_MUTED);
    }
}

static void draw_weather_windy(int cx, int cy, int size)
{
    /* three wind bars, decreasing in length */
    for (int i = 0; i < 3; i++) {
        int y = cy - size / 4 + i * size / 4;
        int w = size - i * ss(6);
        int x_start = cx - size / 3 + i * ss(4);
        fill_rect(x_start, y, w * 2 / 3, ss(3), HUD_CYAN);
        fill_circle(x_start, y + ss(1), ss(2), HUD_CYAN);
    }
}

static void draw_weather_icon(const ornament_state_t *state, int cx, int cy, int size)
{
    if (state == NULL || !state->has_weather || strcmp(state->weather_status, "ok") != 0) {
        draw_weather_unknown(cx, cy, size);
        return;
    }
    if (strcmp(state->weather_icon, "sun") == 0) {
        draw_weather_sun(cx, cy, size);
    } else if (strcmp(state->weather_icon, "partly-cloudy") == 0) {
        draw_weather_partly_cloudy(cx, cy, size);
    } else if (strcmp(state->weather_icon, "cloud") == 0) {
        draw_weather_cloud(cx, cy, size, HUD_WHITE);
    } else if (strcmp(state->weather_icon, "drizzle") == 0) {
        draw_weather_drizzle(cx, cy, size);
    } else if (strcmp(state->weather_icon, "rain") == 0) {
        draw_weather_rain(cx, cy, size);
    } else if (strcmp(state->weather_icon, "heavy-rain") == 0) {
        draw_weather_heavy_rain(cx, cy, size);
    } else if (strcmp(state->weather_icon, "sleet") == 0) {
        draw_weather_sleet(cx, cy, size);
    } else if (strcmp(state->weather_icon, "snow") == 0) {
        draw_weather_snow(cx, cy, size);
    } else if (strcmp(state->weather_icon, "fog") == 0) {
        draw_weather_fog(cx, cy, size);
    } else if (strcmp(state->weather_icon, "haze") == 0) {
        draw_weather_haze(cx, cy, size);
    } else if (strcmp(state->weather_icon, "storm") == 0) {
        draw_weather_storm(cx, cy, size);
    } else if (strcmp(state->weather_icon, "windy") == 0) {
        draw_weather_windy(cx, cy, size);
    } else {
        draw_weather_unknown(cx, cy, size);
    }
}

static int wifi_signal_bars(const ornament_state_t *state)
{
    if (state == NULL || !state->wifi_connected) {
        return 0;
    }
    if (state->wifi_rssi >= -67) {
        return 4;
    }
    if (state->wifi_rssi >= -75) {
        return 3;
    }
    if (state->wifi_rssi >= -82) {
        return 2;
    }
    return 1;
}

static void draw_wifi_signal_icon(int center_x, int center_y, int bars)
{
    const uint16_t active = bars > 0 ? HUD_WHITE : HUD_AMBER;
    const uint16_t inactive = bars > 0 ? HUD_MUTED : HUD_DIM_AMBER;
    const int bar_width = ss(5);
    const int gap = ss(4);
    const int heights[] = {ss(9), ss(15), ss(21), ss(27)};
    const int total_width = 4 * bar_width + 3 * gap;
    const int left = center_x - total_width / 2;
    const int bottom = center_y + ss(10);

    for (int i = 0; i < 4; i++) {
        uint16_t color = i < bars ? active : inactive;
        fill_rect(left + i * (bar_width + gap), bottom - heights[i], bar_width, heights[i], color);
    }
}

static void draw_bridge_offline_icon(int center_x, int center_y)
{
    const int body_w = ss(26);
    const int body_h = ss(18);
    const int thickness = ss(2);
    const int x = center_x - body_w / 2;
    const int y = center_y - body_h / 2;

    draw_rect_outline(x, y, body_w, body_h, thickness, HUD_AMBER);
    fill_rect(x + body_w / 2 - ss(1), y - ss(5), ss(2), ss(5), HUD_AMBER);
    fill_rect(x + ss(7), y + body_h, ss(4), ss(5), HUD_AMBER);
    fill_rect(x + body_w - ss(11), y + body_h, ss(4), ss(5), HUD_AMBER);
    draw_hline(x - ss(4), x + body_w + ss(4), center_y + ss(10), thickness, HUD_AMBER);
}

static void draw_bridge_reconnecting_icon(const ornament_state_t *state, int center_x, int center_y)
{
    int phase = state != NULL ? (state->active_dot_phase & 0x03) : 0;
    const int orbit = ss(12);
    const int dot_radius = ss(3);
    const int trail_radius = ss(2);

    fill_circle(center_x, center_y, ss(9), HUD_DIM_BLUE);
    draw_rect_outline(center_x - ss(9), center_y - ss(9), ss(18), ss(18), ss(1), HUD_CYAN);

    static const int offsets[4][2] = {
        {0, -1},
        {1, 0},
        {0, 1},
        {-1, 0},
    };

    for (int i = 0; i < 4; i++) {
        int index = (phase + i) & 0x03;
        int dx = offsets[index][0] * orbit;
        int dy = offsets[index][1] * orbit;
        uint16_t color = i == 0 ? HUD_CYAN : (i == 1 ? HUD_TEAL : HUD_DIM_CYAN);
        int radius = i == 0 ? dot_radius : trail_radius;
        fill_circle(center_x + dx, center_y + dy, radius, color);
    }
}

static void fill_round_rect(int x, int y, int w, int h, int radius, uint16_t color)
{
    if (w <= 0 || h <= 0) {
        return;
    }
    if (radius < 0) {
        radius = 0;
    }
    int max_radius = w < h ? w / 2 : h / 2;
    if (radius > max_radius) {
        radius = max_radius;
    }
    if (radius == 0) {
        fill_rect(x, y, w, h, color);
        return;
    }

    fill_rect(x + radius, y, w - radius * 2, h, color);
    fill_rect(x, y + radius, radius, h - radius * 2, color);
    fill_rect(x + w - radius, y + radius, radius, h - radius * 2, color);
    fill_circle(x + radius, y + radius, radius, color);
    fill_circle(x + w - radius - 1, y + radius, radius, color);
    fill_circle(x + radius, y + h - radius - 1, radius, color);
    fill_circle(x + w - radius - 1, y + h - radius - 1, radius, color);
}

static void draw_hud_chamfer_box(int x, int y, int w, int h, int cut, int thickness, uint16_t color)
{
    if (w <= cut * 2 || h <= cut * 2 || thickness <= 0) {
        return;
    }
    draw_hline(x + cut, x + w - cut, y, thickness, color);
    draw_hline(x + cut, x + w - cut, y + h - thickness, thickness, color);
    draw_vline(x, y + cut, y + h - cut, thickness, color);
    draw_vline(x + w - thickness, y + cut, y + h - cut, thickness, color);
    draw_line(x + cut, y, x, y + cut, thickness, color);
    draw_line(x + w - cut, y, x + w - thickness, y + cut, thickness, color);
    draw_line(x, y + h - cut, x + cut, y + h - thickness, thickness, color);
    draw_line(x + w - thickness, y + h - cut, x + w - cut, y + h - thickness, thickness, color);
}

static void draw_hud_user_icon(int x, int y, int size, uint16_t color)
{
    draw_hud_chamfer_box(x, y, size, size, ss(8), ss(1), color);
    draw_circle_outline(x + size / 2, y + size / 3, size / 8, ss(2), color);
    draw_circle_outline(x + size / 2, y + size * 2 / 3, size / 4, ss(2), color);
    fill_rect(x + size / 4, y + size * 2 / 3, size / 2, size / 3, HUD_HUD_BG);
}

static void draw_hud_bot_icon(int x, int y, int size, uint16_t color)
{
    draw_hud_chamfer_box(x, y, size, size, ss(8), ss(1), color);
    int head_x = x + size / 4;
    int head_y = y + size / 3;
    int head_w = size / 2;
    int head_h = size / 3;
    draw_rect_outline(head_x, head_y, head_w, head_h, ss(2), color);
    fill_circle(head_x + head_w / 3, head_y + head_h / 2, ss(2), color);
    fill_circle(head_x + head_w * 2 / 3, head_y + head_h / 2, ss(2), color);
    fill_circle(x + size / 2, y + size / 4, ss(2), color);
    draw_vline(x + size / 2, y + size / 4, head_y, ss(1), color);
    draw_hline(head_x - ss(4), head_x, head_y + head_h / 2, ss(2), color);
    draw_hline(head_x + head_w, head_x + head_w + ss(4), head_y + head_h / 2, ss(2), color);
}

static void draw_hud_microphone(int cx, int cy, int size, uint16_t color)
{
    int w = size / 4;
    int h = size / 2;
    draw_rect_outline(cx - w / 2, cy - h / 2, w, h, ss(2), color);
    draw_line(cx - size / 4, cy, cx - size / 4, cy + size / 6, ss(2), color);
    draw_line(cx + size / 4, cy, cx + size / 4, cy + size / 6, ss(2), color);
    draw_hline(cx - size / 5, cx + size / 5, cy + size / 5, ss(2), color);
    draw_vline(cx, cy + size / 5, cy + size / 3, ss(2), color);
    draw_hline(cx - size / 6, cx + size / 6, cy + size / 3, ss(2), color);
}

static void draw_hud_speaker(int cx, int cy, int size, uint16_t color)
{
    fill_rect(cx - size / 3, cy - size / 6, size / 6, size / 3, color);
    draw_line(cx - size / 6, cy - size / 6, cx + size / 12, cy - size / 3, ss(2), color);
    draw_line(cx - size / 6, cy + size / 6, cx + size / 12, cy + size / 3, ss(2), color);
    draw_vline(cx + size / 12, cy - size / 3, cy + size / 3, ss(2), color);
    draw_circle_outline(cx + size / 8, cy, size / 4, ss(2), color);
    draw_circle_outline(cx + size / 8, cy, size / 2, ss(2), color);
    fill_rect(cx + size / 8 - size / 2, cy - size / 2 - ss(1), size / 2, size + ss(2), HUD_HUD_BG);
}

static void draw_hud_check(int cx, int cy, int size, uint16_t color)
{
    draw_circle_outline(cx, cy, size / 2, ss(2), color);
    draw_line(cx - size / 5, cy, cx - size / 16, cy + size / 6, ss(3), color);
    draw_line(cx - size / 16, cy + size / 6, cx + size / 4, cy - size / 5, ss(3), color);
}

static void draw_hud_wave(int cx, int cy, int width, int height, uint16_t color)
{
    const int bars = 11;
    const int gap = width / (bars * 2);
    int start_x = cx - (bars / 2) * gap;
    for (int i = 0; i < bars; i++) {
        int distance = abs(i - bars / 2);
        int bar_h = height - distance * height / (bars / 2 + 1);
        if ((i & 1) == 0) {
            bar_h /= 2;
        }
        int x = start_x + i * gap;
        draw_vline(x, cy - bar_h / 2, cy + bar_h / 2, ss(2), color);
    }
}

static void draw_hud_segmented_ring(int cx, int cy, int radius, uint16_t color)
{
    int thick = ss(3);
    int mid_thick = ss(2);
    int gap = ss(14);
    if (thick < 2) {
        thick = 2;
    }
    if (mid_thick < 1) {
        mid_thick = 1;
    }
    if (gap < 8) {
        gap = 8;
    }

    draw_circle_outline(cx, cy, radius, thick, color);
    draw_circle_outline(cx, cy, radius - ss(8), mid_thick, color);
    draw_circle_outline(cx, cy, radius - ss(17), 1, HUD_HUD_DIM);

    fill_rect(cx - gap / 2, cy - radius - thick, gap, thick * 4, HUD_HUD_BG);
    fill_rect(cx - gap / 2, cy + radius - thick * 3, gap, thick * 5, HUD_HUD_BG);
    fill_rect(cx - radius - thick, cy - gap / 2, thick * 5, gap, HUD_HUD_BG);
    fill_rect(cx + radius - thick * 4, cy - gap / 2, thick * 5, gap, HUD_HUD_BG);

    draw_vline(cx - radius + ss(2), cy - ss(8), cy - ss(2), thick, color);
    draw_vline(cx - radius + ss(2), cy + ss(2), cy + ss(8), thick, color);
    draw_vline(cx + radius - ss(4), cy - ss(8), cy - ss(2), thick, color);
    draw_vline(cx + radius - ss(4), cy + ss(2), cy + ss(8), thick, color);
}

static void draw_hud_panel_marks(int x, int y, int w, int h, uint16_t color)
{
    int line = ss(2);
    if (line < 1) {
        line = 1;
    }

    draw_hline(x + ss(40), x + w - ss(40), y + ss(12), 1, HUD_HUD_DIM);
    for (int i = 0; i < 3; i++) {
        int mark_x = x + w - ss(28) + i * ss(8);
        draw_line(mark_x, y + ss(14), mark_x + ss(6), y + ss(8), line, color);
    }
    draw_hline(x + ss(12), x + ss(18), y + h - ss(12), line, color);
    draw_hline(x + ss(22), x + ss(28), y + h - ss(12), line, color);
    draw_hline(x + ss(32), x + ss(38), y + h - ss(12), line, color);
}

static const char *xiaozhi_hud_main_ascii(xiaozhi_client_state_t state);

static const char *xiaozhi_hud_user_hint(const xiaozhi_client_snapshot_t *snapshot)
{
    if (snapshot == NULL) {
        return "";
    }
    if (snapshot->activation_pending) {
        return snapshot->activation_code[0] != '\0' ? snapshot->activation_code : "";
    }
    if (snapshot->last_stt[0] != '\0') {
        return snapshot->last_stt;
    }
    switch (snapshot->state) {
    case XIAOZHI_CLIENT_STATE_ERROR:
    case XIAOZHI_CLIENT_STATE_CONFIG_MISSING:
        return snapshot->last_error[0] != '\0' ? snapshot->last_error : "";
    case XIAOZHI_CLIENT_STATE_LISTENING:
    case XIAOZHI_CLIENT_STATE_SPEAKING:
    case XIAOZHI_CLIENT_STATE_CONNECTING:
    case XIAOZHI_CLIENT_STATE_IDLE:
    case XIAOZHI_CLIENT_STATE_DISABLED:
    default:
        return "";
    }
}

static const char *xiaozhi_hud_main_text(xiaozhi_client_state_t state)
{
#if CONFIG_ORNAMENT_RICH_XIAOZHI_DISPLAY_ENABLED
    switch (state) {
    case XIAOZHI_CLIENT_STATE_LISTENING:
        return "\xE5\x80\xBE\xE5\x90\xAC\xE4\xB8\xAD";
    case XIAOZHI_CLIENT_STATE_SPEAKING:
        return "\xE5\x9B\x9E\xE7\xAD\x94\xE4\xB8\xAD";
    case XIAOZHI_CLIENT_STATE_CONNECTING:
        return "\xE8\xBF\x9E\xE6\x8E\xA5\xE4\xB8\xAD";
    case XIAOZHI_CLIENT_STATE_ERROR:
    case XIAOZHI_CLIENT_STATE_CONFIG_MISSING:
        return "\xE5\xBC\x82\xE5\xB8\xB8";
    case XIAOZHI_CLIENT_STATE_IDLE:
    case XIAOZHI_CLIENT_STATE_DISABLED:
    default:
        return "\xE5\xBE\x85\xE6\x9C\xBA\xE4\xB8\xAD";
    }
#else
    return xiaozhi_hud_main_ascii(state);
#endif
}

static const char *xiaozhi_hud_main_ascii(xiaozhi_client_state_t state)
{
    switch (state) {
    case XIAOZHI_CLIENT_STATE_LISTENING:
        return "LISTENING";
    case XIAOZHI_CLIENT_STATE_SPEAKING:
        return "ANSWERING";
    case XIAOZHI_CLIENT_STATE_CONNECTING:
        return "CONNECTING";
    case XIAOZHI_CLIENT_STATE_ERROR:
    case XIAOZHI_CLIENT_STATE_CONFIG_MISSING:
        return "ERROR";
    case XIAOZHI_CLIENT_STATE_IDLE:
    case XIAOZHI_CLIENT_STATE_DISABLED:
    default:
        return "STANDBY";
    }
}

static const char *xiaozhi_hud_assistant_hint(const xiaozhi_client_snapshot_t *snapshot, const char *tts_text)
{
    if (snapshot == NULL) {
        return "";
    }
    if (snapshot->activation_pending) {
        return snapshot->activation_code[0] != '\0' ? snapshot->activation_code :
            (snapshot->activation_message[0] != '\0' ? snapshot->activation_message : "");
    }
    if (snapshot->last_tts[0] != '\0') {
        return tts_text;
    }
    switch (snapshot->state) {
    case XIAOZHI_CLIENT_STATE_ERROR:
    case XIAOZHI_CLIENT_STATE_CONFIG_MISSING:
        return snapshot->last_error[0] != '\0' ? snapshot->last_error : "";
    case XIAOZHI_CLIENT_STATE_LISTENING:
    case XIAOZHI_CLIENT_STATE_SPEAKING:
    case XIAOZHI_CLIENT_STATE_CONNECTING:
    case XIAOZHI_CLIENT_STATE_IDLE:
    case XIAOZHI_CLIENT_STATE_DISABLED:
    default:
        return "";
    }
}

static const char *xiaozhi_hud_assistant_ascii(const xiaozhi_client_snapshot_t *snapshot, const char *tts_text)
{
    if (snapshot == NULL) {
        return "";
    }
    if (snapshot->activation_pending) {
        return snapshot->activation_code[0] != '\0' ? snapshot->activation_code :
            (snapshot->activation_message[0] != '\0' ? snapshot->activation_message : "");
    }
    if (snapshot->last_tts[0] != '\0') {
        return tts_text;
    }
    switch (snapshot->state) {
    case XIAOZHI_CLIENT_STATE_ERROR:
    case XIAOZHI_CLIENT_STATE_CONFIG_MISSING:
        return snapshot->last_error[0] != '\0' ? snapshot->last_error : "";
    case XIAOZHI_CLIENT_STATE_LISTENING:
    case XIAOZHI_CLIENT_STATE_SPEAKING:
    case XIAOZHI_CLIENT_STATE_CONNECTING:
    case XIAOZHI_CLIENT_STATE_IDLE:
    case XIAOZHI_CLIENT_STATE_DISABLED:
    default:
        return "";
    }
}

static const char *xiaozhi_hud_done_text(void)
{
#if CONFIG_ORNAMENT_RICH_XIAOZHI_DISPLAY_ENABLED
    return "\xE5\xB7\xB2\xE5\xAE\x8C\xE6\x88\x90";
#else
    return "DONE";
#endif
}

static void draw_standby_background(void)
{
    draw_standby_wallpaper();
    blend_rect(0, 0, active_canvas->width, active_canvas->height, HUD_BLACK, 118);
}

static void draw_standby_header(const char *time_text, const char *date_text, const char *reset_text)
{
    draw_text_center_fit(sy(38), time_text, ss(7), HUD_WHITE);
    draw_text_center_fit(sy(106), date_text, ss(3), HUD_WHITE);
    draw_text_center_fit(sy(138), reset_text, ss(2), HUD_WHITE);
}

static void draw_standby_weather_row(const ornament_state_t *state, const char *temp_text)
{
    const int icon_size = ss(42);
    const int icon_width = icon_size * 3 / 2;
    const int gap = ss(20);
    const int temp_x_scale = ss(5);
    const int temp_y_scale = ss(5);
    const int temp_width = standby_temperature_width(temp_text, temp_x_scale);
    const int row_width = icon_width + gap + temp_width;
    const int row_left = active_canvas->center_x - row_width / 2;
    const int row_center_y = sy(252);
    const int temp_y = row_center_y - (7 * temp_y_scale) / 2;
    const int icon_cx = row_left + icon_width / 2;
    const int temp_x = row_left + icon_width + gap;

    draw_weather_icon(state, icon_cx, row_center_y, icon_size);
    draw_standby_temperature(temp_x, temp_y, temp_text, temp_x_scale, temp_y_scale, HUD_WHITE);
}

static void draw_standby_connectivity(const ornament_state_t *state)
{
    draw_wifi_signal_icon(active_canvas->center_x, sy(342), wifi_signal_bars(state));
    if (state != NULL && state->bridge_offline && state->bridge_reconnecting) {
        draw_bridge_reconnecting_icon(state, active_canvas->center_x + ss(55), sy(342));
    } else if (state != NULL && state->bridge_offline) {
        draw_bridge_offline_icon(active_canvas->center_x + ss(55), sy(342));
    }
}

static void status_label(const ornament_state_t *state, char *out, size_t out_size)
{
    if (out_size == 0) {
        return;
    }

    ornament_status_t status = ornament_state_panel_status(state);
    if (state != NULL && status == ORNAMENT_STATUS_RUNNING && state->active_task_count > 1) {
        snprintf(out, out_size, "%d TASKS", state->active_task_count);
        return;
    }

    const char *label = "DONE";
    switch (status) {
    case ORNAMENT_STATUS_RUNNING:
        label = "1 TASK";
        break;
    case ORNAMENT_STATUS_DONE:
        label = "DONE";
        break;
    case ORNAMENT_STATUS_ERROR:
        label = "HOOK ERROR";
        break;
    case ORNAMENT_STATUS_EVENT:
        label = "EVENT";
        break;
    case ORNAMENT_STATUS_IDLE:
    default:
        label = "DONE";
        break;
    }
    snprintf(out, out_size, "%s", label);
}

static uint16_t active_dot_color_for(const ornament_state_t *state, uint16_t status)
{
    if (ornament_state_panel_status(state) != ORNAMENT_STATUS_RUNNING) {
        return status;
    }

    switch (state->active_dot_phase & 0x03) {
    case 0:
        return HUD_WHITE;
    case 1:
    case 3:
        return HUD_CYAN;
    case 2:
    default:
        return HUD_DIM_CYAN;
    }
}

static void draw_status_badge(const ornament_state_t *state)
{
    ornament_status_t panel_status = ornament_state_panel_status(state);
    uint16_t color = status_color(panel_status);
    if (state->done_flash_active && panel_status == ORNAMENT_STATUS_DONE) {
        color = state->done_flash_on ? HUD_GREEN : HUD_DIM_GREEN;
    }
    uint16_t dot_color = active_dot_color_for(state, color);
    char label[24];
    status_label(state, label, sizeof(label));
    const int y = sy(312);
    const int dot = ss(10);
    int x_scale = ss(2);
    int y_scale = ss(2);
    int text_w = text_width_xy(label, x_scale);
    int text_x = (int)active_canvas->center_x - text_w / 2;
    int dot_x = text_x - ss(10);
    int dot_y = y + (7 * y_scale - dot) / 2;
    if (dot_y < y) {
        dot_y = y;
    }
    fill_circle(dot_x, dot_y + dot / 2, dot / 2, dot_color);
    draw_text_xy(text_x, y, label, x_scale, y_scale, color);

    if (panel_status == ORNAMENT_STATUS_ERROR) {
        draw_rect_outline(sx(18), sy(18), sx(324), sy(324), ss(2), HUD_RED);
    }
}

static void draw_system_widget(const ornament_state_t *state, uint16_t accent)
{
    char wifi_text[32];
    const char *time_text = state->local_time[0] != '\0' ? state->local_time : "--:--";
    const char *date_text = state->local_date[0] != '\0' ? state->local_date : "-- --";

    if (state->wifi_connected) {
        snprintf(wifi_text, sizeof(wifi_text), "WIFI %dDBM", state->wifi_rssi);
    } else {
        snprintf(wifi_text, sizeof(wifi_text), "WIFI OFF");
    }

    draw_text_xy(sx(34), sy(65), time_text, ss(1), ss(1), accent);
    draw_text_xy(sx(118), sy(65), date_text, ss(1), ss(1), HUD_MUTED);
    draw_text_right_fit_xy(active_canvas->width - sx(34), sy(65), wifi_text, ss(1), ss(1), state->wifi_connected ? HUD_MUTED : HUD_AMBER);
}

static void draw_logo_header(const ornament_state_t *state)
{
    draw_text_xy(sx(34), sy(34), "CODEX QUOTA", ss(2), ss(2), HUD_WHITE);
    draw_system_widget(state, ui_accent_color_for(state));
}

static void draw_quota_panel(
    int y,
    const char *label,
    int percent,
    const char *reset_text,
    uint16_t active_color,
    uint16_t panel_color,
    uint16_t inactive_color)
{
    char percent_text[16];

    draw_rect_outline(sx(28), sy(y), sx(304), sy(82), ss(1), panel_color);
    draw_text_xy(sx(38), sy(y + 13), label, ss(2), ss(2), HUD_MUTED);
    snprintf(percent_text, sizeof(percent_text), "%d%%", percent);
    draw_text_right_fit_xy(active_canvas->width - sx(38), sy(y + 6), percent_text, ss(3), ss(3), active_color);
    draw_segment_bar(sx(38), sy(y + 52), sx(284), ss(8), percent, active_color, inactive_color);
    draw_text_xy(sx(38), sy(y + 68), reset_text, 1, 1, HUD_WHITE);
}

static void draw_quota_rows(const ornament_state_t *state)
{
    int primary = percent_or_zero(state->primary_remaining_percent);
    int weekly = percent_or_zero(state->secondary_remaining_percent);
    uint16_t alert = ui_accent_color_for(state);
    char reset_text[32];
    char reset_time[24];

    reset_in_text(reset_time, sizeof(reset_time), state->primary_resets_at);
    snprintf(reset_text, sizeof(reset_text), "CURRENT %s", reset_time);
    draw_quota_panel(88, "CURRENT", primary, reset_text, alert, HUD_PANEL_BLUE, HUD_DIM_BLUE);

    reset_date_text(reset_time, sizeof(reset_time), state->secondary_resets_at);
    snprintf(reset_text, sizeof(reset_text), "WEEKLY %s", reset_time);
    draw_quota_panel(198, "WEEKLY", weekly, reset_text, alert, HUD_PANEL_GREEN, HUD_DIM_GREEN);
}

void display_core_render_hud(display_core_canvas_t *canvas, const ornament_state_t *state)
{
    ornament_state_t fallback;
    if (!begin_render(canvas)) {
        return;
    }
    if (state == NULL) {
        ornament_state_init(&fallback);
        state = &fallback;
    }

    clear_canvas(HUD_BLACK);
    draw_ring_ticks(state);
    draw_logo_header(state);
    draw_quota_rows(state);
    draw_status_badge(state);
}

void display_core_render_clock(display_core_canvas_t *canvas, const ornament_state_t *state)
{
    ornament_state_t fallback;
    if (!begin_render(canvas)) {
        return;
    }
    if (state == NULL) {
        ornament_state_init(&fallback);
        state = &fallback;
    }

    const char *time_text = state->local_time[0] != '\0' ? state->local_time : "--:--";
    const char *date_text = state->local_date[0] != '\0' ? state->local_date : "-- --";
    char temp_text[16];
    char reset_text[32];

    standby_temperature_text(state, temp_text, sizeof(temp_text));
    standby_reset_text(state, reset_text, sizeof(reset_text));

    draw_standby_background();
    draw_standby_header(time_text, date_text, reset_text);
    draw_standby_weather_row(state, temp_text);
    draw_standby_connectivity(state);
}

void display_core_render_voice_status(
    display_core_canvas_t *canvas,
    const ornament_state_t *state,
    const char *bridge_status,
    const char *voice_status)
{
    ornament_state_t fallback;
    if (!begin_render(canvas)) {
        return;
    }
    if (state == NULL) {
        ornament_state_init(&fallback);
        state = &fallback;
    }
    if (bridge_status == NULL) {
        bridge_status = "BRIDGE --";
    }
    if (voice_status == NULL) {
        voice_status = "VOICE READY";
    }

    char quota_text[32];
    char tasks_text[32];
    char wifi_text[32];
    snprintf(
        quota_text,
        sizeof(quota_text),
        "QUOTA %d%%/%d%%",
        percent_or_zero(state->primary_remaining_percent),
        percent_or_zero(state->secondary_remaining_percent));
    snprintf(tasks_text, sizeof(tasks_text), "TASKS %d", state->active_task_count);
    if (state->wifi_connected) {
        snprintf(wifi_text, sizeof(wifi_text), "WIFI %dDBM", state->wifi_rssi);
    } else {
        snprintf(wifi_text, sizeof(wifi_text), "WIFI OFF");
    }

    clear_canvas(HUD_BLACK);
    draw_ring_ticks(state);
    draw_text_center_fit(sy(44), "VOICE STATUS", ss(3), HUD_WHITE);
    draw_text_center_fit(sy(104), voice_status, ss(2), HUD_GREEN);
    draw_text_center_fit(sy(152), bridge_status, ss(2), HUD_TEAL);
    draw_text_center_fit(sy(200), quota_text, ss(2), ui_accent_color_for(state));
    draw_text_center_fit(sy(248), tasks_text, ss(2), status_color(ornament_state_panel_status(state)));
    draw_text_center_fit(sy(296), wifi_text, ss(2), state->wifi_connected ? HUD_WHITE : HUD_AMBER);
}

void display_core_render_tasks(display_core_canvas_t *canvas, const ornament_state_t *state)
{
    ornament_state_t fallback;
    if (!begin_render(canvas)) {
        return;
    }
    if (state == NULL) {
        ornament_state_init(&fallback);
        state = &fallback;
    }

    char codex_text[40];
    char claude_text[40];
    char total_text[32];
    snprintf(
        codex_text,
        sizeof(codex_text),
        "CODEX %s A%d D%d",
        status_text_from_enum(state->codex_task_status),
        state->codex_active_task_count,
        state->codex_done_seq);
    snprintf(
        claude_text,
        sizeof(claude_text),
        "CLAUDE %s A%d D%d",
        status_text_from_enum(state->claude_task_status),
        state->claude_active_task_count,
        state->claude_done_seq);
    snprintf(total_text, sizeof(total_text), "TOTAL ACTIVE %d", state->active_task_count);

    clear_canvas(HUD_BLACK);
    draw_ring_ticks(state);
    draw_text_center_fit(sy(44), "VOICE TASKS", ss(3), HUD_WHITE);
    draw_text_center_fit(sy(112), codex_text, ss(2), status_color(state->codex_task_status));
    draw_text_center_fit(sy(174), claude_text, ss(2), status_color(state->claude_task_status));
    draw_text_center_fit(sy(244), total_text, ss(2), status_color(ornament_state_panel_status(state)));
    draw_status_badge(state);
}

static bool parse_lrc_timestamp(const char *text, uint32_t *timestamp_ms, const char **line_text)
{
    if (text == NULL || text[0] != '[' || timestamp_ms == NULL || line_text == NULL) {
        return false;
    }

    int minute = 0;
    int second = 0;
    int fraction = 0;
    int fraction_digits = 0;
    const char *cursor = text + 1;
    if (*cursor < '0' || *cursor > '9') {
        return false;
    }
    while (*cursor >= '0' && *cursor <= '9') {
        minute = minute * 10 + (*cursor - '0');
        cursor++;
    }
    if (*cursor != ':') {
        return false;
    }
    cursor++;
    for (int i = 0; i < 2; i++) {
        if (*cursor < '0' || *cursor > '9') {
            return false;
        }
        second = second * 10 + (*cursor - '0');
        cursor++;
    }
    if (*cursor == '.' || *cursor == ':') {
        cursor++;
        while (*cursor >= '0' && *cursor <= '9' && fraction_digits < 3) {
            fraction = fraction * 10 + (*cursor - '0');
            fraction_digits++;
            cursor++;
        }
        while (fraction_digits < 3) {
            fraction *= 10;
            fraction_digits++;
        }
        while (*cursor >= '0' && *cursor <= '9') {
            cursor++;
        }
    }
    if (*cursor != ']') {
        return false;
    }
    cursor++;
    while (*cursor == '[') {
        const char *end = strchr(cursor, ']');
        if (end == NULL) {
            break;
        }
        cursor = end + 1;
    }
    while (*cursor == ' ' || *cursor == '\t') {
        cursor++;
    }

    *timestamp_ms = (uint32_t)((minute * 60 + second) * 1000 + fraction);
    *line_text = cursor;
    return true;
}

static int collect_lrc_lines(
    const char *lyrics,
    uint32_t playback_ms,
    const char **lines,
    uint32_t *timestamps,
    int max_lines)
{
    int count = 0;
    if (lyrics == NULL || max_lines <= 0) {
        return 0;
    }

    const char *cursor = lyrics;
    while (*cursor != '\0' && count < max_lines) {
        const char *line_start = cursor;
        while (*cursor != '\0' && *cursor != '\n' && *cursor != '\r') {
            cursor++;
        }
        char line[160];
        size_t len = (size_t)(cursor - line_start);
        if (len >= sizeof(line)) {
            len = sizeof(line) - 1;
        }
        memcpy(line, line_start, len);
        line[len] = '\0';
        while (*cursor == '\r' || *cursor == '\n') {
            cursor++;
        }

        uint32_t timestamp_ms = 0;
        const char *line_text = NULL;
        if (!parse_lrc_timestamp(line, &timestamp_ms, &line_text) || line_text == NULL || line_text[0] == '\0') {
            continue;
        }
        timestamps[count] = timestamp_ms;
        lines[count] = line_start;
        count++;
    }

    (void)playback_ms;
    return count;
}

static void copy_lyric_line_text(const char *source, char *target, size_t target_size)
{
    if (target == NULL || target_size == 0) {
        return;
    }
    target[0] = '\0';
    if (source == NULL) {
        return;
    }

    char line[160];
    size_t len = 0;
    while (source[len] != '\0' && source[len] != '\r' && source[len] != '\n' && len + 1 < sizeof(line)) {
        line[len] = source[len];
        len++;
    }
    line[len] = '\0';

    uint32_t ignored_ms = 0;
    const char *text = line;
    (void)parse_lrc_timestamp(line, &ignored_ms, &text);
    ascii_preview(text, "--", target, target_size);
}

#if CONFIG_ORNAMENT_RICH_XIAOZHI_DISPLAY_ENABLED
static void copy_lyric_line_utf8(const char *source, char *target, size_t target_size)
{
    if (target == NULL || target_size == 0) {
        return;
    }
    target[0] = '\0';
    if (source == NULL) {
        return;
    }

    char line[160];
    size_t len = 0;
    while (source[len] != '\0' && source[len] != '\r' && source[len] != '\n' && len + 1 < sizeof(line)) {
        line[len] = source[len];
        len++;
    }
    line[len] = '\0';

    uint32_t ignored_ms = 0;
    const char *text = line;
    (void)parse_lrc_timestamp(line, &ignored_ms, &text);
    strlcpy(target, text != NULL && text[0] != '\0' ? text : "--", target_size);
}
#endif

static void draw_cover_placeholder(int x, int y, int size, uint16_t accent)
{
    fill_round_rect(x, y, size, size, ss(10), MUSIC_COVER_BG);
    draw_rect_outline(x, y, size, size, ss(1), HUD_HUD_DIM);
    fill_circle(x + size / 2, y + size / 2, size / 3, MUSIC_COVER_DIM);
    fill_circle(x + size / 2, y + size / 2, size / 9, HUD_HUD_BG);
    draw_circle_outline(x + size / 2, y + size / 2, size / 3, ss(2), accent);
}

static void draw_cover_pixels(int x, int y, int size, const music_player_snapshot_t *snapshot)
{
    if (snapshot == NULL || !snapshot->has_cover || size <= 0) {
        draw_cover_placeholder(x, y, size, HUD_HUD_CYAN);
        return;
    }
#if CONFIG_ORNAMENT_MUSIC_COVER_ENABLED
    if (snapshot->cover_pixels == NULL) {
        draw_cover_placeholder(x, y, size, HUD_HUD_CYAN);
        return;
    }
#endif

    for (int yy = 0; yy < size; yy++) {
        int src_y = yy * MUSIC_PLAYER_COVER_SIZE / size;
        for (int xx = 0; xx < size; xx++) {
            int src_x = xx * MUSIC_PLAYER_COVER_SIZE / size;
#if CONFIG_ORNAMENT_MUSIC_COVER_ENABLED
            draw_pixel(x + xx, y + yy, snapshot->cover_pixels[src_y * MUSIC_PLAYER_COVER_SIZE + src_x]);
#else
            (void)src_y;
            (void)src_x;
            draw_cover_placeholder(x, y, size, HUD_HUD_CYAN);
            return;
#endif
        }
    }
    draw_rect_outline(x, y, size, size, ss(1), HUD_HUD_CYAN);
}

static void draw_music_lyrics(const music_player_snapshot_t *snapshot, int x, int y, int w)
{
#if !CONFIG_ORNAMENT_RICH_XIAOZHI_DISPLAY_ENABLED
    (void)x;
#endif
    char lines_text[3][96];
#if CONFIG_ORNAMENT_RICH_XIAOZHI_DISPLAY_ENABLED
    char lines_utf8[3][160];
#endif
    const char *lrc_lines[96];
    uint32_t lrc_times[96];
    int current = 0;
    int line_count = collect_lrc_lines(snapshot->lyrics, snapshot->playback_ms, lrc_lines, lrc_times, 96);

    memset(lines_text, 0, sizeof(lines_text));
#if CONFIG_ORNAMENT_RICH_XIAOZHI_DISPLAY_ENABLED
    memset(lines_utf8, 0, sizeof(lines_utf8));
#endif
    if (line_count > 0) {
        for (int i = 0; i < line_count; i++) {
            if (lrc_times[i] <= snapshot->playback_ms) {
                current = i;
            } else {
                break;
            }
        }
        int first = current > 0 ? current - 1 : 0;
        for (int i = 0; i < 3; i++) {
            int source_index = first + i;
            if (source_index < line_count) {
                copy_lyric_line_text(lrc_lines[source_index], lines_text[i], sizeof(lines_text[i]));
#if CONFIG_ORNAMENT_RICH_XIAOZHI_DISPLAY_ENABLED
                copy_lyric_line_utf8(lrc_lines[source_index], lines_utf8[i], sizeof(lines_utf8[i]));
#endif
            }
        }
    } else {
        char clean[220];
        ascii_preview(snapshot->lyrics, "NO LYRIC", clean, sizeof(clean));
        int base_scale = ss(1);
        if (base_scale < 1) {
            base_scale = 1;
        }
        int max_chars = w / (6 * base_scale);
        if (max_chars < 8) {
            max_chars = 8;
        }
        int len = (int)strlen(clean);
        int offset = len > max_chars ? (int)((snapshot->playback_ms / 420) % (uint32_t)(len + 4)) : 0;
        for (int i = 0; i < 3; i++) {
            int start = offset + i * max_chars;
            if (start >= len) {
                start -= len;
            }
            for (int j = 0; j < max_chars && j + 1 < (int)sizeof(lines_text[i]); j++) {
                int source_index = start + j;
                if (source_index >= len) {
                    break;
                }
                lines_text[i][j] = clean[source_index];
                lines_text[i][j + 1] = '\0';
            }
        }
    }

#if CONFIG_ORNAMENT_RICH_XIAOZHI_DISPLAY_ENABLED
    if (line_count > 0) {
        for (int i = 0; i < 3; i++) {
            uint16_t color = i == 1 ? HUD_WHITE : HUD_MUTED;
            draw_utf8_text(x, y + i * sy(24), &font_puhui_16_4, lines_utf8[i][0] != '\0' ? lines_utf8[i] : "--", color, w);
        }
        return;
    }
#endif

    for (int i = 0; i < 3; i++) {
        uint16_t color = i == 1 ? HUD_WHITE : HUD_MUTED;
        int scale = ss(1);
        if (scale < 1) {
            scale = 1;
        }
        draw_text_center_fit(y + i * sy(26), lines_text[i][0] != '\0' ? lines_text[i] : "--", scale, color);
    }
}

void display_core_render_music(
    display_core_canvas_t *canvas,
    const ornament_state_t *state,
    const music_player_snapshot_t *snapshot)
{
    if (!begin_render(canvas)) {
        return;
    }
    if (snapshot == NULL) {
        memset(&music_fallback_snapshot, 0, sizeof(music_fallback_snapshot));
        music_fallback_snapshot.state = MUSIC_PLAYER_STATE_IDLE;
        snapshot = &music_fallback_snapshot;
    }

    char title[96];
    char artist[96];
    char status[32];
    const char *raw_title = snapshot->title[0] != '\0' ? snapshot->title : snapshot->song_name;
    const char *raw_artist = snapshot->artist_name[0] != '\0' ? snapshot->artist_name : snapshot->album;
    ascii_preview(raw_title, "MUSIC", title, sizeof(title));
    ascii_preview(raw_artist, music_player_state_name(snapshot->state), artist, sizeof(artist));
    snprintf(
        status,
        sizeof(status),
        "%s %02lu:%02lu",
        music_player_state_name(snapshot->state),
        (unsigned long)(snapshot->playback_ms / 60000),
        (unsigned long)((snapshot->playback_ms / 1000) % 60));

    clear_canvas(HUD_HUD_BG);
    draw_hud_chamfer_box(sx(8), sy(6), active_canvas->width - sx(16), active_canvas->height - sy(12), ss(12), ss(2), HUD_HUD_CYAN);
    draw_text_xy(sx(18), sy(18), "MUSIC", ss(1), ss(1), HUD_WHITE);
    draw_text_right_fit_xy(active_canvas->width - sx(18), sy(18), status, ss(1), ss(1), HUD_HUD_CYAN);

    int cover_size = ss(96);
    if (cover_size < 64) {
        cover_size = 64;
    }
    if (cover_size > 104) {
        cover_size = 104;
    }
    int cover_x = active_canvas->center_x - cover_size / 2;
    int cover_y = sy(40);
    draw_cover_pixels(cover_x, cover_y, cover_size, snapshot);

    draw_text_center_fit(cover_y + cover_size + sy(12), title, ss(1), HUD_WHITE);
    draw_text_center_fit(cover_y + cover_size + sy(32), artist, ss(1), HUD_MUTED);
    draw_music_lyrics(snapshot, sx(24), cover_y + cover_size + sy(58), active_canvas->width - sx(48));
    if (snapshot->stop_requested) {
        draw_text_center_fit(active_canvas->height - sy(24), "STOPPING", ss(1), HUD_AMBER);
    } else if (snapshot->last_error[0] != '\0' && snapshot->state == MUSIC_PLAYER_STATE_ERROR) {
        draw_text_center_fit(active_canvas->height - sy(24), snapshot->last_error, ss(1), HUD_RED);
    } else {
        draw_hud_wave(active_canvas->center_x, active_canvas->height - sy(24), sx(70), sy(18), HUD_HUD_CYAN);
    }

    (void)state;
}

void display_core_render_xiaozhi(
    display_core_canvas_t *canvas,
    const ornament_state_t *state,
    const xiaozhi_client_snapshot_t *snapshot)
{
    xiaozhi_client_snapshot_t snapshot_fallback;
    if (!begin_render(canvas)) {
        return;
    }
    if (snapshot == NULL) {
        memset(&snapshot_fallback, 0, sizeof(snapshot_fallback));
        snapshot_fallback.state = XIAOZHI_CLIENT_STATE_DISABLED;
        snapshot = &snapshot_fallback;
    }

    char stt_preview[96];
    char assistant_preview[96];
    const char *stt_text = xiaozhi_hud_user_hint(snapshot);
    const char *tts_text = snapshot->last_tts[0] != '\0' ? snapshot->last_tts :
        (snapshot->activation_pending ?
            (snapshot->activation_code[0] != '\0' ? snapshot->activation_code : "BIND DEVICE") :
            "");
    const bool completed = snapshot->state == XIAOZHI_CLIENT_STATE_IDLE && snapshot->last_tts[0] != '\0';
    const char *main_text = completed ? xiaozhi_hud_done_text() : xiaozhi_hud_main_text(snapshot->state);
    const char *main_ascii = completed ? "DONE" : xiaozhi_hud_main_ascii(snapshot->state);
    const char *assistant_text = xiaozhi_hud_assistant_hint(snapshot, tts_text);
    const char *assistant_ascii = xiaozhi_hud_assistant_ascii(snapshot, tts_text);
    bool stt_ascii_available = ascii_preview(stt_text, "", stt_preview, sizeof(stt_preview));
    bool assistant_ascii_available = ascii_preview(assistant_ascii, "", assistant_preview, sizeof(assistant_preview));
    if (!stt_ascii_available) {
        stt_preview[0] = '\0';
    }
    if (!assistant_ascii_available) {
        assistant_preview[0] = '\0';
    }

    uint16_t state_color = HUD_HUD_CYAN;
    switch (snapshot->state) {
    case XIAOZHI_CLIENT_STATE_ERROR:
    case XIAOZHI_CLIENT_STATE_CONFIG_MISSING:
        state_color = HUD_RED;
        break;
    case XIAOZHI_CLIENT_STATE_CONNECTING:
        state_color = HUD_AMBER;
        break;
    case XIAOZHI_CLIENT_STATE_LISTENING:
    case XIAOZHI_CLIENT_STATE_SPEAKING:
    case XIAOZHI_CLIENT_STATE_IDLE:
    case XIAOZHI_CLIENT_STATE_DISABLED:
    default:
        state_color = HUD_HUD_CYAN;
        break;
    }

    const int line = ss(2) > 0 ? ss(2) : 1;
    const int thin = 1;
    const int text_scale = ss(1) > 0 ? ss(1) : 1;
    const int margin = sx(7);
    const int outer_y = sy(6);
    const int outer_h = active_canvas->height - sy(12);
    const int time_y = sy(16);
    const int top_sep_y = sy(43);
    const int query_x = sx(18);
    const int query_y = sy(57);
    const int query_w = active_canvas->width - sx(36);
    const int query_h = sy(49);
    const int icon_size = ss(30);
    const int ring_cx = active_canvas->center_x;
    const int ring_cy = sy(172);
    const int ring_r = ss(68);
    const int assistant_x = sx(18);
    const int assistant_y = sy(273);
    const int assistant_w = active_canvas->width - sx(36);
    const int assistant_h = active_canvas->height - assistant_y - sy(18);
    const char *time_text = state != NULL && state->local_time[0] != '\0' ? state->local_time : "--:--";

    clear_canvas(HUD_HUD_BG);
    draw_hud_chamfer_box(margin, outer_y, active_canvas->width - margin * 2, outer_h, ss(12), line, HUD_HUD_CYAN);
    draw_hline(margin + sx(2), active_canvas->width - margin - sx(2), top_sep_y, thin, HUD_HUD_DIM);
    fill_circle(active_canvas->center_x, time_y + ss(3), ss(3), HUD_HUD_CYAN);
    draw_text_xy(sx(22), time_y, time_text, text_scale, text_scale, HUD_HUD_CYAN);

    draw_hud_chamfer_box(query_x, query_y, query_w, query_h, ss(8), thin, HUD_HUD_DIM);
    draw_hud_user_icon(query_x + ss(8), query_y + ss(9), icon_size, HUD_HUD_CYAN);
    draw_hud_wrapped_text(
        query_x + icon_size + ss(18),
        query_y + ss(16),
        stt_text,
        stt_preview,
        HUD_HUD_CYAN,
        query_w - icon_size - ss(50),
        2,
        text_scale,
        text_scale);
    if (stt_text[0] != '\0') {
        draw_text_xy(query_x + query_w - ss(30), query_y + query_h - ss(18), "...", text_scale, text_scale, HUD_HUD_CYAN);
    }

    draw_hud_segmented_ring(ring_cx, ring_cy, ring_r, state_color);
    if (snapshot->state == XIAOZHI_CLIENT_STATE_LISTENING) {
        draw_hud_wave(ring_cx - ring_r - ss(28), ring_cy + ss(3), ss(42), ss(35), state_color);
        draw_hud_wave(ring_cx + ring_r + ss(28), ring_cy + ss(3), ss(42), ss(35), state_color);
    }
    draw_hud_center_text(ring_cy - ss(18), main_text, main_ascii, state_color, ss(2));
    if (snapshot->state == XIAOZHI_CLIENT_STATE_LISTENING) {
        draw_hud_microphone(ring_cx, ring_cy + ss(24), ss(32), HUD_WHITE);
    } else if (snapshot->state == XIAOZHI_CLIENT_STATE_SPEAKING) {
        draw_hud_speaker(ring_cx, ring_cy + ss(24), ss(32), HUD_WHITE);
        draw_hud_wave(ring_cx, ring_cy + ss(51), ss(48), ss(18), state_color);
    } else if (completed || snapshot->connected) {
        draw_hud_check(ring_cx, ring_cy + ss(24), ss(32), HUD_WHITE);
    } else {
        draw_hud_microphone(ring_cx, ring_cy + ss(24), ss(32), HUD_WHITE);
    }

    draw_hud_chamfer_box(assistant_x, assistant_y, assistant_w, assistant_h, ss(8), thin, HUD_HUD_DIM);
    draw_hud_panel_marks(assistant_x, assistant_y, assistant_w, assistant_h, state_color);
    draw_hud_bot_icon(assistant_x + ss(8), assistant_y + ss(12), icon_size, state_color);
    draw_hud_wrapped_text(
        assistant_x + icon_size + ss(18),
        assistant_y + ss(18),
        assistant_text,
        assistant_preview,
        HUD_HUD_CYAN,
        assistant_w - icon_size - ss(34),
        2,
        text_scale,
        text_scale);
}

static void render_message(display_core_canvas_t *canvas, const char *line1, const char *line2, uint16_t color)
{
    ornament_state_t state;
    if (!begin_render(canvas)) {
        return;
    }
    if (line1 == NULL) {
        line1 = "";
    }

    ornament_state_init(&state);
    clear_canvas(HUD_BLACK);
    draw_ring_ticks(&state);
    draw_text_center_fit(active_canvas->center_y - sy(36), line1, ts(4), color);
    if (line2 != NULL) {
        draw_text_center_fit(active_canvas->center_y + sy(16), line2, ts(2), HUD_WHITE);
    }
}

void display_core_render_boot(display_core_canvas_t *canvas)
{
    render_message(canvas, "CODEX", "BOOTING", HUD_CYAN);
}

void display_core_render_status_message(display_core_canvas_t *canvas, const char *message)
{
    render_message(canvas, "CODEX", message, HUD_GREEN);
}

void display_core_render_error_message(display_core_canvas_t *canvas, const char *message)
{
    render_message(canvas, "ERROR", message, HUD_RED);
}
