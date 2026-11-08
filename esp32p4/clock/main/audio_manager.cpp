#include "audio_manager.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "bsp/esp-bsp.h"
#include "esp_codec_dev.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

static const char *TAG = "AudioManager";

namespace {

constexpr uint32_t kSampleRate = 22050;
constexpr size_t kFramesPerChunk = 512;
constexpr uint32_t kAttackFrames = kSampleRate * 8 / 1000;
constexpr uint32_t kReleaseFrames = kSampleRate * 14 / 1000;
constexpr uint32_t kAlarmRampMs = 30000;
constexpr uint8_t kAlarmMinVolume = 70;

enum class Timbre : uint8_t {
    Sine,
    Bell,
    Triangle,
    Square,
    Click,
    Swish,
};

struct Note {
    uint16_t frequency;
    uint16_t duration_ms;
    uint8_t level;
    Timbre timbre;
    uint8_t attack_ms;
    uint8_t release_ms;
    uint8_t decay;
};

constexpr Note kClick[] = {
    {2200, 45, 55, Timbre::Click, 1, 8, 90},
};

constexpr Note kConfirm[] = {
    {1046, 90, 58, Timbre::Bell, 5, 30, 55},
    {1568, 220, 52, Timbre::Bell, 5, 60, 70},
};

constexpr Note kError[] = {
    {196, 130, 58, Timbre::Triangle, 8, 30, 25},
    {147, 240, 58, Timbre::Triangle, 8, 60, 40},
};

constexpr Note kMenuOpen[] = {
    {440, 80, 42, Timbre::Triangle, 4, 20, 20},
    {660, 140, 40, Timbre::Bell, 6, 40, 45},
};

constexpr Note kSwipe[] = {
    {1200, 70, 28, Timbre::Swish, 4, 30, 70},
};

constexpr Note kWake[] = {
    {523, 140, 48, Timbre::Bell, 18, 40, 35},
    {659, 140, 48, Timbre::Bell, 18, 40, 40},
    {784, 160, 48, Timbre::Bell, 18, 45, 45},
    {1046, 360, 45, Timbre::Bell, 20, 80, 55},
};

constexpr Note kSleep[] = {
    {784, 180, 48, Timbre::Bell, 10, 50, 40},
    {659, 220, 46, Timbre::Bell, 10, 60, 45},
    {523, 420, 44, Timbre::Bell, 12, 120, 55},
};

constexpr Note kTestTone[] = {
    {1000, 1000, 80, Timbre::Sine, 5, 20, 0},
};

constexpr Note kChime[] = {
    {523, 340, 48, Timbre::Bell, 10, 80, 40},
    {659, 340, 46, Timbre::Bell, 10, 80, 42},
    {784, 380, 44, Timbre::Bell, 10, 90, 45},
    {1046, 520, 42, Timbre::Bell, 10, 120, 50},
};

constexpr Note kTimerDone[] = {
    {1318, 150, 55, Timbre::Triangle, 3, 20, 20},
    {1568, 150, 55, Timbre::Triangle, 3, 20, 20},
    {2093, 300, 50, Timbre::Bell, 5, 50, 55},
    {0, 140, 0, Timbre::Sine, 0, 0, 0},
    {1318, 150, 55, Timbre::Triangle, 3, 20, 20},
    {1568, 150, 55, Timbre::Triangle, 3, 20, 20},
    {2093, 360, 50, Timbre::Bell, 5, 50, 55},
};

constexpr Note kAlarmChime[] = {
    {880, 260, 58, Timbre::Bell, 5, 60, 50},
    {0, 80, 0, Timbre::Sine, 0, 0, 0},
    {880, 200, 45, Timbre::Bell, 5, 40, 60},
    {0, 450, 0, Timbre::Sine, 0, 0, 0},
    {1108, 260, 58, Timbre::Bell, 5, 60, 50},
    {0, 80, 0, Timbre::Sine, 0, 0, 0},
    {1108, 200, 45, Timbre::Bell, 5, 40, 60},
    {0, 450, 0, Timbre::Sine, 0, 0, 0},
    {1320, 300, 58, Timbre::Bell, 5, 60, 50},
    {0, 80, 0, Timbre::Sine, 0, 0, 0},
    {1320, 220, 45, Timbre::Bell, 5, 40, 60},
    {0, 850, 0, Timbre::Sine, 0, 0, 0},
};

constexpr Note kAlarmBeep[] = {
    {1800, 90, 65, Timbre::Square, 2, 10, 0},
    {0, 90, 0, Timbre::Sine, 0, 0, 0},
    {1800, 90, 65, Timbre::Square, 2, 10, 0},
    {0, 90, 0, Timbre::Sine, 0, 0, 0},
    {1800, 90, 65, Timbre::Square, 2, 10, 0},
    {0, 700, 0, Timbre::Sine, 0, 0, 0},
};

constexpr Note kAlarmMelody[] = {
    {523, 180, 50, Timbre::Bell, 5, 40, 45},
    {659, 180, 50, Timbre::Bell, 5, 40, 45},
    {784, 180, 50, Timbre::Bell, 5, 40, 45},
    {1046, 380, 52, Timbre::Bell, 5, 60, 55},
    {0, 200, 0, Timbre::Sine, 0, 0, 0},
    {880, 200, 48, Timbre::Bell, 5, 40, 50},
    {784, 200, 48, Timbre::Bell, 5, 40, 50},
    {659, 380, 48, Timbre::Bell, 5, 60, 55},
    {0, 700, 0, Timbre::Sine, 0, 0, 0},
};

enum class CommandType : uint8_t {
    Play,
    Alarm,
    Noise,
    Stop,
};

struct Command {
    CommandType type;
    uint8_t sound;
    bool loop;
    bool brown;
    uint32_t duration_ms;
};

QueueHandle_t s_queue = nullptr;
TaskHandle_t s_task = nullptr;
esp_codec_dev_handle_t s_codec = nullptr;

int16_t s_samples[kFramesPerChunk] = {};
uint32_t s_sample_rate = kSampleRate;
volatile uint8_t s_volume = 30;
volatile bool s_volume_dirty = false;
volatile bool s_playing = false;
volatile bool s_noise_playing = false;
volatile bool s_stop_requested = false;
volatile uint8_t s_active_volume = 0;
bool s_codec_open = false;
bool s_active_alarm = false;
Command s_pending = {};
bool s_has_pending = false;

int clamp_volume(int volume)
{
    return std::max(0, std::min(100, volume));
}

bool ensure_codec(int volume)
{
    if (s_codec == nullptr) {
        s_codec = bsp_audio_codec_speaker_init();
        if (s_codec == nullptr) {
            ESP_LOGE(TAG, "Speaker codec init failed");
            return false;
        }
    }

    if (!s_codec_open) {
        esp_codec_dev_sample_info_t format = {
            .bits_per_sample = 16,
            .channel = 1,
            .channel_mask = 0,
            .sample_rate = s_sample_rate,
            .mclk_multiple = 256,
        };
        const int err = esp_codec_dev_open(s_codec, &format);
        if (err != ESP_CODEC_DEV_OK) {
            ESP_LOGE(TAG, "Codec open failed: %d", err);
            return false;
        }
        s_codec_open = true;
    }

    esp_codec_dev_set_out_vol(s_codec, clamp_volume(volume));
    esp_codec_dev_set_out_mute(s_codec, false);
    return true;
}

void close_codec(void)
{
    if (s_codec != nullptr && s_codec_open) {
        esp_codec_dev_set_out_mute(s_codec, true);
        esp_codec_dev_close(s_codec);
        s_codec_open = false;
    }
}

bool write_samples(size_t frames)
{
    if (s_codec == nullptr || !s_codec_open) {
        return false;
    }

    if (s_volume_dirty) {
        const int volume = s_active_volume != 0
                               ? s_active_volume
                               : (s_active_alarm ? std::max<int>(s_volume, kAlarmMinVolume)
                                                 : s_volume);
        esp_codec_dev_set_out_vol(s_codec, clamp_volume(volume));
        s_volume_dirty = false;
    }

    const int bytes = (int)(frames * sizeof(int16_t));
    const int err = esp_codec_dev_write(s_codec, s_samples, bytes);
    if (err != ESP_CODEC_DEV_OK) {
        ESP_LOGW(TAG, "Codec write failed: %d", err);
        return false;
    }
    return true;
}

bool poll_control(void)
{
    if (s_stop_requested) {
        return true;
    }

    Command command = {};
    if (xQueueReceive(s_queue, &command, 0) != pdTRUE) {
        return false;
    }

    if (command.type == CommandType::Stop) {
        s_stop_requested = true;
        return true;
    }

    s_pending = command;
    s_has_pending = true;
    return true;
}

uint32_t frame_count(uint32_t duration_ms)
{
    return (uint32_t)((uint64_t)duration_ms * s_sample_rate / 1000ULL);
}

void fill_silence(size_t frames)
{
    std::memset(s_samples, 0, frames * sizeof(int16_t));
}

bool write_silence(uint32_t frames)
{
    while (frames > 0) {
        const size_t chunk = std::min<size_t>(kFramesPerChunk, frames);
        fill_silence(chunk);
        if (!write_samples(chunk)) {
            return false;
        }
        frames -= (uint32_t)chunk;
    }
    return true;
}

void fill_note(size_t frames,
               const Note &note,
               uint32_t note_frame,
               uint32_t note_frames,
               float ramp_gain,
               uint32_t &phase,
               uint32_t phase_step)
{
    const float base = (float)note.level / 100.0f;
    const uint32_t attack_frames =
        std::max<uint32_t>(1, (uint32_t)note.attack_ms * s_sample_rate / 1000U);
    const uint32_t release_frames =
        std::max<uint32_t>(1, (uint32_t)note.release_ms * s_sample_rate / 1000U);
    const float duration_s =
        std::max(0.001f, (float)note_frames / (float)s_sample_rate);
    const float decay_rate =
        note.decay > 0 ? ((float)note.decay / 100.0f * 8.0f / duration_s) : 0.0f;

    for (size_t i = 0; i < frames; ++i) {
        const uint32_t frame = note_frame + (uint32_t)i;
        float envelope = 1.0f;
        if (frame < attack_frames) {
            envelope = (float)frame / (float)attack_frames;
        }
        const uint32_t remaining = note_frames > frame ? note_frames - frame : 0;
        if (remaining < release_frames) {
            envelope = std::min(envelope, (float)remaining / (float)release_frames);
        }
        if (decay_rate > 0.0f) {
            envelope *= std::exp(-decay_rate * (float)frame / (float)s_sample_rate);
        }

        float sample = 0.0f;
        if (note.frequency != 0) {
            const float phase_radians =
                (float)((double)phase * 2.0 * M_PI / 4294967296.0);
            const float sine = std::sin(phase_radians);
            switch (note.timbre) {
            case Timbre::Bell:
                sample = (sine +
                          0.35f * std::sin(phase_radians * 2.0f) +
                          0.16f * std::sin(phase_radians * 3.0f) +
                          0.18f * std::sin(phase_radians * 1.003f)) /
                         1.69f;
                break;
            case Timbre::Triangle:
                sample = (2.0f / (float)M_PI) * std::asin(std::max(-1.0f, std::min(1.0f, sine)));
                break;
            case Timbre::Square:
                sample = std::tanh(2.2f * sine);
                break;
            case Timbre::Click: {
                const float noise =
                    ((int32_t)(esp_random() & 0xFFFFU) - 32768) / 32768.0f;
                const float transient =
                    noise * std::exp(-(float)frame / (float)(s_sample_rate * 0.004f));
                sample = sine * 0.55f + transient * 0.45f;
                break;
            }
            case Timbre::Swish: {
                const float noise =
                    ((int32_t)(esp_random() & 0xFFFFU) - 32768) / 32768.0f;
                sample = noise * 0.9f;
                break;
            }
            case Timbre::Sine:
            default:
                sample = sine;
                break;
            }
            sample *= base * envelope * ramp_gain;
            phase += phase_step;
        }

        sample = std::max(-1.0f, std::min(1.0f, sample));
        s_samples[i] = (int16_t)std::lround(sample * 32767.0f * 0.8f);
    }
}

bool play_notes(const Note *notes,
                size_t count,
                bool loop,
                bool alarm,
                uint8_t volume_override = 0)
{
    const bool was_open = s_codec_open;
    const int codec_volume = volume_override != 0
                                 ? volume_override
                                 : (alarm ? std::max<int>(s_volume, kAlarmMinVolume) : s_volume);
    if (!ensure_codec(codec_volume)) {
        return false;
    }

    s_active_alarm = alarm;
    s_active_volume = volume_override;
    s_volume_dirty = true;
    s_playing = true;
    s_noise_playing = false;
    ESP_LOGI(TAG,
             "play notes: count=%u loop=%d alarm=%d volume=%u",
             (unsigned)count,
             loop ? 1 : 0,
             alarm ? 1 : 0,
             (unsigned)codec_volume);

    if (!was_open) {
        if (!write_silence(frame_count(150))) {
            return false;
        }
    }

    const uint64_t start_us = esp_timer_get_time();
    do {
        for (size_t n = 0; n < count; ++n) {
            const Note &note = notes[n];
            const uint32_t frames = frame_count(note.duration_ms);
            const uint32_t phase_step =
                note.frequency == 0
                    ? 0
                    : (uint32_t)((uint64_t)note.frequency * 4294967296ULL / s_sample_rate);
            uint32_t phase = 0;

            for (uint32_t done = 0; done < frames;) {
                if (poll_control()) {
                    return false;
                }
                const size_t chunk =
                    std::min<size_t>(kFramesPerChunk, (size_t)(frames - done));
                float ramp_gain = 1.0f;
                if (alarm) {
                    const uint64_t elapsed_ms = (esp_timer_get_time() - start_us) / 1000ULL;
                    const float progress =
                        std::min(1.0f, (float)elapsed_ms / (float)kAlarmRampMs);
                    ramp_gain = 0.2f + 0.8f * progress;
                }
                fill_note(chunk,
                          note,
                          done,
                          frames,
                          ramp_gain,
                          phase,
                          phase_step);
                if (!write_samples(chunk)) {
                    return false;
                }
                done += (uint32_t)chunk;
            }
        }

        if (loop) {
            const uint32_t rest_frames = frame_count(700);
            for (uint32_t done = 0; done < rest_frames;) {
                if (poll_control()) {
                    return false;
                }
                const size_t chunk =
                    std::min<size_t>(kFramesPerChunk, (size_t)(rest_frames - done));
                fill_silence(chunk);
                if (!write_samples(chunk)) {
                    return false;
                }
                done += (uint32_t)chunk;
            }
        }
    } while (loop);

    return true;
}

bool play_noise(bool brown, uint32_t duration_ms)
{
    const bool was_open = s_codec_open;
    if (!ensure_codec(s_volume)) {
        return false;
    }

    s_active_alarm = false;
    s_active_volume = 0;
    s_volume_dirty = true;
    s_playing = true;
    s_noise_playing = true;

    if (!was_open) {
        if (!write_silence(frame_count(150))) {
            return false;
        }
    }

    if (duration_ms == 0) {
        duration_ms = 60000;
    }

    const uint32_t total_frames = frame_count(duration_ms);
    const uint32_t fade_frames = s_sample_rate;
    uint32_t elapsed = 0;
    int32_t brown_state = 0;

    while (elapsed < total_frames) {
        if (poll_control()) {
            break;
        }

        const size_t chunk =
            std::min<size_t>(kFramesPerChunk, (size_t)(total_frames - elapsed));
        float fade = 1.0f;
        if (elapsed < fade_frames) {
            fade = (float)elapsed / (float)fade_frames;
        }
        const uint32_t remaining = total_frames - elapsed;
        if (remaining < fade_frames) {
            fade = std::min(fade, (float)remaining / (float)fade_frames);
        }

        for (size_t i = 0; i < chunk; ++i) {
            const int32_t white = (int32_t)(esp_random() & 0xFFFFU) - 32768;
            int32_t value = 0;
            if (brown) {
                brown_state += white / 8;
                brown_state = (brown_state * 255) / 256;
                brown_state = std::max<int32_t>(-32768, std::min<int32_t>(32767, brown_state));
                value = (int32_t)(brown_state * 0.42f * fade);
            } else {
                value = (int32_t)(white * 0.22f * fade);
            }
            s_samples[i] = (int16_t)std::max<int32_t>(-32768, std::min<int32_t>(32767, value));
        }

        if (!write_samples(chunk)) {
            break;
        }
        elapsed += (uint32_t)chunk;
    }

    return true;
}

const Note *pattern_for_sound(uint8_t sound, size_t *count)
{
    switch (sound) {
    case 0:
        *count = sizeof(kAlarmChime) / sizeof(kAlarmChime[0]);
        return kAlarmChime;
    case 1:
        *count = sizeof(kAlarmBeep) / sizeof(kAlarmBeep[0]);
        return kAlarmBeep;
    case 2:
    default:
        *count = sizeof(kAlarmMelody) / sizeof(kAlarmMelody[0]);
        return kAlarmMelody;
    }
}

void audio_task(void *arg)
{
    (void)arg;
    Command command = {};
    bool have_command = false;

    for (;;) {
        if (!have_command) {
            if (xQueueReceive(s_queue, &command, portMAX_DELAY) != pdTRUE) {
                continue;
            }
        }
        have_command = false;

        if (command.type == CommandType::Stop) {
            s_stop_requested = false;
            s_playing = false;
            s_noise_playing = false;
            s_active_alarm = false;
            close_codec();
            continue;
        }

        s_stop_requested = false;
        ESP_LOGI(TAG, "command type=%u sound=%u", (unsigned)command.type, command.sound);
        switch (command.type) {
        case CommandType::Play:
            if (command.sound == AUDIO_SOUND_TEST) {
                play_notes(kTestTone,
                           sizeof(kTestTone) / sizeof(kTestTone[0]),
                           false,
                           false,
                           80);
            } else if (command.sound == AUDIO_SOUND_WAKE) {
                play_notes(kWake, sizeof(kWake) / sizeof(kWake[0]), false, false);
            } else if (command.sound == AUDIO_SOUND_SLEEP) {
                play_notes(kSleep, sizeof(kSleep) / sizeof(kSleep[0]), false, false);
            } else if (command.sound == AUDIO_SOUND_CLICK) {
                play_notes(kClick, sizeof(kClick) / sizeof(kClick[0]), false, false);
            } else if (command.sound == AUDIO_SOUND_CONFIRM) {
                play_notes(kConfirm, sizeof(kConfirm) / sizeof(kConfirm[0]), false, false);
            } else if (command.sound == AUDIO_SOUND_ERROR) {
                play_notes(kError, sizeof(kError) / sizeof(kError[0]), false, false);
            } else if (command.sound == AUDIO_SOUND_SWIPE) {
                play_notes(kSwipe, sizeof(kSwipe) / sizeof(kSwipe[0]), false, false);
            } else if (command.sound == AUDIO_SOUND_MENU_OPEN) {
                play_notes(kMenuOpen, sizeof(kMenuOpen) / sizeof(kMenuOpen[0]), false, false);
            } else if (command.sound == AUDIO_SOUND_ALARM_CHIME) {
                play_notes(kAlarmChime, sizeof(kAlarmChime) / sizeof(kAlarmChime[0]), false, false);
            } else if (command.sound == AUDIO_SOUND_ALARM_BEEP) {
                play_notes(kAlarmBeep, sizeof(kAlarmBeep) / sizeof(kAlarmBeep[0]), false, false);
            } else if (command.sound == AUDIO_SOUND_ALARM_MELODY) {
                play_notes(kAlarmMelody, sizeof(kAlarmMelody) / sizeof(kAlarmMelody[0]), false, false);
            } else if (command.sound == AUDIO_SOUND_CHIME) {
                play_notes(kChime, sizeof(kChime) / sizeof(kChime[0]), false, false);
            } else {
                play_notes(kTimerDone,
                           sizeof(kTimerDone) / sizeof(kTimerDone[0]),
                           command.loop,
                           false);
            }
            break;
        case CommandType::Alarm: {
            size_t count = 0;
            const Note *notes = pattern_for_sound(command.sound, &count);
            play_notes(notes, count, true, true);
            break;
        }
        case CommandType::Noise:
            play_noise(command.brown, command.duration_ms);
            break;
        case CommandType::Stop:
            break;
        }

        if (s_has_pending) {
            command = s_pending;
            s_has_pending = false;
            have_command = true;
            continue;
        }

        s_playing = false;
        s_noise_playing = false;
        s_active_alarm = false;
        s_active_volume = 0;

        // Keep the codec/PA open briefly so short UI sounds can finish in the
        // I2S DMA buffer before the output is disabled.
        if (xQueueReceive(s_queue, &command, pdMS_TO_TICKS(5000)) == pdTRUE) {
            have_command = true;
            continue;
        }
        ESP_LOGI(TAG, "codec idle close");
        close_codec();
    }
}

} // namespace

