#include "display.h"

#include "display_core.h"

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "draw/sw/lv_draw_sw.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_lcd_io_spi.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_st7789.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "lvgl.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "display";
static const int FLUSH_LINES_CANDIDATES[] = {40, 24, 16, 8, 4, 1};
static const uint64_t LVGL_TICK_PERIOD_US = 1000;

LV_FONT_DECLARE(font_puhui_16_4);

static esp_lcd_panel_handle_t panel_handle;
static display_core_canvas_t canvas;
static uint16_t *flush_buffers[2];
static int flush_lines;

static bool lvgl_initialized;
static bool lvgl_ready;
static bool lvgl_xiaozhi_visible;
static lv_display_t *lvgl_display;
static lv_obj_t *lvgl_root;
static lv_obj_t *lvgl_header_label;
static lv_obj_t *lvgl_state_label;
static lv_obj_t *lvgl_user_card;
static lv_obj_t *lvgl_ai_card;
static lv_obj_t *lvgl_user_label;
static lv_obj_t *lvgl_ai_label;
static lv_obj_t *lvgl_footer_label;
static lv_obj_t *lvgl_spinner;
static esp_timer_handle_t lvgl_tick_timer;

static const char *const XIAOZHI_TITLE_TEXT = "\xE5\xB0\x8F\xE6\x99\xBA";
static const char *const XIAOZHI_STATUS_CONNECTING_TEXT = "\xE8\xBF\x9E\xE6\x8E\xA5\xE4\xB8\xAD";
static const char *const XIAOZHI_STATUS_LISTENING_TEXT = "\xE8\x81\x86\xE5\x90\xAC\xE4\xB8\xAD";
static const char *const XIAOZHI_STATUS_SPEAKING_TEXT = "\xE5\x9B\x9E\xE7\xAD\x94\xE4\xB8\xAD";
static const char *const XIAOZHI_STATUS_ERROR_TEXT = "\xE5\xBC\x82\xE5\xB8\xB8";
static const char *const XIAOZHI_STATUS_CONFIG_MISSING_TEXT = "\xE6\x9C\xAA\xE9\x85\x8D\xE7\xBD\xAE";
static const char *const XIAOZHI_STATUS_IDLE_TEXT = "\xE5\xBE\x85\xE6\x9C\xBA";
static const char *const XIAOZHI_STATUS_DISABLED_TEXT = "\xE5\x85\xB3\xE9\x97\xAD";
static const char *const XIAOZHI_PROMPT_SPEAK_TEXT = "\xE8\xAF\xB7\xE8\xAF\xB4\xE8\xAF\x9D...";
static const char *const XIAOZHI_WAIT_RESPONSE_TEXT = "\xE7\xAD\x89\xE5\xBE\x85\xE5\x9B\x9E\xE7\xAD\x94...";
static const char *const XIAOZHI_WAIT_BIND_TEXT = "\xE7\xAD\x89\xE5\xBE\x85\xE7\xBB\x91\xE5\xAE\x9A...";
static const char *const XIAOZHI_BIND_HINT_TEXT = "\xE8\xAF\xB7\xE7\xBB\x91\xE5\xAE\x9A\xE5\xAE\x98\xE6\x96\xB9\xE5\x90\x8E\xE5\x8F\xB0";
static const char *const XIAOZHI_WAIT_CODE_TEXT = "\xE7\xAD\x89\xE5\xBE\x85\xE4\xB8\x8B\xE5\x8F\x91";
static const char *const XIAOZHI_FOOTER_DEFAULT_TEXT = "\xE4\xB8\x8A\xE8\xA1\x8C 0  \xE4\xB8\x8B\xE8\xA1\x8C 0";

static bool flush_buffers_alloc(void);

static void lvgl_tick_cb(void *arg)
{
    (void)arg;
    lv_tick_inc(1);
}

static void lvgl_flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    if (panel_handle == NULL || area == NULL || px_map == NULL) {
        lv_display_flush_ready(disp);
        return;
    }

    lv_draw_sw_rgb565_swap(px_map, (uint32_t)lv_area_get_size(area));

    esp_err_t err = esp_lcd_panel_draw_bitmap(
        panel_handle,
        area->x1,
        area->y1,
        area->x2 + 1,
        area->y2 + 1,
        px_map);
    if (err != ESP_OK) {
        ESP_LOGE(
            TAG,
            "lvgl flush failed x1=%d y1=%d x2=%d y2=%d: %s",
            (int)area->x1,
            (int)area->y1,
            (int)area->x2,
            (int)area->y2,
            esp_err_to_name(err));
    }
    lv_display_flush_ready(disp);
}

