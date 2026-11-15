#pragma once
// Dose schedule glue: the portable Schedule (src/algo/schedule.h) plus NVS,
// the clock, the journal and the buzzer. The main loop ticks it; the web
// server reads and edits it from its own task, so everything is under a mutex.
#include <stdint.h>
#include "algo/schedule.h"

void sched_begin();  // load from NVS; call after nvs_flash_init
bool sched_refused(); // the saved schedule had overlapping windows and was not loaded
void sched_poll();   // main loop: once a minute — reminders and "missed" records

// A dose was taken / the box was lifted right now. With the time unknown a
// dose gets NO_SCHEDULE and nothing else happens.
Schedule::Dose sched_dose();
void sched_lift();

bool sched_due();                     // a dose is due right now (LED blinks fast)
void sched_print_events(bool on);     // event on: print E remind / E missed to USB

// The schedule itself. Every change is saved to NVS.
Schedule::State sched_state();
bool sched_set(const Schedule::State &s);  // whole schedule from the web form
bool sched_add(uint16_t minute);
bool sched_del(int slot);
bool sched_window(int before, int after);
void sched_clear();

// How moment k (0-based) stands today, by the current local time.
Schedule::View sched_view(int k);

// Current local time in minutes since 1970 (Schedule's time line); false — unknown.
bool sched_local_now(int64_t &out);

// Dose status for the journal's kind byte (Journal::Sched).
uint8_t sched_to_journal(Schedule::Status s);
