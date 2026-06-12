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
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#ifndef CONFIG_ORNAMENT_ST77916_DISPLAY_ENABLED
#define CONFIG_ORNAMENT_ST77916_DISPLAY_ENABLED 0
#endif

#if CONFIG_ORNAMENT_ST77916_DISPLAY_ENABLED
#include "esp_lcd_st77916.h"
#include "st77916_truly_init.h"
#endif

#ifndef CONFIG_ORNAMENT_RICH_XIAOZHI_DISPLAY_ENABLED
#define CONFIG_ORNAMENT_RICH_XIAOZHI_DISPLAY_ENABLED 0
#endif

#if CONFIG_ORNAMENT_RICH_XIAOZHI_DISPLAY_ENABLED
#include "draw/sw/lv_draw_sw.h"
#include "lvgl.h"
#endif

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef CONFIG_ORNAMENT_LCD_BL_ACTIVE_LEVEL
#define CONFIG_ORNAMENT_LCD_BL_ACTIVE_LEVEL 1
#endif

#ifndef CONFIG_ORNAMENT_LCD_BOOT_TEST_MS
#define CONFIG_ORNAMENT_LCD_BOOT_TEST_MS 0
#endif

#ifndef CONFIG_ORNAMENT_LCD_SWAP_COLOR_BYTES
#define CONFIG_ORNAMENT_LCD_SWAP_COLOR_BYTES 1
#endif

#ifndef CONFIG_ORNAMENT_LCD_SWAP_XY
#define CONFIG_ORNAMENT_LCD_SWAP_XY 0
#endif

#ifndef CONFIG_ORNAMENT_LCD_REFERENCE_TEST_ONLY
#define CONFIG_ORNAMENT_LCD_REFERENCE_TEST_ONLY 0
#endif

static const char *TAG = "display";
static const int FLUSH_LINES_CANDIDATES[] = {40, 24, 16, 8, 4, 1};

static bool flush_buffers_alloc(void);

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

#if CONFIG_ORNAMENT_RICH_XIAOZHI_DISPLAY_ENABLED

static const uint64_t LVGL_TICK_PERIOD_US = 1000;

LV_FONT_DECLARE(font_puhui_16_4);

static bool lvgl_initialized;
static bool lvgl_ready;
static bool lvgl_xiaozhi_visible;
static lv_display_t *lvgl_display;
static lv_obj_t *lvgl_root;
static lv_obj_t *lvgl_state_label;
static lv_obj_t *lvgl_user_card;
static lv_obj_t *lvgl_ai_card;
static lv_obj_t *lvgl_user_label;
static lv_obj_t *lvgl_ai_label;
static lv_obj_t *lvgl_footer_label;
static lv_obj_t *lvgl_spinner;
static esp_timer_handle_t lvgl_tick_timer;

static const char *const XIAOZHI_STATUS_CONNECTING_TEXT = "\xE8\xBF\x9E\xE6\x8E\xA5\xE4\xB8\xAD";
static const char *const XIAOZHI_STATUS_LISTENING_TEXT = "\xE8\x81\x86\xE5\x90\xAC\xE4\xB8\xAD";
static const char *const XIAOZHI_STATUS_SPEAKING_TEXT = "\xE5\x9B\x9E\xE7\xAD\x94\xE4\xB8\xAD";
static const char *const XIAOZHI_STATUS_ERROR_TEXT = "\xE5\xBC\x82\xE5\xB8\xB8";
static const char *const XIAOZHI_STATUS_CONFIG_MISSING_TEXT = "\xE6\x9C\xAA\xE9\x85\x8D\xE7\xBD\xAE";
static const char *const XIAOZHI_STATUS_IDLE_TEXT = "\xE5\xBE\x85\xE6\x9C\xBA";
static const char *const XIAOZHI_STATUS_DISABLED_TEXT = "\xE5\x85\xB3\xE9\x97\xAD";
static const char *const XIAOZHI_WAIT_CODE_TEXT = "\xE7\xAD\x89\xE5\xBE\x85\xE4\xB8\x8B\xE5\x8F\x91";
static const char *const XIAOZHI_FOOTER_READY_TEXT = "\xE8\xAF\xAD\xE9\x9F\xB3\xE5\x8A\xA9\xE6\x89\x8B\xE5\xB7\xB2\xE5\x87\x86\xE5\xA4\x87";
static const char *const XIAOZHI_FOOTER_ONLINE_TEXT = "\xE5\xB0\x8F\xE6\x99\xBA\xE5\xB7\xB2\xE8\xBF\x9E\xE6\x8E\xA5";
static const char *const XIAOZHI_FOOTER_OFFLINE_TEXT = "\xE7\xAD\x89\xE5\xBE\x85\xE8\xBF\x9E\xE6\x8E\xA5";

