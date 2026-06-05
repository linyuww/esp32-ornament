#include "task_audio.h"

#include "driver/i2s_std.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "settings.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#ifndef CONFIG_ORNAMENT_XIAOZHI_MIC_PIN_DIN
#define CONFIG_ORNAMENT_XIAOZHI_MIC_PIN_DIN -1
#endif

#ifndef CONFIG_ORNAMENT_XIAOZHI_MIC_SLOT_RIGHT
#define CONFIG_ORNAMENT_XIAOZHI_MIC_SLOT_RIGHT 0
#endif

#if CONFIG_ORNAMENT_AUDIO_ENABLED

#define TASK_DONE_AUDIO_CHUNK_FRAMES 256
#define TASK_DONE_AUDIO_TAIL_SILENCE_FRAMES (ORNAMENT_AUDIO_SAMPLE_RATE_HZ / 20)

extern const uint8_t task_done_pcm_start[] asm("_binary_task_done_pcm_start");
extern const uint8_t task_done_pcm_end[] asm("_binary_task_done_pcm_end");

static const char *TAG = "task_audio";

static i2s_chan_handle_t s_tx_chan;
static i2s_chan_handle_t s_rx_chan;
static QueueHandle_t s_play_queue;
static SemaphoreHandle_t s_output_mutex;
static SemaphoreHandle_t s_input_mutex;
static bool s_output_enabled;
static bool s_input_enabled;
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

static esp_err_t write_stereo_frames(const int16_t *mono_samples, size_t frame_count, uint32_t timeout_ms)
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
        esp_err_t err = i2s_channel_write(s_tx_chan, stereo, bytes_to_write, &bytes_written, timeout_ms);
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
    if (task_audio_output_acquire() != ESP_OK) {
        ESP_LOGW(TAG, "task done voice skipped: audio output busy");
        return;
    }

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

    esp_err_t err = write_stereo_frames(samples, sample_count, 1000);
    if (err == ESP_OK) {
        err = write_stereo_frames(NULL, TASK_DONE_AUDIO_TAIL_SILENCE_FRAMES, 1000);
    }

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "task done audio write failed: %s", esp_err_to_name(err));
    }
    task_audio_output_release();
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
    ESP_RETURN_ON_ERROR(
        i2s_new_channel(
            &chan_cfg,
            &s_tx_chan,
            CONFIG_ORNAMENT_XIAOZHI_MIC_PIN_DIN >= 0 ? &s_rx_chan : NULL),
        TAG,
        "i2s_new_channel failed");

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(ORNAMENT_AUDIO_SAMPLE_RATE_HZ),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = CONFIG_ORNAMENT_AUDIO_PIN_BCLK,
            .ws = CONFIG_ORNAMENT_AUDIO_PIN_LRC,
            .dout = CONFIG_ORNAMENT_AUDIO_PIN_DIN,
            .din = CONFIG_ORNAMENT_XIAOZHI_MIC_PIN_DIN >= 0 ? CONFIG_ORNAMENT_XIAOZHI_MIC_PIN_DIN : I2S_GPIO_UNUSED,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };

    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_tx_chan, &std_cfg), TAG, "i2s std init failed");
    if (CONFIG_ORNAMENT_XIAOZHI_MIC_PIN_DIN >= 0) {
        ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_rx_chan, &std_cfg), TAG, "i2s rx std init failed");
    }
    ESP_LOGI(
        TAG,
        "I2S audio ready: bclk=GPIO%d lrc=GPIO%d speaker_din=GPIO%d mic_dout=GPIO%d sample_rate=%d volume=%d%%",
        CONFIG_ORNAMENT_AUDIO_PIN_BCLK,
        CONFIG_ORNAMENT_AUDIO_PIN_LRC,
        CONFIG_ORNAMENT_AUDIO_PIN_DIN,
        CONFIG_ORNAMENT_XIAOZHI_MIC_PIN_DIN,
        ORNAMENT_AUDIO_SAMPLE_RATE_HZ,
        CONFIG_ORNAMENT_AUDIO_VOLUME_PERCENT);
    return ESP_OK;
}

