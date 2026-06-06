#include "task_audio.h"

#include "driver/i2s_std.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "settings.h"
#include "soc/soc_caps.h"

#include <limits.h>
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
#define TASK_AUDIO_OUTPUT_PORT I2S_NUM_0
#define TASK_AUDIO_INPUT_PORT I2S_NUM_1

extern const uint8_t task_done_pcm_start[] asm("_binary_task_done_pcm_start");
extern const uint8_t task_done_pcm_end[] asm("_binary_task_done_pcm_end");

static const char *TAG = "task_audio";

typedef enum {
    AUDIO_ROUTE_NONE = 0,
    AUDIO_ROUTE_TX,
    AUDIO_ROUTE_RX,
} audio_route_t;

static i2s_chan_handle_t s_tx_chan;
static i2s_chan_handle_t s_rx_chan;
static QueueHandle_t s_play_queue;
static SemaphoreHandle_t s_bus_mutex;
static SemaphoreHandle_t s_input_session_mutex;
static bool s_output_enabled;
static bool s_output_locked;
static bool s_input_session_active;
static bool s_rx_enabled;
static int s_play_volume_percent = CONFIG_ORNAMENT_AUDIO_VOLUME_PERCENT;
static audio_route_t s_active_route;
static int32_t s_mic_prev_input;
static int32_t s_mic_prev_output;
static i2s_std_gpio_config_t s_tx_gpio_active_cfg;
static i2s_std_gpio_config_t s_tx_gpio_idle_cfg;
static i2s_std_gpio_config_t s_rx_gpio_active_cfg;
static i2s_std_gpio_config_t s_rx_gpio_idle_cfg;

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

