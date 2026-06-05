#include "display_core.h"
#include "standby_wallpaper.h"

#include "lvgl.h"
#include "misc/lv_text_private.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

LV_FONT_DECLARE(font_puhui_14_1);

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

static display_core_canvas_t *active_canvas;

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
        const uint8_t *bitmap = (const uint8_t *)lv_font_get_glyph_bitmap(&glyph, NULL);
        if (bitmap != NULL) {
            int glyph_x = cursor + glyph.ofs_x;
            int glyph_y = y + font->line_height - font->base_line - glyph.box_h - glyph.ofs_y;
            uint32_t bitmap_index = 0;
            for (int row = 0; row < glyph.box_h; row++) {
                for (int col = 0; col < glyph.box_w; col++, bitmap_index++) {
                    uint8_t alpha = lv_alpha_from_bitmap(bitmap, bitmap_index, glyph.format);
                    blend_pixel(glyph_x + col, glyph_y + row, color, alpha);
                }
            }
        }
        lv_font_glyph_release_draw_data(&glyph);
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

static int min_int(int a, int b)
{
    return a < b ? a : b;
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
    if (active_canvas->width != STANDBY_WALLPAPER_WIDTH || active_canvas->height != STANDBY_WALLPAPER_HEIGHT) {
        clear_canvas(HUD_BLACK);
        return;
    }
    memcpy(
        active_canvas->pixels,
        standby_wallpaper_rgb565,
        (size_t)STANDBY_WALLPAPER_WIDTH * STANDBY_WALLPAPER_HEIGHT * sizeof(uint16_t));
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

void display_core_render_xiaozhi(
    display_core_canvas_t *canvas,
    const ornament_state_t *state,
    const xiaozhi_client_snapshot_t *snapshot)
{
    ornament_state_t state_fallback;
    xiaozhi_client_snapshot_t snapshot_fallback;
    if (!begin_render(canvas)) {
        return;
    }
    if (state == NULL) {
        ornament_state_init(&state_fallback);
        state = &state_fallback;
    }
    if (snapshot == NULL) {
        memset(&snapshot_fallback, 0, sizeof(snapshot_fallback));
        snapshot_fallback.state = XIAOZHI_CLIENT_STATE_DISABLED;
        snapshot = &snapshot_fallback;
    }

    char status_text[32];
    char frames_text[40];
    char config_text[40];
    const char *stt_text = snapshot->last_stt[0] != '\0' ? snapshot->last_stt : "NO STT";
    const char *tts_text = snapshot->last_tts[0] != '\0' ? snapshot->last_tts : "NO TTS";
    snprintf(status_text, sizeof(status_text), "AI %s", xiaozhi_client_state_name(snapshot->state));
    snprintf(frames_text, sizeof(frames_text), "UP %lu DOWN %lu", (unsigned long)snapshot->uplink_frames, (unsigned long)snapshot->downlink_frames);
    snprintf(config_text, sizeof(config_text), "%s %s", snapshot->configured ? "CONFIG OK" : "NO CONFIG", snapshot->connected ? "ONLINE" : "OFFLINE");

    uint16_t state_color = HUD_MUTED;
    switch (snapshot->state) {
    case XIAOZHI_CLIENT_STATE_LISTENING:
        state_color = HUD_GREEN;
        break;
    case XIAOZHI_CLIENT_STATE_SPEAKING:
        state_color = HUD_CYAN;
        break;
    case XIAOZHI_CLIENT_STATE_CONNECTING:
        state_color = HUD_AMBER;
        break;
    case XIAOZHI_CLIENT_STATE_ERROR:
    case XIAOZHI_CLIENT_STATE_CONFIG_MISSING:
        state_color = HUD_RED;
        break;
    case XIAOZHI_CLIENT_STATE_IDLE:
    case XIAOZHI_CLIENT_STATE_DISABLED:
    default:
        state_color = HUD_MUTED;
        break;
    }

    clear_canvas(HUD_BLACK);
    draw_ring_ticks(state);
    draw_text_center_fit(sy(34), "XIAOZHI AI", ss(3), HUD_WHITE);
    draw_text_center_fit(sy(78), status_text, ss(2), state_color);
    draw_text_center_fit(sy(110), "YOU", ss(1), HUD_MUTED);
    draw_utf8_text_wrapped(sx(32), sy(128), &font_puhui_14_1, stt_text, HUD_GREEN, sx(296), 2);
    draw_text_center_fit(sy(188), "AI", ss(1), HUD_MUTED);
    draw_utf8_text_wrapped(sx(32), sy(204), &font_puhui_14_1, tts_text, HUD_CYAN, sx(296), 2);
    draw_text_center_fit(sy(268), frames_text, ss(1), HUD_TEAL);
    draw_text_center_fit(sy(298), config_text, ss(1), snapshot->configured ? HUD_WHITE : HUD_AMBER);
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