typedef struct {
    int margin;
    int gap;
    int card_w;
    int user_y;
    int user_h;
    int ai_y;
    int ai_h;
    int radius;
    int pad_x;
    int pad_y;
    int label_w;
    int state_y;
    int footer_bottom;
    int spinner_size;
} xiaozhi_lvgl_layout_t;

static int lvgl_min_int(int a, int b)
{
    return a < b ? a : b;
}

static int lvgl_max_int(int a, int b)
{
    return a > b ? a : b;
}

static int lvgl_clamp_int(int value, int min_value, int max_value)
{
    if (value < min_value) {
        return min_value;
    }
    if (value > max_value) {
        return max_value;
    }
    return value;
}

static int lvgl_scale_min(int value)
{
    int x_scaled = value * CONFIG_ORNAMENT_LCD_H_RES / 240;
    int y_scaled = value * CONFIG_ORNAMENT_LCD_V_RES / 240;
    return lvgl_min_int(x_scaled, y_scaled);
}

static void xiaozhi_lvgl_layout(xiaozhi_lvgl_layout_t *layout)
{
    if (layout == NULL) {
        return;
    }

    const int screen_w = CONFIG_ORNAMENT_LCD_H_RES;
    const int screen_h = CONFIG_ORNAMENT_LCD_V_RES;
    const int min_side = lvgl_min_int(screen_w, screen_h);
    const int header_h = lvgl_clamp_int(lvgl_scale_min(24), 22, 30);
    const int footer_h = lvgl_clamp_int(lvgl_scale_min(18), 16, 22);

    layout->margin = lvgl_clamp_int(min_side / 17, 10, 18);
    layout->gap = lvgl_clamp_int(min_side / 34, 6, 10);
    layout->state_y = layout->margin;
    layout->footer_bottom = -layout->margin;
    layout->spinner_size = lvgl_clamp_int(min_side / 12, 16, 22);

    int available_w = screen_w - layout->margin * 2;
    int offset = layout->gap * 2;
    layout->card_w = available_w - offset;
    if (layout->card_w < available_w * 4 / 5) {
        layout->card_w = available_w * 4 / 5;
    }
    layout->card_w = lvgl_clamp_int(layout->card_w, lvgl_min_int(available_w, 150), available_w);
    layout->radius = lvgl_clamp_int(min_side / 30, 6, 8);
    layout->pad_x = lvgl_clamp_int(layout->card_w / 16, 10, 14);
    layout->pad_y = lvgl_clamp_int(min_side / 30, 7, 10);
    layout->label_w = lvgl_max_int(24, layout->card_w - layout->pad_x * 2);

    const int cards_top = layout->margin + header_h + layout->gap;
    const int cards_bottom = screen_h - layout->margin - footer_h - layout->gap;
    int cards_h = cards_bottom - cards_top;
    int min_card_h = lvgl_clamp_int(lvgl_scale_min(54), 46, 62);
    if (cards_h < min_card_h * 2 + layout->gap) {
        min_card_h = lvgl_max_int(34, (cards_h - layout->gap) / 2);
    }

    layout->user_h = lvgl_max_int(min_card_h, cards_h * 42 / 100);
    layout->ai_h = cards_h - layout->user_h - layout->gap;
    if (layout->ai_h < min_card_h) {
        layout->ai_h = min_card_h;
        layout->user_h = lvgl_max_int(34, cards_h - layout->ai_h - layout->gap);
    }

    layout->user_y = cards_top;
    layout->ai_y = layout->user_y + layout->user_h + layout->gap;
}

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

