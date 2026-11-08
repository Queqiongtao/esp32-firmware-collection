#include "clock_alert_scheduler.h"

void ClockAlertScheduler::collect(const tm &local, const clock_alarm_t *alarms,
                                  uint64_t now_ms)
{
    const int date = (local.tm_year + 1900) * 10000 +
                     (local.tm_mon + 1) * 100 + local.tm_mday;
    for (unsigned i = 0; i < CLOCK_ALARM_COUNT; ++i) {
        const auto &alarm = alarms[i];
        if (!alarm.enabled) {
            cancel(i);
            continue;
        }
        if (snooze_due_[i] != 0 && now_ms >= snooze_due_[i]) {
            pending_[i] = true;
            snooze_due_[i] = 0;
        }
        // A date high-water mark also prevents a backwards clock correction
        // from replaying an already collected occurrence. Don't catch up gaps.
        if (date <= last_date_[i] || alarm.hour != local.tm_hour ||
            alarm.minute != local.tm_min || local.tm_wday < 0 || local.tm_wday > 6 ||
            (alarm.weekdays != 0 && !(alarm.weekdays & (1U << local.tm_wday)))) {
            continue;
        }
        last_date_[i] = date;
        pending_[i] = true;
    }
}

int ClockAlertScheduler::take_alarm()
{
    for (unsigned i = 0; i < CLOCK_ALARM_COUNT; ++i) {
        if (pending_[i]) {
            pending_[i] = false;
            return static_cast<int>(i);
        }
    }
    return -1;
}

void ClockAlertScheduler::snooze(unsigned slot, uint64_t now_ms)
{
    if (slot < CLOCK_ALARM_COUNT) {
        pending_[slot] = false;
        snooze_due_[slot] = now_ms + kSnoozeMs;
    }
}

void ClockAlertScheduler::cancel(unsigned slot)
{
    if (slot < CLOCK_ALARM_COUNT) {
        pending_[slot] = false;
        snooze_due_[slot] = 0;
    }
}

bool ClockAlertScheduler::take_timer(bool alarm_active)
{
    if (alarm_active || !timer_pending_) {
        return false;
    }
    for (bool pending : pending_) {
        if (pending) {
            return false;
        }
    }
    timer_pending_ = false;
    return true;
}

void ClockAlertScheduler::reconfigure(unsigned slot)
{
    if (slot < CLOCK_ALARM_COUNT) {
        cancel(slot);
        last_date_[slot] = 0;
    }
}

bool ClockAlertScheduler::take_chime(const tm &local, bool allowed)
{
    if (local.tm_min != 0) {
        return false;
    }
    const int64_t key = ((int64_t)(local.tm_year + 1900) * 10000 +
                         (local.tm_mon + 1) * 100 + local.tm_mday) * 24 + local.tm_hour;
    if (key <= last_chime_key_) {
        return false;
    }
    // Consume a suppressed occurrence, so clearing an alert or unmuting later
    // in the minute doesn't play a delayed hourly chime.
    last_chime_key_ = key;
    return allowed && local.tm_hour >= 8 && local.tm_hour < 22;
}
