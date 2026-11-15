#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"
#include "sched.h"
#include "logbook.h"
#include "board/buzzer.h"

// NVS: namespace "sched", key "plan" — Schedule::State as is.
static const char *NS = "sched";

static Schedule sched;
static SemaphoreHandle_t mtx;
static bool print_events = false;
static int64_t last_tick = INT64_MIN;  // local minute of the last tick
static bool clock_was_known = false;
static bool refused = false;  // the schedule in NVS broke the rules and was not loaded

// In an anonymous namespace: logbook.cpp and sched.cpp each have their own
// Lock over their own mutex. Without it both are the same ::Lock to the
// linker, which keeps one copy of the inline methods — and one file silently
// locks the other's mutex (the main loop hung once the clock became known).
namespace {
struct Lock {
    Lock() { xSemaphoreTake(mtx, portMAX_DELAY); }
    ~Lock() { xSemaphoreGive(mtx); }
};
}  // namespace

// Current local time in minutes; false — the clock is unknown.
bool sched_local_now(int64_t &out) {
    if (!logbook_time_known()) return false;
    out = ((int64_t)logbook_now_unix() + (int64_t)logbook_tz() * 60) / 60;
    return true;
}

static void save() {
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_blob(h, "plan", &sched.state(), sizeof(Schedule::State));
    nvs_commit(h);
    nvs_close(h);
}

void sched_begin() {
    mtx = xSemaphoreCreateMutex();
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) return;
    Schedule::State st;
    size_t len = sizeof(st);
    // A schedule saved by older firmware may have overlapping windows: it is
    // refused, the scale starts with an empty schedule and says so.
    if (nvs_get_blob(h, "plan", &st, &len) == ESP_OK && len == sizeof(st) && !sched.restore(st))
        refused = true;  // printed by main() once the console is up
    nvs_close(h);
}

// The clock has just become known: doses journaled during the last day (before
// the clock was set, or before a power loss) go into their windows, so a dose
// already taken does not get a reminder. Oldest first — the order matters for
// "extra".
static void replay() {
    int64_t now;
    if (!sched_local_now(now)) return;
    uint32_t since = (uint32_t)((now - Schedule::DAY) * 60 - (int64_t)logbook_tz() * 60);  // UTC
    uint32_t times[32];
    int n = 0;
    Record r;
    for (uint32_t i = 0; n < 32 && logbook_recent(i, r); i++) {
        uint32_t t = logbook_record_utc(r);
        if (t && t < since) break;  // older than a day — the rest is older still
        if (t && (r.type == Journal::DOSE || r.type == Journal::DOUBLE)) times[n++] = t;
    }
    Lock l;
    for (int i = n; i-- > 0;) sched.dose(((int64_t)times[i] + (int64_t)logbook_tz() * 60) / 60);
}

void sched_poll() {
    bool known = logbook_time_known();
    if (known && !clock_was_known) replay();
    clock_was_known = known;
    int64_t now;
    if (!sched_local_now(now) || now == last_tick) return;
    last_tick = now;

    Schedule::Event ev[2 * Schedule::MAX_SLOTS];
    int n;
    {
        Lock l;
        n = sched.tick(now, ev, 2 * Schedule::MAX_SLOTS);
    }
    for (int i = 0; i < n; i++) {
        if (ev[i].action == Schedule::MISSED) {
            if (print_events) printf("E missed slot=%d\n", ev[i].slot);
            logbook_add(Journal::MISSED, Journal::pack(0, 0, (uint8_t)ev[i].slot), 0, 0,
                        logbook_snapshot().weight);
        } else {
            if (print_events) printf("E remind slot=%d\n", ev[i].slot);
            buzzer_beeps(3, 300, 200);
        }
    }
}

Schedule::Dose sched_dose() {
    int64_t now;
    if (!sched_local_now(now)) return {Schedule::NO_SCHEDULE, 0};
    Lock l;
    return sched.dose(now);
}

void sched_lift() {
    int64_t now;
    if (!sched_local_now(now)) return;
    Lock l;
    sched.lift(now);
}

bool sched_due() {
    int64_t now;
    if (!sched_local_now(now)) return false;
    Lock l;
    return sched.due(now);
}

void sched_print_events(bool on) { print_events = on; }

bool sched_refused() { return refused; }

Schedule::State sched_state() {
    Lock l;
    return sched.state();
}

bool sched_set(const Schedule::State &s) {
    Lock l;
    if (!sched.restore(s)) return false;
    save();
    return true;
}

bool sched_add(uint16_t minute) {
    Lock l;
    if (!sched.add(minute)) return false;
    save();
    return true;
}

bool sched_del(int slot) {
    Lock l;
    if (!sched.remove(slot)) return false;
    save();
    return true;
}

bool sched_window(int before, int after) {
    Lock l;
    if (!sched.set_window(before, after)) return false;
    save();
    return true;
}

void sched_clear() {
    Lock l;
    Schedule::State st = sched.state();
    st.n = 0;
    sched.restore(st);
    save();
}

Schedule::View sched_view(int k) {
    int64_t now;
    Lock l;
    if (!sched_local_now(now)) return {sched.state().minute[k], Schedule::UPCOMING, 0};
    return sched.view(k, now);
}

uint8_t sched_to_journal(Schedule::Status s) {
    switch (s) {
    case Schedule::ON_TIME: return Journal::S_ON;
    case Schedule::EXTRA: return Journal::S_EXTRA;
    case Schedule::OFF_SCHEDULE: return Journal::S_OFF;
    default: return Journal::S_NONE;
    }
}
