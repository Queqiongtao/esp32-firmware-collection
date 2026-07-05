/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#pragma once

#include "esp_err.h"

typedef enum {
    TEST_AUDIO_STATE_UNINITIALIZED = 0,
    TEST_AUDIO_STATE_IDLE,
    TEST_AUDIO_STATE_RECORDING,
    TEST_AUDIO_STATE_RECORDED,
    TEST_AUDIO_STATE_PLAYING,
    TEST_AUDIO_STATE_ERROR,
} test_audio_state_t;

/**
 * @brief Initialize I2S peripheral, ES8311 codec (via shared I2C bus), and
 *        power amplifier. The codec shares the touch I2C bus.
 *
 * @return ESP_OK on success, otherwise an error code describing the failure.
 */
esp_err_t test_audio_init(void);

/**
 * @brief Start the audio test backend. In music mode, a worker task plays the
 *        embedded canon.pcm in a loop. In echo mode, a worker task reads from
 *        microphone and writes to speaker. In record/playback mode, the module
 *        enters idle state and waits for UI-triggered actions.
 *
 * @return ESP_OK on success, otherwise an error code describing the failure.
 */
esp_err_t test_audio_run(void);

/**
 * @brief Start recording audio into the internal memory buffer.
 *
 * @return ESP_OK on success, otherwise an error code describing the failure.
 */
esp_err_t test_audio_start_record(void);

/**
 * @brief Start playback of the last successfully recorded audio buffer.
 *
 * @return ESP_OK on success, otherwise an error code describing the failure.
 */
esp_err_t test_audio_start_playback(void);

/**
 * @brief Get current audio test state.
 *
 * @return Current state of the audio module.
 */
test_audio_state_t test_audio_get_state(void);

/**
 * @brief Get byte count of the last successful recording.
 *
 * @return Recorded byte count.
 */
size_t test_audio_get_last_record_size(void);

/**
 * @brief Get the last error seen by the audio module.
 *
 * @return ESP_OK if no error is latched, otherwise the last error code.
 */
esp_err_t test_audio_get_last_error(void);

/**
 * @brief Get the peak absolute sample value of the last successful recording.
 *
 * @return Peak absolute PCM sample value.
 */
uint16_t test_audio_get_last_record_peak(void);

/**
 * @brief Get the number of non-zero PCM samples in the last successful recording.
 *
 * @return Count of non-zero 16-bit PCM samples.
 */
size_t test_audio_get_last_record_nonzero_count(void);

/**
 * @brief Stop the audio worker task, disable I2S channels, and release the
 *        codec. Safe to call when audio has not been initialized.
 *
 * @return ESP_OK on success, otherwise an error code describing the failure.
 */
esp_err_t test_audio_deinit(void);
