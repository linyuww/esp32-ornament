#include "display.h"

#include "display_core.h"
#include "st77916_truly_init.h"

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_lcd_io_spi.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_st77916.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "display";
static const int FLUSH_LINES_CANDIDATES[] = {40, 24, 16, 8, 4, 1};

static esp_lcd_panel_handle_t panel_handle;
static display_core_canvas_t canvas;
static uint16_t *flush_buffers[2];
static int flush_lines;

static void display_log_config(void)
{
    ESP_LOGI(
        TAG,
        "lcd config driver=%s host=%d res=%dx%d pclk=%d",
        CONFIG_ORNAMENT_DISPLAY_DRIVER,
        CONFIG_ORNAMENT_LCD_SPI_HOST,
        CONFIG_ORNAMENT_LCD_H_RES,
        CONFIG_ORNAMENT_LCD_V_RES,
        CONFIG_ORNAMENT_LCD_PIXEL_CLOCK_HZ);
    ESP_LOGI(
        TAG,
        "lcd pins sclk=%d cs=%d rst=%d bl=%d bl_active=%d io0=%d io1=%d io2=%d io3=%d dc=%d",
        CONFIG_ORNAMENT_LCD_PIN_SCLK,
        CONFIG_ORNAMENT_LCD_PIN_CS,
        CONFIG_ORNAMENT_LCD_PIN_RST,
        CONFIG_ORNAMENT_LCD_PIN_BL,
        CONFIG_ORNAMENT_LCD_BL_ACTIVE_LEVEL,
        CONFIG_ORNAMENT_LCD_PIN_MOSI,
        CONFIG_ORNAMENT_LCD_PIN_MISO,
        CONFIG_ORNAMENT_LCD_PIN_D2,
        CONFIG_ORNAMENT_LCD_PIN_D3,
        CONFIG_ORNAMENT_LCD_PIN_DC);
}

static esp_err_t display_init_spi(void)
{
    const spi_bus_config_t bus_config = ST77916_PANEL_BUS_SPI_CONFIG(
        CONFIG_ORNAMENT_LCD_PIN_SCLK,
        CONFIG_ORNAMENT_LCD_PIN_MOSI,
        CONFIG_ORNAMENT_LCD_H_RES * 80 * sizeof(uint16_t));
    ESP_RETURN_ON_ERROR(
        spi_bus_initialize(CONFIG_ORNAMENT_LCD_SPI_HOST, &bus_config, SPI_DMA_CH_AUTO),
        TAG,
        "spi_bus_initialize failed");

    esp_lcd_panel_io_handle_t io_handle = NULL;
    esp_lcd_panel_io_spi_config_t io_config = ST77916_PANEL_IO_SPI_CONFIG(
        CONFIG_ORNAMENT_LCD_PIN_CS,
        CONFIG_ORNAMENT_LCD_PIN_DC,
        NULL,
        NULL);
    io_config.pclk_hz = CONFIG_ORNAMENT_LCD_PIXEL_CLOCK_HZ;
    ESP_RETURN_ON_ERROR(
        esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)CONFIG_ORNAMENT_LCD_SPI_HOST, &io_config, &io_handle),
        TAG,
        "esp_lcd_new_panel_io_spi failed");

    st77916_vendor_config_t vendor_config = {
        .flags = {
            .use_qspi_interface = 0,
        },
    };
    const esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = CONFIG_ORNAMENT_LCD_PIN_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
        .vendor_config = &vendor_config,
    };
    return esp_lcd_new_panel_st77916(io_handle, &panel_config, &panel_handle);
}