static int16_t narrow_mic_sample(int32_t raw)
{
    /*
     * INMP441 outputs 24-bit two's-complement I2S data.
     * On ESP32 I2S STD RX the payload is effectively left-aligned in the 32-bit
     * slot. The official Xiaozhi simplex path narrows microphone samples with
     * `>> 12`, which preserves more usable speech energy than `>> 16`.
     *
     * On this ESP32-S3 + INMP441 wiring, the captured stream also carries a
     * strong DC bias. Remove it with a lightweight single-pole high-pass
     * filter before clamping to 16-bit PCM, otherwise STT receives a waveform
     * pinned mostly on the positive half-axis.
     */
    int32_t narrowed = raw >> 12;
    int32_t filtered = narrowed - s_mic_prev_input + ((s_mic_prev_output * 255) / 256);
    s_mic_prev_input = narrowed;
    s_mic_prev_output = filtered;

    if (filtered > INT16_MAX) {
        filtered = INT16_MAX;
    } else if (filtered < INT16_MIN) {
        filtered = INT16_MIN;
    }
    return (int16_t)filtered;
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

static void refresh_output_volume_from_settings(const ornament_settings_t *settings)
{
    s_play_volume_percent = settings_audio_volume_percent_or_default(settings);
}

static esp_err_t disable_route_locked(audio_route_t route)
{
    if (route == AUDIO_ROUTE_TX && s_output_enabled) {
        ESP_RETURN_ON_ERROR(i2s_channel_disable(s_tx_chan), TAG, "disable tx route failed");
        ESP_RETURN_ON_ERROR(i2s_channel_reconfig_std_gpio(s_tx_chan, &s_tx_gpio_idle_cfg), TAG, "park tx gpio failed");
        s_output_enabled = false;
    } else if (route == AUDIO_ROUTE_RX && s_rx_enabled) {
        ESP_RETURN_ON_ERROR(i2s_channel_disable(s_rx_chan), TAG, "disable rx route failed");
        ESP_RETURN_ON_ERROR(i2s_channel_reconfig_std_gpio(s_rx_chan, &s_rx_gpio_idle_cfg), TAG, "park rx gpio failed");
        s_rx_enabled = false;
    }

    if (s_active_route == route) {
        s_active_route = AUDIO_ROUTE_NONE;
    }
    return ESP_OK;
}

static esp_err_t switch_route_locked(audio_route_t target)
{
    if (target == AUDIO_ROUTE_NONE) {
        return disable_route_locked(s_active_route);
    }

    if (target == AUDIO_ROUTE_TX) {
        if (s_active_route != AUDIO_ROUTE_TX) {
            ESP_RETURN_ON_ERROR(disable_route_locked(s_active_route), TAG, "drop previous route failed");
            ESP_LOGI(TAG, "switching audio route to TX");
            ESP_RETURN_ON_ERROR(i2s_channel_reconfig_std_gpio(s_tx_chan, &s_tx_gpio_active_cfg), TAG, "tx gpio switch failed");
            ESP_RETURN_ON_ERROR(i2s_channel_enable(s_tx_chan), TAG, "tx enable failed");
            s_output_enabled = true;
            s_active_route = AUDIO_ROUTE_TX;
        }
        return ESP_OK;
    }

    if (target == AUDIO_ROUTE_RX) {
        if (s_rx_chan == NULL) {
            return ESP_ERR_INVALID_STATE;
        }
        if (s_active_route != AUDIO_ROUTE_RX) {
            ESP_RETURN_ON_ERROR(disable_route_locked(s_active_route), TAG, "drop previous route failed");
            ESP_LOGI(TAG, "switching audio route to RX");
            ESP_RETURN_ON_ERROR(i2s_channel_reconfig_std_gpio(s_rx_chan, &s_rx_gpio_active_cfg), TAG, "rx gpio switch failed");
            ESP_RETURN_ON_ERROR(i2s_channel_enable(s_rx_chan), TAG, "rx enable failed");
            s_rx_enabled = true;
            s_active_route = AUDIO_ROUTE_RX;
        }
        return ESP_OK;
    }

    return ESP_ERR_INVALID_ARG;
}

static void play_task_done_audio(void)
{
    ornament_settings_t settings;
    if (settings_load(&settings) == ESP_OK) {
        refresh_output_volume_from_settings(&settings);
    } else {
        s_play_volume_percent = CONFIG_ORNAMENT_AUDIO_VOLUME_PERCENT;
    }

    if (task_audio_output_acquire() != ESP_OK) {
        ESP_LOGW(TAG, "task done voice skipped: audio output busy");
        return;
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
    i2s_chan_config_t tx_chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(TASK_AUDIO_OUTPUT_PORT, I2S_ROLE_MASTER);
    tx_chan_cfg.dma_desc_num = 4;
    tx_chan_cfg.dma_frame_num = TASK_DONE_AUDIO_CHUNK_FRAMES;
    ESP_RETURN_ON_ERROR(i2s_new_channel(&tx_chan_cfg, &s_tx_chan, NULL), TAG, "alloc tx channel failed");

    s_tx_gpio_active_cfg = (i2s_std_gpio_config_t) {
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
    };
    s_tx_gpio_idle_cfg = s_tx_gpio_active_cfg;
    s_tx_gpio_idle_cfg.bclk = I2S_GPIO_UNUSED;
    s_tx_gpio_idle_cfg.ws = I2S_GPIO_UNUSED;
    s_tx_gpio_idle_cfg.dout = I2S_GPIO_UNUSED;

    i2s_std_config_t tx_std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(ORNAMENT_AUDIO_SAMPLE_RATE_HZ),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = s_tx_gpio_idle_cfg,
    };
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_tx_chan, &tx_std_cfg), TAG, "init tx channel failed");

    if (CONFIG_ORNAMENT_XIAOZHI_MIC_PIN_DIN >= 0) {
#if SOC_I2S_NUM < 2
        ESP_LOGE(TAG, "ESP32 target only exposes one I2S controller, mic simplex path unavailable");
        return ESP_ERR_NOT_SUPPORTED;
#else
        i2s_chan_config_t rx_chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(TASK_AUDIO_INPUT_PORT, I2S_ROLE_MASTER);
        rx_chan_cfg.dma_desc_num = 4;
        rx_chan_cfg.dma_frame_num = TASK_DONE_AUDIO_CHUNK_FRAMES;
        ESP_RETURN_ON_ERROR(i2s_new_channel(&rx_chan_cfg, NULL, &s_rx_chan), TAG, "alloc rx channel failed");

        i2s_std_slot_config_t rx_slot_cfg =
            I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_MONO);
        rx_slot_cfg.slot_mask = CONFIG_ORNAMENT_XIAOZHI_MIC_SLOT_RIGHT ? I2S_STD_SLOT_RIGHT : I2S_STD_SLOT_LEFT;

        s_rx_gpio_active_cfg = (i2s_std_gpio_config_t) {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = CONFIG_ORNAMENT_AUDIO_PIN_BCLK,
            .ws = CONFIG_ORNAMENT_AUDIO_PIN_LRC,
            .dout = I2S_GPIO_UNUSED,
            .din = CONFIG_ORNAMENT_XIAOZHI_MIC_PIN_DIN,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        };
        s_rx_gpio_idle_cfg = s_rx_gpio_active_cfg;
        s_rx_gpio_idle_cfg.bclk = I2S_GPIO_UNUSED;
        s_rx_gpio_idle_cfg.ws = I2S_GPIO_UNUSED;

        i2s_std_config_t rx_std_cfg = {
            .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(ORNAMENT_AUDIO_SAMPLE_RATE_HZ),
            .slot_cfg = rx_slot_cfg,
            .gpio_cfg = s_rx_gpio_idle_cfg,
        };
        ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_rx_chan, &rx_std_cfg), TAG, "init rx channel failed");