static void lvgl_style_card(lv_obj_t *obj, lv_color_t bg_color, lv_color_t border_color, int radius)
{
    lv_obj_remove_style_all(obj);
    lv_obj_set_scrollbar_mode(obj, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(obj, bg_color, 0);
    lv_obj_set_style_radius(obj, radius, 0);
    lv_obj_set_style_border_width(obj, 1, 0);
    lv_obj_set_style_border_color(obj, border_color, 0);
    lv_obj_set_style_pad_all(obj, 0, 0);
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

static esp_err_t __attribute__((unused)) lvgl_xiaozhi_overlay_init(void)
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
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x070b10), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_set_style_text_font(screen, &font_puhui_16_4, 0);
    lv_obj_set_style_text_color(screen, lv_color_hex(0xf6f7fb), 0);

    xiaozhi_lvgl_layout_t layout = {0};
    xiaozhi_lvgl_layout(&layout);

    lvgl_root = lv_obj_create(screen);
    lv_obj_remove_style_all(lvgl_root);
    lv_obj_set_size(lvgl_root, CONFIG_ORNAMENT_LCD_H_RES, CONFIG_ORNAMENT_LCD_V_RES);
    lv_obj_set_style_bg_opa(lvgl_root, LV_OPA_TRANSP, 0);
    lv_obj_set_scrollbar_mode(lvgl_root, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_flag(lvgl_root, LV_OBJ_FLAG_HIDDEN);

    lvgl_state_label = lv_label_create(lvgl_root);
    lvgl_style_label(lvgl_state_label, &font_puhui_16_4, lv_color_hex(0x98a2b3), LV_TEXT_ALIGN_CENTER);
    lv_label_set_text(lvgl_state_label, XIAOZHI_STATUS_IDLE_TEXT);
    lv_obj_set_width(lvgl_state_label, CONFIG_ORNAMENT_LCD_H_RES - layout.margin * 2);
    lv_obj_align(lvgl_state_label, LV_ALIGN_TOP_MID, 0, layout.state_y);

    lvgl_spinner = lv_spinner_create(lvgl_root);
    lv_spinner_set_anim_params(lvgl_spinner, 900, 90);
    lv_obj_set_size(lvgl_spinner, layout.spinner_size, layout.spinner_size);
    lv_obj_set_style_arc_width(lvgl_spinner, 2, LV_PART_MAIN);
    lv_obj_set_style_arc_color(lvgl_spinner, lv_color_hex(0x223047), LV_PART_MAIN);
    lv_obj_set_style_arc_width(lvgl_spinner, 2, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(lvgl_spinner, lv_color_hex(0x4cc9f0), LV_PART_INDICATOR);
    lv_obj_align(lvgl_spinner, LV_ALIGN_TOP_RIGHT, -layout.margin, layout.state_y);

    lvgl_user_card = lv_obj_create(lvgl_root);
    lvgl_style_card(lvgl_user_card, lv_color_hex(0x10263a), lv_color_hex(0x1d4764), layout.radius);
    lv_obj_set_size(lvgl_user_card, layout.card_w, layout.user_h);
    lv_obj_set_pos(
        lvgl_user_card,
        CONFIG_ORNAMENT_LCD_H_RES - layout.margin - layout.card_w,
        layout.user_y);

    lvgl_ai_card = lv_obj_create(lvgl_root);
    lvgl_style_card(lvgl_ai_card, lv_color_hex(0x10251f), lv_color_hex(0x24634e), layout.radius);
    lv_obj_set_size(lvgl_ai_card, layout.card_w, layout.ai_h);
    lv_obj_set_pos(lvgl_ai_card, layout.margin, layout.ai_y);

    lvgl_user_label = lv_label_create(lvgl_user_card);
    lvgl_style_label(lvgl_user_label, &font_puhui_16_4, lv_color_hex(0xf8fbff), LV_TEXT_ALIGN_LEFT);
    lv_label_set_long_mode(lvgl_user_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_size(lvgl_user_label, layout.label_w, layout.user_h - layout.pad_y * 2);
    lv_obj_set_pos(lvgl_user_label, layout.pad_x, layout.pad_y);
    lv_label_set_text(lvgl_user_label, "");

    lvgl_ai_label = lv_label_create(lvgl_ai_card);
    lvgl_style_label(lvgl_ai_label, &font_puhui_16_4, lv_color_hex(0xf8fbff), LV_TEXT_ALIGN_LEFT);
    lv_label_set_long_mode(lvgl_ai_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_size(lvgl_ai_label, layout.label_w, layout.ai_h - layout.pad_y * 2);
    lv_obj_set_pos(lvgl_ai_label, layout.pad_x, layout.pad_y);
    lv_label_set_text(lvgl_ai_label, "");

    lvgl_footer_label = lv_label_create(lvgl_root);
    lvgl_style_label(lvgl_footer_label, &font_puhui_16_4, lv_color_hex(0x94a3b8), LV_TEXT_ALIGN_CENTER);
    lv_label_set_text(lvgl_footer_label, XIAOZHI_FOOTER_READY_TEXT);
    lv_obj_set_width(lvgl_footer_label, CONFIG_ORNAMENT_LCD_H_RES - layout.margin * 2);
    lv_obj_align(lvgl_footer_label, LV_ALIGN_BOTTOM_MID, 0, layout.footer_bottom);

    lvgl_ready = true;
    lvgl_xiaozhi_visible = false;
    lvgl_update_now();
    return ESP_OK;
}

static void __attribute__((unused)) lvgl_render_xiaozhi_overlay(const xiaozhi_client_snapshot_t *snapshot)
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
    const char *footer_text = XIAOZHI_FOOTER_READY_TEXT;
    const char *user_text = snapshot->last_stt;
    const char *ai_text = snapshot->last_tts;

    snprintf(state_text, sizeof(state_text), "%s", xiaozhi_status_cn(snapshot->state));
    if (snapshot->activation_pending) {
        if (snapshot->activation_code[0] != '\0') {
            user_text = snapshot->activation_code;
            ai_text = snapshot->activation_code;
        } else if (snapshot->activation_message[0] != '\0') {
            ai_text = snapshot->activation_message;
        }
        footer_text = snapshot->activation_code[0] != '\0' ? snapshot->activation_code : XIAOZHI_WAIT_CODE_TEXT;
    } else if ((snapshot->state == XIAOZHI_CLIENT_STATE_ERROR ||
                snapshot->state == XIAOZHI_CLIENT_STATE_CONFIG_MISSING) &&
               snapshot->last_error[0] != '\0') {
        user_text = snapshot->last_error;
        ai_text = "";
    } else if (snapshot->connected) {
        footer_text = XIAOZHI_FOOTER_ONLINE_TEXT;
    } else if (snapshot->session_requested || snapshot->state == XIAOZHI_CLIENT_STATE_CONNECTING) {
        footer_text = XIAOZHI_FOOTER_OFFLINE_TEXT;
    } else {
        footer_text = XIAOZHI_FOOTER_READY_TEXT;
    }

    lv_label_set_text(lvgl_state_label, state_text);
    lv_label_set_text(lvgl_user_label, user_text);
    lv_label_set_text(lvgl_ai_label, ai_text);
    lv_label_set_text(lvgl_footer_label, footer_text);

    lv_color_t state_color = lv_color_hex(0x8f9aa8);
    lv_color_t user_border_color = lv_color_hex(0x1d4764);
    lv_color_t ai_border_color = lv_color_hex(0x24634e);
    switch (snapshot->state) {
    case XIAOZHI_CLIENT_STATE_LISTENING:
        state_color = lv_color_hex(0x35d07f);
        user_border_color = lv_color_hex(0x35d07f);
        break;
    case XIAOZHI_CLIENT_STATE_SPEAKING:
        state_color = lv_color_hex(0x4cc9f0);
        ai_border_color = lv_color_hex(0x4cc9f0);
        break;
    case XIAOZHI_CLIENT_STATE_CONNECTING:
        state_color = lv_color_hex(0xf6c453);
        user_border_color = lv_color_hex(0x5a4b24);
        ai_border_color = lv_color_hex(0x5a4b24);
        break;
    case XIAOZHI_CLIENT_STATE_ERROR:
    case XIAOZHI_CLIENT_STATE_CONFIG_MISSING:
        state_color = lv_color_hex(0xff6b6b);
        user_border_color = lv_color_hex(0x7f2d35);
        ai_border_color = lv_color_hex(0x7f2d35);
        break;
    case XIAOZHI_CLIENT_STATE_IDLE:
    case XIAOZHI_CLIENT_STATE_DISABLED:
    default:
        break;
    }
    lv_obj_set_style_text_color(lvgl_state_label, state_color, 0);
    lv_obj_set_style_border_color(lvgl_user_card, user_border_color, 0);
    lv_obj_set_style_border_color(lvgl_ai_card, ai_border_color, 0);

    if (snapshot->state == XIAOZHI_CLIENT_STATE_CONNECTING ||
        snapshot->state == XIAOZHI_CLIENT_STATE_LISTENING ||
        snapshot->state == XIAOZHI_CLIENT_STATE_SPEAKING) {
        lv_obj_remove_flag(lvgl_spinner, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(lvgl_spinner, LV_OBJ_FLAG_HIDDEN);
    }

    lvgl_set_xiaozhi_visible(true);
    lvgl_update_now();
}

#endif

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

#if CONFIG_ORNAMENT_ST77916_DISPLAY_ENABLED
static esp_err_t display_init_st77916_spi(void)
{
    const spi_bus_config_t bus_config = ST77916_PANEL_BUS_SPI_CONFIG(
        CONFIG_ORNAMENT_LCD_PIN_SCLK,
        CONFIG_ORNAMENT_LCD_PIN_MOSI,
        CONFIG_ORNAMENT_LCD_H_RES * 80 * sizeof(uint16_t));
    ESP_RETURN_ON_ERROR(
        spi_bus_initialize(CONFIG_ORNAMENT_LCD_SPI_HOST, &bus_config, SPI_DMA_CH_AUTO),
        TAG,
        "st77916 spi bus initialize failed");

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
        "st77916 spi panel io failed");

    st77916_vendor_config_t vendor_config = {
        .init_cmds = st77916_truly_init_cmds,
        .init_cmds_size = sizeof(st77916_truly_init_cmds) / sizeof(st77916_truly_init_cmds[0]),
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

static esp_err_t display_init_st77916_qspi(void)
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
        "st77916 qspi bus initialize failed");

    esp_lcd_panel_io_handle_t io_handle = NULL;
    esp_lcd_panel_io_spi_config_t io_config = ST77916_PANEL_IO_QSPI_CONFIG(
        CONFIG_ORNAMENT_LCD_PIN_CS,
        NULL,
        NULL);
    io_config.pclk_hz = CONFIG_ORNAMENT_LCD_PIXEL_CLOCK_HZ;
    ESP_RETURN_ON_ERROR(
        esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)CONFIG_ORNAMENT_LCD_SPI_HOST, &io_config, &io_handle),
        TAG,
        "st77916 qspi panel io failed");

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
#endif

static esp_err_t display_init_panel_driver(void)
{
    if (strcmp(CONFIG_ORNAMENT_DISPLAY_DRIVER, "st7789_spi") == 0) {
        return display_init_st7789_spi();
    }
#if CONFIG_ORNAMENT_ST77916_DISPLAY_ENABLED
    if (strcmp(CONFIG_ORNAMENT_DISPLAY_DRIVER, "st77916_spi") == 0) {
        return display_init_st77916_spi();
    }
    if (strcmp(CONFIG_ORNAMENT_DISPLAY_DRIVER, "st77916_qspi") == 0) {
        return display_init_st77916_qspi();
    }
#endif
    ESP_LOGE(TAG, "unsupported lcd driver: %s", CONFIG_ORNAMENT_DISPLAY_DRIVER);
    return ESP_ERR_NOT_SUPPORTED;
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
        if (CONFIG_ORNAMENT_LCD_SWAP_COLOR_BYTES) {
            for (size_t i = 0; i < pixel_count; i++) {
                uint16_t pixel = src[i];
                dst[i] = (uint16_t)((pixel << 8) | (pixel >> 8));
            }
        } else {
            memcpy(dst, src, pixel_count * sizeof(uint16_t));
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
    const int w = canvas.width;
    const int h = canvas.height;

    fill_canvas_color(dark);
    fill_canvas_rect(0, 0, w, 8, yellow);
    fill_canvas_rect(0, h - 8, w, 8, yellow);
    fill_canvas_rect(0, 0, 8, h, yellow);
    fill_canvas_rect(w - 8, 0, 8, h, yellow);
    fill_canvas_rect(8, 8, 32, 32, red);
    fill_canvas_rect(w - 40, 8, 32, 32, green);
    fill_canvas_rect(8, h - 40, 32, 32, blue);
    fill_canvas_rect(w - 40, h - 40, 32, 32, white);

    int bar_x = w / 9;
    int bar_w = (w - bar_x * 2) / 8;
    for (int i = 0; i < 8; i++) {
        fill_canvas_rect(bar_x + i * bar_w, h / 7, bar_w, h / 6, bars[i]);
    }
    fill_canvas_rect(bar_x, h / 7 + h / 6, bar_w * 8, 4, white);

    for (int i = 0; i < 16; i++) {
        uint8_t level = (uint8_t)(i * 255 / 15);
        fill_canvas_rect(bar_x + i * (bar_w / 2), h / 3, bar_w / 2, h / 12, display_core_rgb565(level, level, level));
    }

    fill_canvas_rect(w / 7, h * 5 / 8, w / 6, h / 7, red);
    fill_canvas_rect(w * 2 / 7, h * 5 / 8, w / 6, h / 7, green);
    fill_canvas_rect(w * 3 / 7, h * 5 / 8, w / 6, h / 7, blue);
    fill_canvas_rect(w * 4 / 7, h * 5 / 8, w / 6, h / 7, magenta);
    fill_canvas_rect(w / 7 + 10, h * 5 / 8 + 14, w / 6 - 20, h / 12, black);
    fill_canvas_rect(w * 2 / 7 + 10, h * 5 / 8 + 14, w / 6 - 20, h / 12, black);
    fill_canvas_rect(w * 3 / 7 + 10, h * 5 / 8 + 14, w / 6 - 20, h / 12, black);
    fill_canvas_rect(w * 4 / 7 + 10, h * 5 / 8 + 14, w / 6 - 20, h / 12, black);

    draw_test_text_center(18, "TOP", 3, black);
    draw_test_text(16, h / 2 - 10, "LEFT", 2, orange);
    draw_test_text(w - 80, h / 2 - 10, "RIGHT", 2, orange);
    draw_test_text_center(h - 32, "BOTTOM", 2, black);
    draw_test_text_center(h - 74, "LCD TEST", 2, white);
    draw_test_text_center(h - 54, "RGB565", 1, gray);
    draw_test_text(w / 7 + 14, h * 5 / 8 + 20, "R", 2, white);
    draw_test_text(w * 2 / 7 + 14, h * 5 / 8 + 20, "G", 2, white);
    draw_test_text(w * 3 / 7 + 14, h * 5 / 8 + 20, "B", 2, white);
    draw_test_text(w * 4 / 7 + 14, h * 5 / 8 + 20, "M", 2, white);

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
    fill_canvas_rect(w / 6, h / 3, w * 2 / 3, h / 12, black);
    fill_canvas_rect(w / 6, h / 2, w * 2 / 3, h / 12, black);
    fill_canvas_rect(w / 5, h / 3 + 6, w * 3 / 5, h / 20, cyan);
    fill_canvas_rect(w / 5, h / 2 + 6, w * 3 / 5, h / 20, magenta);

    (void)flush_canvas();
}

static void render_boot_test_pattern(void)
{
    if (CONFIG_ORNAMENT_LCD_REFERENCE_TEST_ONLY) {
        ESP_LOGI(TAG, "showing fixed LCD reference chart");
        render_lcd_reference_chart();
        while (true) {
            vTaskDelay(pdMS_TO_TICKS(60000));
        }
    }

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
        fill_canvas_color(colors[i]);
        vTaskDelay(color_delay_ticks);
    }
    render_lcd_diagnostic_pattern();
    vTaskDelay(pdMS_TO_TICKS(1000));
}

static void hide_xiaozhi_overlay_if_needed(void)
{
#if CONFIG_ORNAMENT_RICH_XIAOZHI_DISPLAY_ENABLED
    if (lvgl_display != NULL || lvgl_ready || lvgl_xiaozhi_visible) {
        lvgl_release_xiaozhi_overlay();
    }
#endif
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
    hide_xiaozhi_overlay_if_needed();
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

static void render_music(const ornament_state_t *state, const music_player_snapshot_t *snapshot)
{
    hide_xiaozhi_overlay_if_needed();
    if (!canvas_alloc()) {
        ESP_LOGE(TAG, "failed to allocate display canvas");
        return;
    }
    display_core_render_music(&canvas, state, snapshot);
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
    ESP_LOGI(TAG, "initializing display");
    display_log_config();
    display_set_backlight(false);

    esp_err_t err = display_init_panel_driver();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "display init failed: %s", esp_err_to_name(err));
        return;
    }

    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel_handle));
    if (strcmp(CONFIG_ORNAMENT_DISPLAY_DRIVER, "st7789_spi") == 0) {
        ESP_ERROR_CHECK(esp_lcd_panel_invert_color(panel_handle, true));
    }
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel_handle, true));
#if CONFIG_ORNAMENT_ST77916_DISPLAY_ENABLED
    if (strcmp(CONFIG_ORNAMENT_DISPLAY_DRIVER, "st77916_spi") == 0 ||
        strcmp(CONFIG_ORNAMENT_DISPLAY_DRIVER, "st77916_qspi") == 0) {
        ESP_ERROR_CHECK(esp_lcd_panel_swap_xy(panel_handle, CONFIG_ORNAMENT_LCD_SWAP_XY));
    }
#endif
    ESP_ERROR_CHECK(esp_lcd_panel_mirror(panel_handle, false, false));
    display_set_backlight(true);
    canvas_alloc();
    render_boot_test_pattern();
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
        "xiaozhi display: state=%s",
        snapshot != NULL ? xiaozhi_client_state_name(snapshot->state) : "none");
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

void display_render_music(const ornament_state_t *state, const music_player_snapshot_t *snapshot)
{
    ESP_LOGD(
        TAG,
        "music display: state=%s title=%s cover=%d",
        snapshot != NULL ? music_player_state_name(snapshot->state) : "none",
        snapshot != NULL ? snapshot->title : "",
        snapshot != NULL ? snapshot->has_cover : 0);
    render_music(state, snapshot);
}
