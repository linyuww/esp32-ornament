#include "task_audio.h"

#include "driver/i2s_std.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "settings.h"

#include <stdint.h>
#include <string.h>

#if CONFIG_ORNAMENT_AUDIO_ENABLED

#define TASK_DONE_AUDIO_SAMPLE_RATE_HZ 16000
#define TASK_DONE_AUDIO_CHUNK_FRAMES 256
#define TASK_DONE_AUDIO_TAIL_SILENCE_FRAMES (TASK_DONE_AUDIO_SAMPLE_RATE_HZ / 20)

extern const uint8_t task_done_pcm_start[] asm("_binary_task_done_pcm_start");
extern const uint8_t task_done_pcm_end[] asm("_binary_task_done_pcm_end");

static const char *TAG = "task_audio";

static i2s_chan_handle_t s_tx_chan;
static QueueHandle_t s_play_queue;
static int s_play_volume_percent = CONFIG_ORNAMENT_AUDIO_VOLUME_PERCENT;

static int16_t apply_volume(int16_t sample)
{
    int32_t scaled = ((int32_t)sample * s_play_volume_percent) / 100;
    if (scaled > INT16_MAX) {
        return INT16_MAX;
    }
    if (scaled < INT16_MIN) {
        return INT16_MIN;
    }
    return (int16_t)scaled;
}

static esp_err_t write_stereo_frames(const int16_t *mono_samples, size_t frame_count)
{
    int16_t stereo[TASK_DONE_AUDIO_CHUNK_FRAMES * 2];

    while (frame_count > 0) {
        size_t frames = frame_count;
        if (frames > TASK_DONE_AUDIO_CHUNK_FRAMES) {
            frames = TASK_DONE_AUDIO_CHUNK_FRAMES;
        }

        for (size_t i = 0; i < frames; i++) {
            int16_t sample = mono_samples == NULL ? 0 : apply_volume(mono_samples[i]);
            stereo[i * 2] = sample;
            stereo[i * 2 + 1] = sample;
        }

        size_t bytes_to_write = frames * 2 * sizeof(int16_t);
        size_t bytes_written = 0;
        esp_err_t err = i2s_channel_write(s_tx_chan, stereo, bytes_to_write, &bytes_written, pdMS_TO_TICKS(1000));
        if (err != ESP_OK) {
            return err;
        }
        if (bytes_written != bytes_to_write) {
            ESP_LOGW(TAG, "short I2S write: %u/%u bytes", (unsigned)bytes_written, (unsigned)bytes_to_write);
            return ESP_ERR_TIMEOUT;
        }

        if (mono_samples != NULL) {
            mono_samples += frames;
        }
        frame_count -= frames;
    }

    return ESP_OK;
}

static void play_task_done_audio(void)
{
    ornament_settings_t settings;
    if (settings_load(&settings) == ESP_OK) {
        s_play_volume_percent = settings_audio_volume_percent_or_default(&settings);
    } else {
        s_play_volume_percent = CONFIG_ORNAMENT_AUDIO_VOLUME_PERCENT;
    }

    const size_t pcm_bytes = (size_t)(task_done_pcm_end - task_done_pcm_start);
    const size_t sample_count = pcm_bytes / sizeof(int16_t);
    const int16_t *samples = (const int16_t *)task_done_pcm_start;

    ESP_LOGI(
        TAG,
        "playing task done voice: %u bytes, %u samples, volume=%d%%",
        (unsigned)pcm_bytes,
        (unsigned)sample_count,
        s_play_volume_percent);

    esp_err_t err = i2s_channel_enable(s_tx_chan);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "i2s enable failed: %s", esp_err_to_name(err));
        return;
    }

    err = write_stereo_frames(samples, sample_count);
    if (err == ESP_OK) {
        err = write_stereo_frames(NULL, TASK_DONE_AUDIO_TAIL_SILENCE_FRAMES);
    }

    esp_err_t disable_err = i2s_channel_disable(s_tx_chan);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "task done audio write failed: %s", esp_err_to_name(err));
    }
    if (disable_err != ESP_OK) {
        ESP_LOGW(TAG, "i2s disable failed: %s", esp_err_to_name(disable_err));
    }
}

static void audio_task(void *arg)
{
    (void)arg;
    uint8_t request = 0;

    while (true) {
        if (xQueueReceive(s_play_queue, &request, portMAX_DELAY) == pdTRUE) {
            play_task_done_audio();
        }
    }
}

static esp_err_t init_i2s(void)
{
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num = 4;
    chan_cfg.dma_frame_num = TASK_DONE_AUDIO_CHUNK_FRAMES;
    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan_cfg, &s_tx_chan, NULL), TAG, "i2s_new_channel failed");

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(TASK_DONE_AUDIO_SAMPLE_RATE_HZ),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = CONFIG_ORNAMENT_AUDIO_PIN_BCLK,
            .ws = CONFIG_ORNAMENT_AUDIO_PIN_LRC,
            .dout = CONFIG_ORNAMENT_AUDIO_PIN_DIN,
            .din = I2S_GPIO_UNUSED,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };

    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_tx_chan, &std_cfg), TAG, "i2s std init failed");
    ESP_LOGI(
        TAG,
        "I2S audio ready: bclk=GPIO%d lrc=GPIO%d din=GPIO%d sample_rate=%d volume=%d%%",
        CONFIG_ORNAMENT_AUDIO_PIN_BCLK,
        CONFIG_ORNAMENT_AUDIO_PIN_LRC,
        CONFIG_ORNAMENT_AUDIO_PIN_DIN,
        TASK_DONE_AUDIO_SAMPLE_RATE_HZ,
        CONFIG_ORNAMENT_AUDIO_VOLUME_PERCENT);
    return ESP_OK;
}

esp_err_t task_audio_start(void)
{
    if (s_play_queue != NULL) {
        return ESP_OK;
    }

    ESP_RETURN_ON_ERROR(init_i2s(), TAG, "audio init failed");

    s_play_queue = xQueueCreate(1, sizeof(uint8_t));
    if (s_play_queue == NULL) {
        return ESP_ERR_NO_MEM;
    }

    BaseType_t created = xTaskCreate(audio_task, "task_audio", 4096, NULL, 4, NULL);
    if (created != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

void task_audio_play_done(void)
{
    if (s_play_queue == NULL) {
        return;
    }

    uint8_t request = 1;
    if (xQueueSend(s_play_queue, &request, 0) != pdTRUE) {
        (void)xQueueOverwrite(s_play_queue, &request);
    }
}

#else

static const char *TAG = "task_audio";

esp_err_t task_audio_start(void)
{
    ESP_LOGI(TAG, "task done audio disabled");
    return ESP_OK;
}

void task_audio_play_done(void)
{
}

#endif
