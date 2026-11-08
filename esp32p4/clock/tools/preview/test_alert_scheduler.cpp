#include <cstdio>
#include <cstdlib>

#include "clock_alert_scheduler.h"

#define CHECK(condition) do { if (!(condition)) { \
    std::fprintf(stderr, "FAILED %s:%d: %s\n", __FILE__, __LINE__, #condition); \
    std::exit(1); } } while (0)

static tm at(int day, int hour = 7, int minute = 30)
{
    tm local = {};
    local.tm_year = 126;
    local.tm_mon = 10;
    local.tm_mday = day;
    local.tm_wday = (day - 8) % 7; // Nov 8, 2026 is Sunday.
    local.tm_hour = hour;
    local.tm_min = minute;
    return local;
}

int main()
{
    clock_alarm_t alarms[CLOCK_ALARM_COUNT] = {
        {true, 7, 30, 0x7f, 0}, {true, 7, 30, 0x7f, 1}, {true, 7, 31, 0x7f, 2},
    };
    ClockAlertScheduler schedule;
    schedule.collect(at(8), alarms, 1000);
    schedule.complete_timer();
    CHECK(!schedule.take_timer(false)); // Queued alarms precede the timer.
    CHECK(schedule.take_alarm() == 0);
    CHECK(!schedule.take_timer(true));
    schedule.snooze(0, 1000);
    CHECK(schedule.take_alarm() == 1); // Snooze doesn't block a second alarm.
    CHECK(schedule.take_alarm() == -1);
    CHECK(schedule.take_timer(false)); // Timer can be delivered during snooze.
    CHECK(!schedule.take_timer(false));
    schedule.collect(at(8), alarms, 2000);
    CHECK(schedule.take_alarm() == -1); // No repeated occurrence in the same minute.
    schedule.collect(at(8, 7, 31), alarms, 61000);
    CHECK(schedule.take_alarm() == 2); // Collect even while another alarm rings.
    schedule.collect(at(8, 6, 0), alarms, 301000);
    CHECK(schedule.take_alarm() == 0); // Snooze uses monotonic time after clock rollback.
    schedule.collect(at(8), alarms, 302000);
    CHECK(schedule.take_alarm() == -1);
    schedule.collect(at(9), alarms, 400000);
    CHECK(schedule.take_alarm() == 0); // Same hour/minute on the following date.
    CHECK(schedule.take_alarm() == 1);
    schedule.collect(at(8), alarms, 500000);
    CHECK(schedule.take_alarm() == -1); // Date rollback doesn't replay alarms.
    schedule.collect(at(10, 7, 29), alarms, 600000);
    schedule.collect(at(10, 7, 32), alarms, 700000);
    CHECK(schedule.take_alarm() == -1); // No catch-up after a forward correction.

    ClockAlertScheduler cancelled;
    cancelled.collect(at(8), alarms, 0);
    CHECK(cancelled.take_alarm() == 0);
    cancelled.snooze(0, 0);
    alarms[0].enabled = false;
    alarms[1].enabled = false;
    cancelled.collect(at(8), alarms, 300000);
    CHECK(cancelled.take_alarm() == -1); // Disabling cancels queued and snoozed alerts.
    cancelled.complete_timer();
    cancelled.cancel_timer();
    CHECK(!cancelled.take_timer(false));

    ClockAlertScheduler weekdays;
    alarms[2] = {true, 7, 30, 0x3e, 0};
    weekdays.collect(at(8), alarms, 0);
    CHECK(weekdays.take_alarm() == -1); // Sunday excluded.
    weekdays.collect(at(9), alarms, 0);
    CHECK(weekdays.take_alarm() == 2); // Monday included.
    weekdays.reconfigure(2);
    weekdays.collect(at(9), alarms, 10);
    CHECK(weekdays.take_alarm() == 2); // Explicit schedule edits re-arm the slot.
    CHECK(!ClockAlertScheduler::expired(1000, 60999));
    CHECK(ClockAlertScheduler::expired(1000, 61000));
    CHECK(!ClockAlertScheduler::expired(1000, 999));
    CHECK(ClockAlertScheduler::expired(1ULL << 33, (1ULL << 33) + 60000));
    ClockAlertScheduler chimes;
    CHECK(!chimes.take_chime(at(8, 8, 0), false));
    CHECK(!chimes.take_chime(at(8, 8, 0), true)); // Suppressed chime isn't deferred.
    CHECK(chimes.take_chime(at(8, 9, 0), true));
    CHECK(!chimes.take_chime(at(8, 9, 0), true));
    CHECK(!chimes.take_chime(at(8, 22, 0), true));
    CHECK(chimes.take_chime(at(9, 9, 0), true));
    std::puts("PASS alert scheduler: dates, queue, snooze, cancellation, timer priority, clock jumps, timeout");
}