#endif
    }

    ESP_LOGI(
        TAG,
        "I2S audio ready: tx_port=%d rx_port=%d bclk=GPIO%d lrc=GPIO%d speaker_din=GPIO%d mic_dout=GPIO%d mic_slot=%s mic_bits=32 sample_rate=%d volume=%d%%",
        TASK_AUDIO_OUTPUT_PORT,
        CONFIG_ORNAMENT_XIAOZHI_MIC_PIN_DIN >= 0 ? TASK_AUDIO_INPUT_PORT : -1,
        CONFIG_ORNAMENT_AUDIO_PIN_BCLK,
        CONFIG_ORNAMENT_AUDIO_PIN_LRC,
        CONFIG_ORNAMENT_AUDIO_PIN_DIN,
        CONFIG_ORNAMENT_XIAOZHI_MIC_PIN_DIN,
        CONFIG_ORNAMENT_XIAOZHI_MIC_SLOT_RIGHT ? "right" : "left",
        ORNAMENT_AUDIO_SAMPLE_RATE_HZ,
        CONFIG_ORNAMENT_AUDIO_VOLUME_PERCENT);
    return ESP_OK;
}

esp_err_t task_audio_start(void)
{
    if (s_play_queue != NULL) {
        return ESP_OK;
    }

    s_bus_mutex = xSemaphoreCreateMutex();
    if (s_bus_mutex == NULL) {
        return ESP_ERR_NO_MEM;
    }
    s_input_session_mutex = xSemaphoreCreateMutex();
    if (s_input_session_mutex == NULL) {
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
    return task_audio_output_acquire_with_volume(NULL);
}

esp_err_t task_audio_output_acquire_with_volume(const ornament_settings_t *settings)
{
    if (s_bus_mutex == NULL || s_tx_chan == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(s_bus_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    if (settings != NULL) {
        refresh_output_volume_from_settings(settings);
    } else {
        ornament_settings_t loaded;
        if (settings_load(&loaded) == ESP_OK) {
            refresh_output_volume_from_settings(&loaded);
        } else {
            s_play_volume_percent = CONFIG_ORNAMENT_AUDIO_VOLUME_PERCENT;
        }
    }
    esp_err_t err = switch_route_locked(AUDIO_ROUTE_TX);
    if (err != ESP_OK) {
        xSemaphoreGive(s_bus_mutex);
        return err;
    }
    s_output_locked = true;
    return ESP_OK;
}

esp_err_t task_audio_output_write_mono(const int16_t *samples, size_t frame_count, uint32_t timeout_ms)
{
    if (samples == NULL || !s_output_enabled || !s_output_locked) {
        return ESP_ERR_INVALID_STATE;
    }
    return write_stereo_frames(samples, frame_count, timeout_ms);
}

esp_err_t task_audio_output_write_silence(size_t frame_count, uint32_t timeout_ms)
{
    if (!s_output_enabled || !s_output_locked) {
        return ESP_ERR_INVALID_STATE;
    }
    return write_stereo_frames(NULL, frame_count, timeout_ms);
}

void task_audio_output_release(void)
{
    if (s_bus_mutex == NULL || !s_output_locked) {
        return;
    }
    esp_err_t err = disable_route_locked(AUDIO_ROUTE_TX);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "i2s tx disable failed: %s", esp_err_to_name(err));
    }
    s_output_locked = false;
    xSemaphoreGive(s_bus_mutex);
}

esp_err_t task_audio_input_start(void)
{
    if (s_input_session_mutex == NULL || s_rx_chan == NULL || CONFIG_ORNAMENT_XIAOZHI_MIC_PIN_DIN < 0) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(s_input_session_mutex, pdMS_TO_TICKS(20)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    s_mic_prev_input = 0;
    s_mic_prev_output = 0;
    s_input_session_active = true;
    return ESP_OK;
}

esp_err_t task_audio_input_read_mono(int16_t *samples, size_t frame_count, uint32_t timeout_ms)
{
    if (samples == NULL || !s_input_session_active || s_bus_mutex == NULL || s_rx_chan == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(s_bus_mutex, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    esp_err_t err = switch_route_locked(AUDIO_ROUTE_RX);
    if (err != ESP_OK) {
        xSemaphoreGive(s_bus_mutex);
        return err;
    }

    int32_t mono[TASK_DONE_AUDIO_CHUNK_FRAMES];
    size_t copied = 0;
    while (copied < frame_count) {
        size_t frames = frame_count - copied;
        if (frames > TASK_DONE_AUDIO_CHUNK_FRAMES) {
            frames = TASK_DONE_AUDIO_CHUNK_FRAMES;
        }

        size_t bytes_to_read = frames * sizeof(int32_t);
        size_t bytes_read = 0;
        err = i2s_channel_read(s_rx_chan, mono, bytes_to_read, &bytes_read, timeout_ms);
        if (err != ESP_OK) {
            break;
        }
        if (bytes_read != bytes_to_read) {
            err = ESP_ERR_TIMEOUT;
            break;
        }

        for (size_t i = 0; i < frames; i++) {
            samples[copied + i] = narrow_mic_sample(mono[i]);
        }
        copied += frames;
    }

    xSemaphoreGive(s_bus_mutex);
    return err;
}

void task_audio_input_stop(void)
{
    if (s_input_session_mutex == NULL || !s_input_session_active) {
        return;
    }
    s_input_session_active = false;
    if (s_bus_mutex != NULL && xSemaphoreTake(s_bus_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        if (s_active_route == AUDIO_ROUTE_RX) {
            esp_err_t err = disable_route_locked(AUDIO_ROUTE_RX);
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "park rx after session failed: %s", esp_err_to_name(err));
            }
        }
        xSemaphoreGive(s_bus_mutex);
    }
    xSemaphoreGive(s_input_session_mutex);
}

static esp_err_t probe_input_internal(task_audio_mic_probe_result_t *result, uint32_t timeout_ms, bool hold_tx_clock)
{
    if (result == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    (void)hold_tx_clock;

    memset(result, 0, sizeof(*result));
    result->start_err = ESP_FAIL;
    result->read_err = ESP_FAIL;
    result->frames_requested = TASK_AUDIO_MIC_PROBE_FRAMES;
    result->min_sample = INT16_MAX;
    result->max_sample = INT16_MIN;

    int16_t samples[TASK_AUDIO_MIC_PROBE_FRAMES] = {0};

    esp_err_t err = task_audio_input_start();
    result->start_err = err;
    if (err != ESP_OK) {
        result->read_err = err;
        return err;
    }

    err = task_audio_input_read_mono(samples, TASK_AUDIO_MIC_PROBE_FRAMES, timeout_ms);
    result->read_err = err;
    if (err == ESP_OK) {
        uint32_t abs_acc = 0;
        int32_t sum = 0;
        int16_t prev = 0;
        bool prev_valid = false;
        for (size_t i = 0; i < TASK_AUDIO_MIC_PROBE_FRAMES; i++) {
            int16_t sample = samples[i];
            if (sample != 0) {
                result->nonzero_samples++;
            }
            if (sample > 0) {
                result->positive_samples++;
            } else if (sample < 0) {
                result->negative_samples++;
            }
            if (sample == INT16_MAX || sample == INT16_MIN) {
                result->saturated_samples++;
            }
            if (sample < result->min_sample) {
                result->min_sample = sample;
            }
            if (sample > result->max_sample) {
                result->max_sample = sample;
            }
            if (prev_valid &&
                ((prev < 0 && sample > 0) || (prev > 0 && sample < 0))) {
                result->zero_crossings++;
            }
            prev = sample;
            prev_valid = sample != 0;
            sum += sample;
            abs_acc += (uint32_t)(sample < 0 ? -(int32_t)sample : sample);
        }
        result->frames_captured = TASK_AUDIO_MIC_PROBE_FRAMES;
        result->mean_sample = sum / (int32_t)TASK_AUDIO_MIC_PROBE_FRAMES;
        result->mean_abs_sample = abs_acc / TASK_AUDIO_MIC_PROBE_FRAMES;
    } else {
        result->min_sample = 0;
        result->max_sample = 0;
    }

    task_audio_input_stop();
    return err;
}

esp_err_t task_audio_input_probe(task_audio_mic_probe_result_t *result, uint32_t timeout_ms)
{
    return probe_input_internal(result, timeout_ms, false);
}

esp_err_t task_audio_input_probe_with_tx_clock(task_audio_mic_probe_result_t *result, uint32_t timeout_ms)
{
    return probe_input_internal(result, timeout_ms, true);
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

esp_err_t task_audio_input_probe(task_audio_mic_probe_result_t *result, uint32_t timeout_ms)
{
    (void)result;
    (void)timeout_ms;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t task_audio_input_probe_with_tx_clock(task_audio_mic_probe_result_t *result, uint32_t timeout_ms)
{
    (void)result;
    (void)timeout_ms;
    return ESP_ERR_NOT_SUPPORTED;
}

#endif
