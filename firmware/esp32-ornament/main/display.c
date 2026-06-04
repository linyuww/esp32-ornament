#include "display.h"

#include "display_core.h"

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_lcd_io_spi.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_st7789.h"
#include "esp_log.h"

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

static esp_err_t display_init_st7789_spi(void)
{
    const spi_bus_config_t bus_config = {
        .sclk_io_num = CONFIG_ORNAMENT_LCD_PIN_SCLK,
        .mosi_io_num = CONFIG_ORNAMENT_LCD_PIN_MOSI,
        .miso_io_num = -1,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = CONFIG_ORNAMENT_LCD_H_RES * 80 * sizeof(uint16_t),
    };
    ESP_RETURN_ON_ERROR(
        spi_bus_initialize(CONFIG_ORNAMENT_LCD_SPI_HOST, &bus_config, SPI_DMA_CH_AUTO),
        TAG,
        "spi_bus_initialize failed");

    esp_lcd_panel_io_handle_t io_handle = NULL;
    const esp_lcd_panel_io_spi_config_t io_config = {
        .cs_gpio_num = CONFIG_ORNAMENT_LCD_PIN_CS,
        .dc_gpio_num = CONFIG_ORNAMENT_LCD_PIN_DC,
        .spi_mode = 0,
        .pclk_hz = CONFIG_ORNAMENT_LCD_PIXEL_CLOCK_HZ,
        .trans_queue_depth = 10,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
    };
    ESP_RETURN_ON_ERROR(
        esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)CONFIG_ORNAMENT_LCD_SPI_HOST, &io_config, &io_handle),
        TAG,
        "esp_lcd_new_panel_io_spi failed");

    const esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = CONFIG_ORNAMENT_LCD_PIN_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
    };
    return esp_lcd_new_panel_st7789(io_handle, &panel_config, &panel_handle);
}

static void display_set_backlight(bool enabled)
{
    gpio_config_t config = {
        .pin_bit_mask = 1ULL << CONFIG_ORNAMENT_LCD_PIN_BL,
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&config);
    gpio_set_level(CONFIG_ORNAMENT_LCD_PIN_BL, enabled ? 1 : 0);
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
        const uint16_t *src = &canvas.pixels[(size_t)y * canvas.width];
        uint16_t *dst = flush_buffers[buffer_index];
        for (size_t i = 0; i < pixel_count; i++) {
            uint16_t pixel = src[i];
            dst[i] = (uint16_t)((pixel << 8) | (pixel >> 8));
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

static void render_voice_status(const ornament_state_t *state, const char *bridge_status, const char *voice_status)
{
    if (!canvas_alloc()) {
        ESP_LOGE(TAG, "failed to allocate display canvas");
        return;
    }
    display_core_render_voice_status(&canvas, state, bridge_status, voice_status);
    (void)flush_canvas();
}

static void render_xiaozhi(const ornament_state_t *state, const xiaozhi_client_snapshot_t *snapshot)
{
    if (!canvas_alloc()) {
        ESP_LOGE(TAG, "failed to allocate display canvas");
        return;
    }
    display_core_render_xiaozhi(&canvas, state, snapshot);
    (void)flush_canvas();
}

static void render_tasks(const ornament_state_t *state)
{
    if (!canvas_alloc()) {
        ESP_LOGE(TAG, "failed to allocate display canvas");
        return;
    }
    display_core_render_tasks(&canvas, state);
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
    ESP_LOGI(TAG, "initializing ST7789 SPI display");
    display_set_backlight(false);

    esp_err_t err = display_init_st7789_spi();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "display init failed: %s", esp_err_to_name(err));
        return;
    }

    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(panel_handle, true));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel_handle, true));
    ESP_ERROR_CHECK(esp_lcd_panel_mirror(panel_handle, false, false));
    display_set_backlight(true);
    canvas_alloc();
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

void display_render_voice_status(const ornament_state_t *state, const char *bridge_status, const char *voice_status)
{
    ESP_LOGI(TAG, "voice status: bridge=%s voice=%s", bridge_status, voice_status);
    render_voice_status(state, bridge_status, voice_status);
}

void display_render_xiaozhi(const ornament_state_t *state, const xiaozhi_client_snapshot_t *snapshot)
{
    ESP_LOGI(
        TAG,
        "xiaozhi display: state=%s up=%lu down=%lu",
        snapshot != NULL ? xiaozhi_client_state_name(snapshot->state) : "none",
        snapshot != NULL ? (unsigned long)snapshot->uplink_frames : 0UL,
        snapshot != NULL ? (unsigned long)snapshot->downlink_frames : 0UL);
    render_xiaozhi(state, snapshot);
}

void display_render_tasks(const ornament_state_t *state)
{
    ESP_LOGI(
        TAG,
        "voice tasks: codex=%d/%d claude=%d/%d",
        state->codex_active_task_count,
        state->codex_done_seq,
        state->claude_active_task_count,
        state->claude_done_seq);
    render_tasks(state);
}
