#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

typedef enum {
    AUDIO_SOUND_CLICK = 0,
    AUDIO_SOUND_CONFIRM,
    AUDIO_SOUND_ERROR,
    AUDIO_SOUND_CHIME,
    AUDIO_SOUND_TIMER_DONE,
    AUDIO_SOUND_ALARM_CHIME,
    AUDIO_SOUND_ALARM_BEEP,
    AUDIO_SOUND_ALARM_MELODY,
} audio_manager_sound_t;

esp_err_t audio_manager_init(void);
esp_err_t audio_manager_prepare(void);
void audio_manager_set_volume(uint8_t volume);
uint8_t audio_manager_get_volume(void);
bool audio_manager_is_playing(void);
bool audio_manager_is_noise_playing(void);
esp_err_t audio_manager_play(audio_manager_sound_t sound, bool loop);
esp_err_t audio_manager_play_alarm(uint8_t sound);
esp_err_t audio_manager_start_noise(bool brown, uint32_t duration_ms);
void audio_manager_stop(void);
