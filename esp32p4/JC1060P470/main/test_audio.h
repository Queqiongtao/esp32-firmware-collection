/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#pragma once

#include "esp_err.h"

/**
 * @brief Initialize I2S peripheral, ES8311 codec (via shared I2C bus), and
 *        power amplifier. The codec shares the touch I2C bus.
 *
 * @return ESP_OK on success, otherwise an error code describing the failure.
 */
esp_err_t test_audio_init(void);

/**
 * @brief Run the audio test. In music mode, plays the embedded canon.pcm in
 *        a loop. In echo mode, reads from microphone and writes to speaker.
 *
 * @return ESP_OK on success, otherwise an error code describing the failure.
 */
esp_err_t test_audio_run(void);

/**
 * @brief Stop audio playback/recording, disable I2S channels, and release
 *        the codec. Safe to call when audio has not been initialized.
 *
 * @return ESP_OK on success, otherwise an error code describing the failure.
 */
esp_err_t test_audio_deinit(void);