esp_err_t task_audio_start(void)
{
    if (s_play_queue != NULL) {
        return ESP_OK;
    }

    s_output_mutex = xSemaphoreCreateMutex();
    if (s_output_mutex == NULL) {
        return ESP_ERR_NO_MEM;
    }
    s_input_mutex = xSemaphoreCreateMutex();
    if (s_input_mutex == NULL) {
        return ESP_ERR_NO_MEM;
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

esp_err_t task_audio_output_acquire(void)
{
    if (s_output_mutex == NULL || s_tx_chan == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(s_output_mutex, pdMS_TO_TICKS(20)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    if (!s_output_enabled) {
        esp_err_t err = i2s_channel_enable(s_tx_chan);
        if (err != ESP_OK) {
            xSemaphoreGive(s_output_mutex);
            return err;
        }
        s_output_enabled = true;
    }
    return ESP_OK;
}

esp_err_t task_audio_output_write_mono(const int16_t *samples, size_t frame_count, uint32_t timeout_ms)
{
    if (samples == NULL || !s_output_enabled) {
        return ESP_ERR_INVALID_STATE;
    }
    return write_stereo_frames(samples, frame_count, timeout_ms);
}

esp_err_t task_audio_output_write_silence(size_t frame_count, uint32_t timeout_ms)
{
    if (!s_output_enabled) {
        return ESP_ERR_INVALID_STATE;
    }
    return write_stereo_frames(NULL, frame_count, timeout_ms);
}

void task_audio_output_release(void)
{
    if (s_output_mutex == NULL) {
        return;
    }
    if (s_output_enabled) {
        esp_err_t err = i2s_channel_disable(s_tx_chan);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "i2s tx disable failed: %s", esp_err_to_name(err));
        }
        s_output_enabled = false;
    }
    xSemaphoreGive(s_output_mutex);
}

esp_err_t task_audio_input_start(void)
{
    if (s_input_mutex == NULL || s_rx_chan == NULL || CONFIG_ORNAMENT_XIAOZHI_MIC_PIN_DIN < 0) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(s_input_mutex, pdMS_TO_TICKS(20)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    if (!s_input_enabled) {
        esp_err_t err = i2s_channel_enable(s_rx_chan);
        if (err != ESP_OK) {
            xSemaphoreGive(s_input_mutex);
            return err;
        }
        s_input_enabled = true;
    }
    return ESP_OK;
}

esp_err_t task_audio_input_read_mono(int16_t *samples, size_t frame_count, uint32_t timeout_ms)
{
    if (samples == NULL || !s_input_enabled) {
        return ESP_ERR_INVALID_STATE;
    }

    int16_t stereo[TASK_DONE_AUDIO_CHUNK_FRAMES * 2];
    size_t copied = 0;
    while (copied < frame_count) {
        size_t frames = frame_count - copied;
        if (frames > TASK_DONE_AUDIO_CHUNK_FRAMES) {
            frames = TASK_DONE_AUDIO_CHUNK_FRAMES;
        }
        size_t bytes_to_read = frames * 2 * sizeof(int16_t);
        size_t bytes_read = 0;
        esp_err_t err = i2s_channel_read(s_rx_chan, stereo, bytes_to_read, &bytes_read, timeout_ms);
        if (err != ESP_OK) {
            return err;
        }
        if (bytes_read != bytes_to_read) {
            return ESP_ERR_TIMEOUT;
        }
        for (size_t i = 0; i < frames; i++) {
            samples[copied + i] = stereo[i * 2 + (CONFIG_ORNAMENT_XIAOZHI_MIC_SLOT_RIGHT ? 1 : 0)];
        }
        copied += frames;
    }
    return ESP_OK;
}

void task_audio_input_stop(void)
{
    if (s_input_mutex == NULL) {
        return;
    }
    if (s_input_enabled) {
        esp_err_t err = i2s_channel_disable(s_rx_chan);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "i2s rx disable failed: %s", esp_err_to_name(err));
        }
        s_input_enabled = false;
    }
    xSemaphoreGive(s_input_mutex);
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

esp_err_t task_audio_output_acquire(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t task_audio_output_write_mono(const int16_t *samples, size_t frame_count, uint32_t timeout_ms)
{
    (void)samples;
    (void)frame_count;
    (void)timeout_ms;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t task_audio_output_write_silence(size_t frame_count, uint32_t timeout_ms)
{
    (void)frame_count;
    (void)timeout_ms;
    return ESP_ERR_NOT_SUPPORTED;
}

void task_audio_output_release(void)
{
}

esp_err_t task_audio_input_start(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t task_audio_input_read_mono(int16_t *samples, size_t frame_count, uint32_t timeout_ms)
{
    (void)samples;
    (void)frame_count;
    (void)timeout_ms;
    return ESP_ERR_NOT_SUPPORTED;
}

void task_audio_input_stop(void)
{
}

#endif