static esp_err_t display_init_qspi(void)
{
    const spi_bus_config_t bus_config = ST77916_PANEL_BUS_QSPI_CONFIG(
        CONFIG_ORNAMENT_LCD_PIN_SCLK,
        CONFIG_ORNAMENT_LCD_PIN_MOSI,
        CONFIG_ORNAMENT_LCD_PIN_MISO,
        CONFIG_ORNAMENT_LCD_PIN_D2,
        CONFIG_ORNAMENT_LCD_PIN_D3,
        CONFIG_ORNAMENT_LCD_H_RES * 80 * sizeof(uint16_t));
    ESP_RETURN_ON_ERROR(
        spi_bus_initialize(CONFIG_ORNAMENT_LCD_SPI_HOST, &bus_config, SPI_DMA_CH_AUTO),
        TAG,
        "qspi bus init failed");

    esp_lcd_panel_io_handle_t io_handle = NULL;
    esp_lcd_panel_io_spi_config_t io_config = ST77916_PANEL_IO_QSPI_CONFIG(
        CONFIG_ORNAMENT_LCD_PIN_CS,
        NULL,
        NULL);
    io_config.pclk_hz = CONFIG_ORNAMENT_LCD_PIXEL_CLOCK_HZ;
    ESP_RETURN_ON_ERROR(
        esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)CONFIG_ORNAMENT_LCD_SPI_HOST, &io_config, &io_handle),
        TAG,
        "esp_lcd_new_panel_io_spi qspi failed");

    st77916_vendor_config_t vendor_config = {
        .init_cmds = st77916_truly_init_cmds,
        .init_cmds_size = sizeof(st77916_truly_init_cmds) / sizeof(st77916_truly_init_cmds[0]),
        .flags = {
            .use_qspi_interface = 1,
        },
    };
    const esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = CONFIG_ORNAMENT_LCD_PIN_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
        .vendor_config = &vendor_config,
    };
    return esp_lcd_new_panel_st77916(io_handle, &panel_config, &panel_handle);
}

static void display_set_backlight(bool enabled)
{
    if (CONFIG_ORNAMENT_LCD_PIN_BL < 0) {
        ESP_LOGW(TAG, "backlight GPIO disabled");
        return;
    }

    gpio_config_t config = {
        .pin_bit_mask = 1ULL << CONFIG_ORNAMENT_LCD_PIN_BL,
        .mode = GPIO_MODE_OUTPUT,
    };
    esp_err_t err = gpio_config(&config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "backlight GPIO config failed: %s", esp_err_to_name(err));
        return;
    }

    int active_level = CONFIG_ORNAMENT_LCD_BL_ACTIVE_LEVEL ? 1 : 0;
    int level = enabled ? active_level : !active_level;
    err = gpio_set_level(CONFIG_ORNAMENT_LCD_PIN_BL, level);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "backlight GPIO set failed: %s", esp_err_to_name(err));
        return;
    }
    ESP_LOGI(TAG, "backlight %s level=%d", enabled ? "on" : "off", level);
}