esp_err_t audio_manager_init(void)
{
    if (s_task != nullptr) {
        return ESP_OK;
    }

    s_queue = xQueueCreate(8, sizeof(Command));
    if (s_queue == nullptr) {
        return ESP_ERR_NO_MEM;
    }

    if (xTaskCreate(audio_task, "audio", 12288, nullptr, 4, &s_task) != pdPASS) {
        vQueueDelete(s_queue);
        s_queue = nullptr;
        s_task = nullptr;
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "Audio manager task started");
    return ESP_OK;
}

esp_err_t audio_manager_prepare(void)
{
    if (!ensure_codec(s_volume)) {
        return ESP_FAIL;
    }
    close_codec();
    ESP_LOGI(TAG, "Speaker codec ready");
    return ESP_OK;
}

void audio_manager_set_volume(uint8_t volume)
{
    s_volume = (uint8_t)clamp_volume(volume);
    s_volume_dirty = true;
}

uint8_t audio_manager_get_volume(void)
{
    return s_volume;
}

bool audio_manager_is_playing(void)
{
    return s_playing;
}

bool audio_manager_is_noise_playing(void)
{
    return s_noise_playing;
}

esp_err_t audio_manager_play(audio_manager_sound_t sound, bool loop)
{
    if (s_queue == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    s_stop_requested = false;
    Command command = {
        .type = CommandType::Play,
        .sound = (uint8_t)sound,
        .loop = loop,
        .brown = false,
        .duration_ms = 0,
    };
    return xQueueSend(s_queue, &command, 0) == pdTRUE ? ESP_OK : ESP_ERR_TIMEOUT;
}

esp_err_t audio_manager_play_alarm(uint8_t sound)
{
    if (s_queue == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    s_stop_requested = false;
    Command command = {
        .type = CommandType::Alarm,
        .sound = sound,
        .loop = true,
        .brown = false,
        .duration_ms = 0,
    };
    return xQueueSend(s_queue, &command, 0) == pdTRUE ? ESP_OK : ESP_ERR_TIMEOUT;
}

esp_err_t audio_manager_start_noise(bool brown, uint32_t duration_ms)
{
    if (s_queue == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    s_stop_requested = false;
    Command command = {
        .type = CommandType::Noise,
        .sound = 0,
        .loop = false,
        .brown = brown,
        .duration_ms = duration_ms,
    };
    return xQueueSend(s_queue, &command, 0) == pdTRUE ? ESP_OK : ESP_ERR_TIMEOUT;
}

void audio_manager_stop(void)
{
    s_stop_requested = true;
    if (s_queue == nullptr) {
        return;
    }
    Command command = {
        .type = CommandType::Stop,
        .sound = 0,
        .loop = false,
        .brown = false,
        .duration_ms = 0,
    };
    xQueueSend(s_queue, &command, 0);
}
