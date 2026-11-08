#pragma once

#include <stdint.h>
#include <time.h>

#include "clock_settings.h"

// LVGL-thread-owned scheduling state; no hardware or wall-clock reads here.
// Call collect even while an alert is visible so concurrent alarms aren't lost.
class ClockAlertScheduler {
public:
    static constexpr uint64_t kSnoozeMs = 5ULL * 60ULL * 1000ULL;
    static constexpr uint64_t kAlertDurationMs = 60ULL * 1000ULL;

    void collect(const tm &local, const clock_alarm_t *alarms, uint64_t now_ms);
    int take_alarm();
    void snooze(unsigned slot, uint64_t now_ms);
    void cancel(unsigned slot);
    void reconfigure(unsigned slot);
    void complete_timer() { timer_pending_ = true; }
    void cancel_timer() { timer_pending_ = false; }
    bool take_timer(bool alarm_active);
    bool take_chime(const tm &local, bool allowed);
    static bool expired(uint64_t started_ms, uint64_t now_ms)
    { return now_ms >= started_ms && now_ms - started_ms >= kAlertDurationMs; }

private:
    int last_date_[CLOCK_ALARM_COUNT] = {};
    bool pending_[CLOCK_ALARM_COUNT] = {};
    uint64_t snooze_due_[CLOCK_ALARM_COUNT] = {};
    bool timer_pending_ = false;
    int64_t last_chime_key_ = -1;
};
