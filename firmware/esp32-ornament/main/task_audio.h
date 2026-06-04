#pragma once

#include "esp_err.h"

#include <stddef.h>
#include <stdint.h>

#define ORNAMENT_AUDIO_SAMPLE_RATE_HZ 16000

esp_err_t task_audio_start(void);
void task_audio_play_done(void);
esp_err_t task_audio_output_acquire(void);
esp_err_t task_audio_output_write_mono(const int16_t *samples, size_t frame_count, uint32_t timeout_ms);
esp_err_t task_audio_output_write_silence(size_t frame_count, uint32_t timeout_ms);
void task_audio_output_release(void);
esp_err_t task_audio_input_start(void);
esp_err_t task_audio_input_read_mono(int16_t *samples, size_t frame_count, uint32_t timeout_ms);
void task_audio_input_stop(void);
