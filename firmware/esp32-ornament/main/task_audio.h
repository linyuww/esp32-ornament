#pragma once

#include "esp_err.h"

#include <stddef.h>
#include <stdint.h>

#define ORNAMENT_AUDIO_SAMPLE_RATE_HZ 16000
#define TASK_AUDIO_MIC_PROBE_FRAMES 256

typedef struct {
    esp_err_t start_err;
    esp_err_t read_err;
    size_t frames_requested;
    size_t frames_captured;
    size_t nonzero_samples;
    int16_t min_sample;
    int16_t max_sample;
    uint32_t mean_abs_sample;
} task_audio_mic_probe_result_t;

esp_err_t task_audio_start(void);
void task_audio_play_done(void);
esp_err_t task_audio_output_acquire(void);
esp_err_t task_audio_output_write_mono(const int16_t *samples, size_t frame_count, uint32_t timeout_ms);
esp_err_t task_audio_output_write_silence(size_t frame_count, uint32_t timeout_ms);
void task_audio_output_release(void);
esp_err_t task_audio_input_start(void);
esp_err_t task_audio_input_read_mono(int16_t *samples, size_t frame_count, uint32_t timeout_ms);
void task_audio_input_stop(void);
esp_err_t task_audio_input_probe(task_audio_mic_probe_result_t *result, uint32_t timeout_ms);
esp_err_t task_audio_input_probe_with_tx_clock(task_audio_mic_probe_result_t *result, uint32_t timeout_ms);