static void lvgl_style_card(lv_obj_t *obj, lv_color_t bg_color)
{
    lv_obj_remove_style_all(obj);
    lv_obj_set_scrollbar_mode(obj, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(obj, bg_color, 0);
    lv_obj_set_style_radius(obj, 16, 0);
    lv_obj_set_style_border_width(obj, 0, 0);
    lv_obj_set_style_pad_left(obj, 14, 0);
    lv_obj_set_style_pad_right(obj, 14, 0);
    lv_obj_set_style_pad_top(obj, 12, 0);
    lv_obj_set_style_pad_bottom(obj, 12, 0);
}

static void lvgl_style_label(lv_obj_t *obj, const lv_font_t *font, lv_color_t color, lv_text_align_t align)
{
    lv_obj_set_style_text_font(obj, font, 0);
    lv_obj_set_style_text_color(obj, color, 0);
    lv_obj_set_style_text_align(obj, align, 0);
}

static void lvgl_update_now(void)
{
    if (!lvgl_ready) {
        return;
    }
    lv_timer_handler();
}

static void lvgl_set_xiaozhi_visible(bool visible)
{
    if (!lvgl_ready || lvgl_root == NULL) {
        return;
    }

    if (visible) {
        lv_obj_remove_flag(lvgl_root, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(lvgl_root, LV_OBJ_FLAG_HIDDEN);
    }
    lvgl_xiaozhi_visible = visible;
}

static const char *xiaozhi_status_cn(xiaozhi_client_state_t state)
{
    switch (state) {
    case XIAOZHI_CLIENT_STATE_CONNECTING:
        return XIAOZHI_STATUS_CONNECTING_TEXT;
    case XIAOZHI_CLIENT_STATE_LISTENING:
        return XIAOZHI_STATUS_LISTENING_TEXT;
    case XIAOZHI_CLIENT_STATE_SPEAKING:
        return XIAOZHI_STATUS_SPEAKING_TEXT;
    case XIAOZHI_CLIENT_STATE_ERROR:
        return XIAOZHI_STATUS_ERROR_TEXT;
    case XIAOZHI_CLIENT_STATE_CONFIG_MISSING:
        return XIAOZHI_STATUS_CONFIG_MISSING_TEXT;
    case XIAOZHI_CLIENT_STATE_IDLE:
        return XIAOZHI_STATUS_IDLE_TEXT;
    case XIAOZHI_CLIENT_STATE_DISABLED:
    default:
        return XIAOZHI_STATUS_DISABLED_TEXT;
    }
}

static const char *xiaozhi_default_user_text(const xiaozhi_client_snapshot_t *snapshot)
{
    if (snapshot != NULL && snapshot->activation_pending) {
        return XIAOZHI_WAIT_BIND_TEXT;
    }
    return XIAOZHI_PROMPT_SPEAK_TEXT;
}

static const char *xiaozhi_default_ai_text(const xiaozhi_client_snapshot_t *snapshot)
{
    if (snapshot != NULL && snapshot->activation_pending) {
        if (snapshot->activation_code[0] != '\0') {
            return snapshot->activation_code;
        }
        return XIAOZHI_BIND_HINT_TEXT;
    }
    return XIAOZHI_WAIT_RESPONSE_TEXT;
}

static void lvgl_cleanup_failed_init(void)
{
    if (lvgl_tick_timer != NULL) {
        esp_timer_stop(lvgl_tick_timer);
        esp_timer_delete(lvgl_tick_timer);
        lvgl_tick_timer = NULL;
    }
    if (lvgl_display != NULL) {
        lv_display_delete(lvgl_display);
        lvgl_display = NULL;
    }
    lvgl_root = NULL;
    lvgl_header_label = NULL;
    lvgl_state_label = NULL;
    lvgl_user_card = NULL;
    lvgl_ai_card = NULL;
    lvgl_user_label = NULL;
    lvgl_ai_label = NULL;
    lvgl_footer_label = NULL;
    lvgl_spinner = NULL;
    lvgl_ready = false;
    lvgl_xiaozhi_visible = false;
}

static void lvgl_release_xiaozhi_overlay(void)
{
    if (lvgl_display == NULL && lvgl_tick_timer == NULL && !lvgl_ready) {
        return;
    }

    lvgl_cleanup_failed_init();
    ESP_LOGI(TAG, "LVGL overlay released");
}

static esp_err_t lvgl_xiaozhi_overlay_init(void)
{
    if (lvgl_ready) {
        return ESP_OK;
    }

    if (!lvgl_initialized) {
        lv_init();
        lvgl_initialized = true;
    }

    const esp_timer_create_args_t tick_args = {
        .callback = lvgl_tick_cb,
        .arg = NULL,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "lvgl_tick",
        .skip_unhandled_events = true,
    };
    esp_err_t err = esp_timer_create(&tick_args, &lvgl_tick_timer);
    if (err != ESP_OK) {
        return err;
    }

    err = esp_timer_start_periodic(lvgl_tick_timer, LVGL_TICK_PERIOD_US);
    if (err != ESP_OK) {
        lvgl_cleanup_failed_init();
        return err;
    }

    lvgl_display = lv_display_create(CONFIG_ORNAMENT_LCD_H_RES, CONFIG_ORNAMENT_LCD_V_RES);
    if (lvgl_display == NULL) {
        lvgl_cleanup_failed_init();
        return ESP_FAIL;
    }
    lv_display_set_color_format(lvgl_display, LV_COLOR_FORMAT_RGB565);

    if (!flush_buffers_alloc()) {
        lvgl_cleanup_failed_init();
        return ESP_ERR_NO_MEM;
    }
    size_t buffer_bytes = (size_t)CONFIG_ORNAMENT_LCD_H_RES * (size_t)flush_lines * sizeof(uint16_t);

    lv_display_set_buffers(
        lvgl_display,
        flush_buffers[0],
        flush_buffers[1],
        (uint32_t)buffer_bytes,
        LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(lvgl_display, lvgl_flush_cb);
    lv_display_set_default(lvgl_display);
    ESP_LOGI(TAG, "LVGL overlay reusing %d-line DMA flush buffers (%u bytes each)", flush_lines, (unsigned int)buffer_bytes);

    lv_obj_t *screen = lv_screen_active();
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x05070b), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_set_style_text_font(screen, &font_puhui_16_4, 0);
    lv_obj_set_style_text_color(screen, lv_color_hex(0xf6f7fb), 0);

    lvgl_root = lv_obj_create(screen);
    lv_obj_remove_style_all(lvgl_root);
    lv_obj_set_size(lvgl_root, CONFIG_ORNAMENT_LCD_H_RES, CONFIG_ORNAMENT_LCD_V_RES);
    lv_obj_set_style_bg_opa(lvgl_root, LV_OPA_TRANSP, 0);
    lv_obj_set_scrollbar_mode(lvgl_root, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_flag(lvgl_root, LV_OBJ_FLAG_HIDDEN);

    lvgl_header_label = lv_label_create(lvgl_root);
    lvgl_style_label(lvgl_header_label, &font_puhui_16_4, lv_color_hex(0xf7fafc), LV_TEXT_ALIGN_CENTER);
    lv_label_set_text(lvgl_header_label, XIAOZHI_TITLE_TEXT);
    lv_obj_set_width(lvgl_header_label, 240);
    lv_obj_align(lvgl_header_label, LV_ALIGN_TOP_MID, 0, 16);

    lvgl_state_label = lv_label_create(lvgl_root);
    lvgl_style_label(lvgl_state_label, &font_puhui_16_4, lv_color_hex(0x8f9aa8), LV_TEXT_ALIGN_CENTER);
    lv_label_set_text_fmt(lvgl_state_label, "%s %s", XIAOZHI_TITLE_TEXT, XIAOZHI_STATUS_IDLE_TEXT);
    lv_obj_set_width(lvgl_state_label, 220);
    lv_obj_align(lvgl_state_label, LV_ALIGN_TOP_MID, 0, 42);

    lvgl_spinner = lv_spinner_create(lvgl_root);
    lv_spinner_set_anim_params(lvgl_spinner, 900, 90);
    lv_obj_set_size(lvgl_spinner, 20, 20);
    lv_obj_set_style_arc_width(lvgl_spinner, 3, LV_PART_MAIN);
    lv_obj_set_style_arc_color(lvgl_spinner, lv_color_hex(0x1d3348), LV_PART_MAIN);
    lv_obj_set_style_arc_width(lvgl_spinner, 3, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(lvgl_spinner, lv_color_hex(0x4dc7ff), LV_PART_INDICATOR);
    lv_obj_align(lvgl_spinner, LV_ALIGN_TOP_RIGHT, -18, 16);

    lvgl_user_card = lv_obj_create(lvgl_root);
    lvgl_style_card(lvgl_user_card, lv_color_hex(0x153248));
    lv_obj_set_size(lvgl_user_card, 212, 72);
    lv_obj_align(lvgl_user_card, LV_ALIGN_TOP_MID, 0, 72);

    lvgl_ai_card = lv_obj_create(lvgl_root);
    lvgl_style_card(lvgl_ai_card, lv_color_hex(0x113a2e));
    lv_obj_set_size(lvgl_ai_card, 212, 96);
    lv_obj_align(lvgl_ai_card, LV_ALIGN_TOP_MID, 0, 154);

    lvgl_user_label = lv_label_create(lvgl_user_card);
    lvgl_style_label(lvgl_user_label, &font_puhui_16_4, lv_color_hex(0xf8fbff), LV_TEXT_ALIGN_LEFT);
    lv_label_set_long_mode(lvgl_user_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(lvgl_user_label, 184);
    lv_obj_align(lvgl_user_label, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_label_set_text(lvgl_user_label, XIAOZHI_PROMPT_SPEAK_TEXT);

    lvgl_ai_label = lv_label_create(lvgl_ai_card);
    lvgl_style_label(lvgl_ai_label, &font_puhui_16_4, lv_color_hex(0xf8fbff), LV_TEXT_ALIGN_LEFT);
    lv_label_set_long_mode(lvgl_ai_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(lvgl_ai_label, 184);
    lv_obj_align(lvgl_ai_label, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_label_set_text(lvgl_ai_label, XIAOZHI_WAIT_RESPONSE_TEXT);

    lvgl_footer_label = lv_label_create(lvgl_root);
    lvgl_style_label(lvgl_footer_label, &font_puhui_16_4, lv_color_hex(0x8f9aa8), LV_TEXT_ALIGN_CENTER);
    lv_label_set_text(lvgl_footer_label, XIAOZHI_FOOTER_DEFAULT_TEXT);
    lv_obj_set_width(lvgl_footer_label, 220);
    lv_obj_align(lvgl_footer_label, LV_ALIGN_BOTTOM_MID, 0, -18);

    lvgl_ready = true;
    lvgl_xiaozhi_visible = false;
    lvgl_update_now();
    return ESP_OK;
}

static void lvgl_render_xiaozhi_overlay(const xiaozhi_client_snapshot_t *snapshot)
{
    xiaozhi_client_snapshot_t fallback = {0};
    if (!lvgl_ready || lvgl_root == NULL) {
        return;
    }

    if (snapshot == NULL) {
        fallback.state = XIAOZHI_CLIENT_STATE_DISABLED;
        snapshot = &fallback;
    }

    char state_text[32];
    char footer_text[96];
    const char *user_text = snapshot->last_stt[0] != '\0' ? snapshot->last_stt : xiaozhi_default_user_text(snapshot);
    const char *ai_text = snapshot->last_tts[0] != '\0' ? snapshot->last_tts : xiaozhi_default_ai_text(snapshot);

    snprintf(state_text, sizeof(state_text), "%s %s", XIAOZHI_TITLE_TEXT, xiaozhi_status_cn(snapshot->state));
    if (snapshot->activation_pending) {
        snprintf(
            footer_text,
            sizeof(footer_text),
            "\xE9\xAA\x8C\xE8\xAF\x81\xE7\xA0\x81 %s",
            snapshot->activation_code[0] != '\0' ? snapshot->activation_code : XIAOZHI_WAIT_CODE_TEXT);
    } else {
        snprintf(
            footer_text,
            sizeof(footer_text),
            "\xE4\xB8\x8A\xE8\xA1\x8C %lu  \xE4\xB8\x8B\xE8\xA1\x8C %lu",
            (unsigned long)snapshot->uplink_frames,
            (unsigned long)snapshot->downlink_frames);
    }

    lv_label_set_text(lvgl_state_label, state_text);
    lv_label_set_text(lvgl_user_label, user_text);
    lv_label_set_text(lvgl_ai_label, ai_text);
    lv_label_set_text(lvgl_footer_label, footer_text);

    lv_color_t state_color = lv_color_hex(0x8f9aa8);
    switch (snapshot->state) {
    case XIAOZHI_CLIENT_STATE_LISTENING:
        state_color = lv_color_hex(0x24c16a);
        break;
    case XIAOZHI_CLIENT_STATE_SPEAKING:
        state_color = lv_color_hex(0x36c2ff);
        break;
    case XIAOZHI_CLIENT_STATE_CONNECTING:
        state_color = lv_color_hex(0xf4b942);
        break;
    case XIAOZHI_CLIENT_STATE_ERROR:
    case XIAOZHI_CLIENT_STATE_CONFIG_MISSING:
        state_color = lv_color_hex(0xff6b6b);
        break;
    case XIAOZHI_CLIENT_STATE_IDLE:
    case XIAOZHI_CLIENT_STATE_DISABLED:
    default:
        break;
    }
    lv_obj_set_style_text_color(lvgl_state_label, state_color, 0);

    if (snapshot->state == XIAOZHI_CLIENT_STATE_CONNECTING || snapshot->state == XIAOZHI_CLIENT_STATE_IDLE) {
        lv_obj_remove_flag(lvgl_spinner, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(lvgl_spinner, LV_OBJ_FLAG_HIDDEN);
    }

    lvgl_set_xiaozhi_visible(true);
    lvgl_update_now();
}

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

static void hide_xiaozhi_overlay_if_needed(void)
{
    if (lvgl_display != NULL || lvgl_ready || lvgl_xiaozhi_visible) {
        lvgl_release_xiaozhi_overlay();
    }
}

static void render_hud(const ornament_state_t *state)
{
    hide_xiaozhi_overlay_if_needed();
    if (!canvas_alloc()) {
        ESP_LOGE(TAG, "failed to allocate display canvas");
        return;
    }
    display_core_render_hud(&canvas, state);
    (void)flush_canvas();
}

static void render_clock(const ornament_state_t *state)
{
    hide_xiaozhi_overlay_if_needed();
    if (!canvas_alloc()) {
        ESP_LOGE(TAG, "failed to allocate display canvas");
        return;
    }
    display_core_render_clock(&canvas, state);
    (void)flush_canvas();
}

static void render_voice_status(const ornament_state_t *state, const char *bridge_status, const char *voice_status)
{
    hide_xiaozhi_overlay_if_needed();
    if (!canvas_alloc()) {
        ESP_LOGE(TAG, "failed to allocate display canvas");
        return;
    }
    display_core_render_voice_status(&canvas, state, bridge_status, voice_status);
    (void)flush_canvas();
}

static void render_xiaozhi(const ornament_state_t *state, const xiaozhi_client_snapshot_t *snapshot)
{
    (void)state;
    if (lvgl_xiaozhi_overlay_init() == ESP_OK) {
        lvgl_render_xiaozhi_overlay(snapshot);
        return;
    }

    if (!canvas_alloc()) {
        ESP_LOGE(TAG, "failed to allocate display canvas");
        return;
    }
    display_core_render_xiaozhi(&canvas, state, snapshot);
    (void)flush_canvas();
}

static void render_tasks(const ornament_state_t *state)
{
    hide_xiaozhi_overlay_if_needed();
    if (!canvas_alloc()) {
        ESP_LOGE(TAG, "failed to allocate display canvas");
        return;
    }
    display_core_render_tasks(&canvas, state);
    (void)flush_canvas();
}

static void render_boot_message(void)
{
    hide_xiaozhi_overlay_if_needed();
    if (!canvas_alloc()) {
        ESP_LOGE(TAG, "failed to allocate display canvas");
        return;
    }
    display_core_render_boot(&canvas);
    (void)flush_canvas();
}

static void render_status_message(const char *message)
{
    hide_xiaozhi_overlay_if_needed();
    if (!canvas_alloc()) {
        ESP_LOGE(TAG, "failed to allocate display canvas");
        return;
    }
    display_core_render_status_message(&canvas, message);
    (void)flush_canvas();
}

static void render_error_message(const char *message)
{
    hide_xiaozhi_overlay_if_needed();
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
