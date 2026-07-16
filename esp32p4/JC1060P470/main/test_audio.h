/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "test_common.h"

typedef enum {
    TEST_AUDIO_STATE_UNINITIALIZED = 0,
    TEST_AUDIO_STATE_IDLE,
    TEST_AUDIO_STATE_RECORDING,
    TEST_AUDIO_STATE_RECORDED,
    TEST_AUDIO_STATE_PLAYING,
    TEST_AUDIO_STATE_MUSIC_PLAYING,
    TEST_AUDIO_STATE_ERROR,
} test_audio_state_t;

typedef struct {
    test_module_meta_t module;
    bool initialized;
    bool running;
    test_audio_state_t state;
    size_t last_record_size;
    uint16_t last_record_peak;
    size_t last_record_nonzero_count;
    esp_err_t last_error;
} test_audio_status_t;

/**
 * @brief Initialize I2S peripheral, ES8311 codec through the board-owned
 *        shared I2C bus, and power amplifier.
 *
 * @return ESP_OK on success, otherwise an error code describing the failure.
 */
esp_err_t test_audio_init(void);

/**
 * @brief Start the audio backend task. The worker stays idle until the UI
 *        triggers one of the supported actions: play embedded canon.pcm,
 *        lazily allocate a PSRAM recording buffer, record audio, or replay
 *        the last recording.
 *
 * @return ESP_OK on success, otherwise an error code describing the failure.
 */
esp_err_t test_audio_run(void);

/**
 * @brief Start recording audio into the lazily allocated PSRAM buffer.
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
 * @brief Start playback of the embedded canon.pcm music asset.
 *
 * @return ESP_OK on success, otherwise an error code describing the failure.
 */
esp_err_t test_audio_start_music(void);

/**
 * @brief Get current audio test state.
 *
 * @return Current state of the audio module.
 */
test_audio_state_t test_audio_get_state(void);

/**
 * @brief Get a snapshot of the audio module state.
 *
 * @param out_status  Output status buffer.
 * @return ESP_OK on success, ESP_ERR_INVALID_ARG if out_status is NULL.
 */
esp_err_t test_audio_get_status(test_audio_status_t *out_status);

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