static bool canvas_alloc(void)
{
    if (canvas.pixels != NULL) {
        return true;
    }

    const uint16_t width = CONFIG_ORNAMENT_LCD_H_RES;
    const uint16_t height = CONFIG_ORNAMENT_LCD_V_RES;
    uint16_t *pixels = heap_caps_malloc((size_t)width * height * sizeof(uint16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (pixels == NULL) {
        pixels = heap_caps_malloc((size_t)width * height * sizeof(uint16_t), MALLOC_CAP_8BIT);
    }
    if (pixels == NULL) {
        return false;
    }

    display_core_canvas_init(&canvas, width, height, pixels);
    return true;
}

static bool flush_buffers_alloc(void)
{
    if (flush_buffers[0] != NULL && flush_buffers[1] != NULL && flush_lines > 0) {
        return true;
    }

    const size_t candidate_count = sizeof(FLUSH_LINES_CANDIDATES) / sizeof(FLUSH_LINES_CANDIDATES[0]);
    for (size_t i = 0; i < candidate_count; i++) {
        int lines = FLUSH_LINES_CANDIDATES[i];
        size_t bytes = (size_t)CONFIG_ORNAMENT_LCD_H_RES * (size_t)lines * sizeof(uint16_t);
        uint16_t *first = heap_caps_malloc(bytes, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (first == NULL) {
            continue;
        }

        uint16_t *second = heap_caps_malloc(bytes, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (second == NULL) {
            heap_caps_free(first);
            continue;
        }

        flush_buffers[0] = first;
        flush_buffers[1] = second;
        flush_lines = lines;
        ESP_LOGI(TAG, "allocated %d-line DMA flush buffers (%u bytes each)", lines, (unsigned int)bytes);
        return true;
    }

    ESP_LOGE(TAG, "failed to allocate DMA flush buffers");
    return false;
}

static esp_err_t flush_canvas(void)
{
    if (panel_handle == NULL || canvas.pixels == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!flush_buffers_alloc()) {
        return ESP_ERR_NO_MEM;
    }

    for (int y = 0, buffer_index = 0; y < canvas.height; y += flush_lines, buffer_index ^= 1) {
        int lines = flush_lines;
        if (y + lines > canvas.height) {
            lines = canvas.height - y;
        }

        size_t pixel_count = (size_t)canvas.width * (size_t)lines;
        size_t bytes = pixel_count * sizeof(uint16_t);
        const uint16_t *src = &canvas.pixels[(size_t)y * canvas.width];
        if (CONFIG_ORNAMENT_LCD_SWAP_COLOR_BYTES) {
            for (size_t i = 0; i < pixel_count; i++) {
                uint16_t pixel = src[i];
                flush_buffers[buffer_index][i] = (uint16_t)((pixel << 8) | (pixel >> 8));
            }
        } else {
            memcpy(flush_buffers[buffer_index], src, bytes);
        }

        esp_err_t err = esp_lcd_panel_draw_bitmap(
            panel_handle,
            0,
            y,
            canvas.width,
            y + lines,
            flush_buffers[buffer_index]);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "draw bitmap failed at y=%d lines=%d: %s", y, lines, esp_err_to_name(err));
            return err;
        }
    }

    return ESP_OK;
}

static void fill_canvas_color(uint16_t color)
{
    if (!canvas_alloc()) {
        ESP_LOGE(TAG, "failed to allocate display canvas");
        return;
    }
    for (int y = 0; y < canvas.height; y++) {
        for (int x = 0; x < canvas.width; x++) {
            canvas.pixels[y * canvas.width + x] = color;
        }
    }
    (void)flush_canvas();
}

static void fill_canvas_rect(int x, int y, int width, int height, uint16_t color)
{
    if (!canvas_alloc()) {
        ESP_LOGE(TAG, "failed to allocate display canvas");
        return;
    }

    int x_end = x + width;
    int y_end = y + height;
    if (x < 0) {
        x = 0;
    }
    if (y < 0) {
        y = 0;
    }
    if (x_end > canvas.width) {
        x_end = canvas.width;
    }
    if (y_end > canvas.height) {
        y_end = canvas.height;
    }

    for (int yy = y; yy < y_end; yy++) {
        for (int xx = x; xx < x_end; xx++) {
            canvas.pixels[(size_t)yy * canvas.width + xx] = color;
        }
    }
}

static const uint8_t *test_glyph_for(char ch)
{
    static const uint8_t blank[7] = {0, 0, 0, 0, 0, 0, 0};
    static const uint8_t glyphs[][7] = {
        ['0'] = {0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E},
        ['1'] = {0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E},
        ['2'] = {0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F},
        ['3'] = {0x1E, 0x01, 0x01, 0x0E, 0x01, 0x01, 0x1E},
        ['4'] = {0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02},
        ['A'] = {0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11},
        ['B'] = {0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E},
        ['C'] = {0x0E, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0E},
        ['D'] = {0x1E, 0x12, 0x11, 0x11, 0x11, 0x12, 0x1E},
        ['E'] = {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F},
        ['F'] = {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10},
        ['G'] = {0x0E, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0F},
        ['H'] = {0x11, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11},
        ['I'] = {0x0E, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0E},
        ['L'] = {0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F},
        ['M'] = {0x11, 0x1B, 0x15, 0x15, 0x11, 0x11, 0x11},
        ['N'] = {0x11, 0x19, 0x19, 0x15, 0x13, 0x13, 0x11},
        ['O'] = {0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E},
        ['P'] = {0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10},
        ['R'] = {0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11},
        ['S'] = {0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E},
        ['T'] = {0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04},
        ['W'] = {0x11, 0x11, 0x11, 0x15, 0x15, 0x15, 0x0A},
        ['X'] = {0x11, 0x11, 0x0A, 0x04, 0x0A, 0x11, 0x11},
        ['Y'] = {0x11, 0x11, 0x0A, 0x04, 0x04, 0x04, 0x04},
        [' '] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
        ['-'] = {0x00, 0x00, 0x00, 0x1F, 0x00, 0x00, 0x00},
    };
    unsigned char index = (unsigned char)ch;
    if (index >= sizeof(glyphs) / sizeof(glyphs[0])) {
        return blank;
    }
    return glyphs[index];
}

static void draw_test_text(int x, int y, const char *text, int scale, uint16_t color)
{
    int cursor = x;
    for (const char *ch = text; *ch != '\0'; ch++) {
        char glyph_ch = *ch;
        if (glyph_ch >= 'a' && glyph_ch <= 'z') {
            glyph_ch = (char)(glyph_ch - 'a' + 'A');
        }
        const uint8_t *glyph = test_glyph_for(glyph_ch);
        for (int row = 0; row < 7; row++) {
            for (int col = 0; col < 5; col++) {
                if ((glyph[row] >> (4 - col)) & 1) {
                    fill_canvas_rect(cursor + col * scale, y + row * scale, scale, scale, color);
                }
            }
        }
        cursor += 6 * scale;
    }
}

static void draw_test_text_center(int y, const char *text, int scale, uint16_t color)
{
    int width = (int)strlen(text) * 6 * scale;
    draw_test_text(((int)canvas.width - width) / 2, y, text, scale, color);
}

static void render_lcd_reference_chart(void)
{
    if (!canvas_alloc()) {
        ESP_LOGE(TAG, "failed to allocate display canvas");
        return;
    }

    const uint16_t black = display_core_rgb565(0, 0, 0);
    const uint16_t white = display_core_rgb565(255, 255, 255);
    const uint16_t red = display_core_rgb565(255, 0, 0);
    const uint16_t green = display_core_rgb565(0, 255, 0);
    const uint16_t blue = display_core_rgb565(0, 0, 255);
    const uint16_t yellow = display_core_rgb565(255, 255, 0);
    const uint16_t cyan = display_core_rgb565(0, 255, 255);
    const uint16_t magenta = display_core_rgb565(255, 0, 255);
    const uint16_t orange = display_core_rgb565(255, 128, 0);
    const uint16_t gray = display_core_rgb565(96, 96, 96);
    const uint16_t dark = display_core_rgb565(12, 16, 22);
    const uint16_t bars[] = {white, yellow, cyan, green, magenta, red, blue, black};

    fill_canvas_color(dark);
    fill_canvas_rect(0, 0, 360, 8, yellow);
    fill_canvas_rect(0, 352, 360, 8, yellow);
    fill_canvas_rect(0, 0, 8, 360, yellow);
    fill_canvas_rect(352, 0, 8, 360, yellow);
    fill_canvas_rect(8, 8, 32, 32, red);
    fill_canvas_rect(320, 8, 32, 32, green);
    fill_canvas_rect(8, 320, 32, 32, blue);
    fill_canvas_rect(320, 320, 32, 32, white);

    for (int i = 0; i < 8; i++) {
        fill_canvas_rect(40 + i * 35, 52, 35, 64, bars[i]);
    }
    fill_canvas_rect(40, 116, 280, 4, white);

    for (int i = 0; i < 16; i++) {
        uint8_t level = (uint8_t)(i * 255 / 15);
        fill_canvas_rect(40 + i * 17, 128, 17, 28, display_core_rgb565(level, level, level));
    }

    for (int x = 40; x < 320; x += 10) {
        fill_canvas_rect(x, 168, 1, 44, white);
    }
    for (int x = 45; x < 320; x += 10) {
        fill_canvas_rect(x, 168, 1, 44, gray);
    }
    for (int y = 168; y < 212; y += 10) {
        fill_canvas_rect(40, y, 280, 1, white);
    }

    fill_canvas_rect(48, 224, 64, 52, red);
    fill_canvas_rect(112, 224, 64, 52, green);
    fill_canvas_rect(176, 224, 64, 52, blue);
    fill_canvas_rect(240, 224, 64, 52, magenta);
    fill_canvas_rect(58, 238, 44, 24, black);
    fill_canvas_rect(122, 238, 44, 24, black);
    fill_canvas_rect(186, 238, 44, 24, black);
    fill_canvas_rect(250, 238, 44, 24, black);

    draw_test_text_center(18, "TOP", 3, black);
    draw_test_text(16, 158, "LEFT", 2, orange);
    draw_test_text(280, 158, "RIGHT", 2, orange);
    draw_test_text_center(328, "BOTTOM", 2, black);
    draw_test_text_center(286, "ST77916 QSPI", 2, white);
    draw_test_text_center(306, "IO0 IO1 IO2 IO3", 1, white);
    draw_test_text(62, 244, "R", 2, white);
    draw_test_text(126, 244, "G", 2, white);
    draw_test_text(190, 244, "B", 2, white);
    draw_test_text(254, 244, "M", 2, white);

    (void)flush_canvas();
}

static void render_lcd_diagnostic_pattern(void)
{
    if (!canvas_alloc()) {
        ESP_LOGE(TAG, "failed to allocate display canvas");
        return;
    }

    const uint16_t black = display_core_rgb565(0, 0, 0);
    const uint16_t white = display_core_rgb565(255, 255, 255);
    const uint16_t red = display_core_rgb565(255, 0, 0);
    const uint16_t green = display_core_rgb565(0, 255, 0);
    const uint16_t blue = display_core_rgb565(0, 0, 255);
    const uint16_t yellow = display_core_rgb565(255, 255, 0);
    const uint16_t cyan = display_core_rgb565(0, 255, 255);
    const uint16_t magenta = display_core_rgb565(255, 0, 255);
    const int w = canvas.width;
    const int h = canvas.height;

    fill_canvas_color(black);
    fill_canvas_rect(0, 0, w / 2, h / 2, red);
    fill_canvas_rect(w / 2, 0, w - w / 2, h / 2, green);
    fill_canvas_rect(0, h / 2, w / 2, h - h / 2, blue);
    fill_canvas_rect(w / 2, h / 2, w - w / 2, h - h / 2, white);

    fill_canvas_rect(w / 2 - 4, 0, 8, h, black);
    fill_canvas_rect(0, h / 2 - 4, w, 8, black);
    fill_canvas_rect(0, 0, w, 8, yellow);
    fill_canvas_rect(0, h - 8, w, 8, yellow);
    fill_canvas_rect(0, 0, 8, h, yellow);
    fill_canvas_rect(w - 8, 0, 8, h, yellow);

    fill_canvas_rect(60, 112, 240, 28, black);
    fill_canvas_rect(60, 160, 240, 28, black);
    fill_canvas_rect(60, 208, 240, 28, black);
    fill_canvas_rect(72, 118, 216, 16, cyan);
    fill_canvas_rect(72, 166, 216, 16, magenta);
    fill_canvas_rect(72, 214, 216, 16, white);

    (void)flush_canvas();
}

static void render_boot_test_pattern(void)
{
#if CONFIG_ORNAMENT_LCD_REFERENCE_TEST_ONLY
    {
        ESP_LOGI(TAG, "showing LCD black/white flash probe");
        for (int i = 0; i < 6; i++) {
            fill_canvas_color(display_core_rgb565(255, 255, 255));
            vTaskDelay(pdMS_TO_TICKS(300));
            fill_canvas_color(display_core_rgb565(0, 0, 0));
            vTaskDelay(pdMS_TO_TICKS(300));
        }

        ESP_LOGI(TAG, "showing fixed LCD reference chart");
        render_lcd_reference_chart();
        while (true) {
            vTaskDelay(pdMS_TO_TICKS(60000));
        }
    }
#endif

    if (CONFIG_ORNAMENT_LCD_BOOT_TEST_MS <= 0) {
        return;
    }

    const uint16_t colors[] = {
        display_core_rgb565(255, 0, 0),
        display_core_rgb565(0, 255, 0),
        display_core_rgb565(0, 0, 255),
        display_core_rgb565(255, 255, 255),
        display_core_rgb565(0, 0, 0),
        display_core_rgb565(255, 255, 0),
        display_core_rgb565(0, 255, 255),
        display_core_rgb565(255, 0, 255),
    };
    const int color_count = sizeof(colors) / sizeof(colors[0]);
    TickType_t color_delay_ticks = pdMS_TO_TICKS(CONFIG_ORNAMENT_LCD_BOOT_TEST_MS / color_count);
    if (color_delay_ticks == 0) {
        color_delay_ticks = 1;
    }

    ESP_LOGI(TAG, "showing LCD full-screen color test for %d ms", CONFIG_ORNAMENT_LCD_BOOT_TEST_MS);
    for (int i = 0; i < color_count; i++) {
        ESP_LOGI(TAG, "LCD color test step %d/%d", i + 1, color_count);
        fill_canvas_color(colors[i]);
        vTaskDelay(color_delay_ticks);
    }
    ESP_LOGI(TAG, "showing LCD diagnostic pattern");
    render_lcd_diagnostic_pattern();
    vTaskDelay(pdMS_TO_TICKS(8000));
    fill_canvas_color(display_core_rgb565(0, 0, 0));
}

static void render_hud(const ornament_state_t *state)
{
    if (!canvas_alloc()) {
        ESP_LOGE(TAG, "failed to allocate display canvas");
        return;
    }
    display_core_render_hud(&canvas, state);
    (void)flush_canvas();
}

static void render_clock(const ornament_state_t *state)
{
    if (!canvas_alloc()) {
        ESP_LOGE(TAG, "failed to allocate display canvas");
        return;
    }
    display_core_render_clock(&canvas, state);
    (void)flush_canvas();
}

static void render_boot_message(void)
{
    if (!canvas_alloc()) {
        ESP_LOGE(TAG, "failed to allocate display canvas");
        return;
    }
    display_core_render_boot(&canvas);
    (void)flush_canvas();
}

static void render_status_message(const char *message)
{
    if (!canvas_alloc()) {
        ESP_LOGE(TAG, "failed to allocate display canvas");
        return;
    }
    display_core_render_status_message(&canvas, message);
    (void)flush_canvas();
}

static void render_error_message(const char *message)
{
    if (!canvas_alloc()) {
        ESP_LOGE(TAG, "failed to allocate display canvas");
        return;
    }
    display_core_render_error_message(&canvas, message);
    (void)flush_canvas();
}

void display_init(void)
{
    ESP_LOGI(TAG, "initializing ST77916 display");
    display_log_config();
    display_set_backlight(true);

    esp_err_t ret = strcmp(CONFIG_ORNAMENT_DISPLAY_DRIVER, "st77916_qspi") == 0
                        ? display_init_qspi()
                        : display_init_spi();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "display init failed: %s", esp_err_to_name(ret));
        return;
    }

    ESP_GOTO_ON_ERROR(esp_lcd_panel_reset(panel_handle), fail, TAG, "panel reset failed");
    ESP_GOTO_ON_ERROR(esp_lcd_panel_init(panel_handle), fail, TAG, "panel init failed");
    ESP_GOTO_ON_ERROR(esp_lcd_panel_disp_on_off(panel_handle, true), fail, TAG, "panel display on failed");
    ESP_GOTO_ON_ERROR(esp_lcd_panel_swap_xy(panel_handle, CONFIG_ORNAMENT_LCD_SWAP_XY), fail, TAG, "panel swap xy config failed");
    ESP_GOTO_ON_ERROR(esp_lcd_panel_mirror(panel_handle, false, false), fail, TAG, "panel mirror config failed");
    display_set_backlight(true);
    canvas_alloc();
    render_boot_test_pattern();
    ESP_LOGI(TAG, "display init complete");
    return;

fail:
    display_set_backlight(false);
    ESP_LOGE(TAG, "display init failed after panel create: %s", esp_err_to_name(ret));
}

void display_render_boot(void)
{
    render_boot_message();
}

void display_render_status(const char *message)
{
    ESP_LOGI(TAG, "status: %s", message);
    render_status_message(message);
}

void display_render_error(const char *message)
{
    ESP_LOGW(TAG, "error: %s", message);
    render_error_message(message);
}

void display_render_state(const ornament_state_t *state)
{
    ESP_LOGD(
        TAG,
        "state status=%d active_tasks=%d primary=%d secondary=%d title=%s message=%s",
        state->status,
        state->active_task_count,
        state->primary_remaining_percent,
        state->secondary_remaining_percent,
        state->task_title,
        state->task_message);
    render_hud(state);
}

void display_render_clock(const ornament_state_t *state)
{
    ESP_LOGD(
        TAG,
        "clock time=%s date=%s wifi=%s rssi=%d",
        state->local_time,
        state->local_date,
        state->wifi_ssid,
        state->wifi_rssi);
    render_clock(state);
}
