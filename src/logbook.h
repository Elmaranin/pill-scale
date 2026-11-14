#pragma once
// Event journal in flash and the clock — shared by the command line and the web page.
// The main loop writes, the web server reads from its own task, so everything is under a mutex.
#include <stddef.h>
#include <stdint.h>
#include "algo/journal.h"

// Call once after nvs_flash_init. Writes a "boot" record.
void logbook_begin();
bool logbook_ok();  // the journal exists (partition "log" found)

void logbook_add(uint8_t type, uint8_t kind = 0, uint16_t off_s = 0, float delta = 0, float weight = 0);
bool logbook_recent(uint32_t i, Record &out);  // 0 — the newest
uint32_t logbook_count();                      // how many records are stored
uint32_t logbook_total();                      // seq of the last one (written since the beginning)
bool logbook_clear();

// Time. There is no battery-backed clock: known only if it was sent after power-on.
void logbook_set_time(uint32_t unix, int tz_min);
bool logbook_time_known();
uint32_t logbook_now_unix();  // 0 — unknown
int logbook_tz();             // local time offset, minutes
uint32_t logbook_boot();      // number of the current power-on
uint32_t logbook_uptime_s();

// Local time of a record, "2026-10-25T08:05:12", or "-" if unknown.
void logbook_format_time(const Record &r, char *buf, size_t n);

// State snapshot for the web page: updated by the main loop.
struct Snapshot {
    float weight = 0;      // weight on the platform, g
    float box = 0;         // box weight, g
    bool box_on = false;
    bool calibrated = false;
    uint8_t kinds = 0;     // learned dose kinds
    float kind_g[4] = {};
    uint16_t kind_n[4] = {};
};
void logbook_set_snapshot(const Snapshot &s);
Snapshot logbook_snapshot();

// Current weight for the "Scale" tab: updated by the main loop on every ADC sample.
struct Live {
    bool calibrated = false;
    int32_t raw = 0;     // last raw ADC sample
    int32_t offset = 0;  // calibration zero
    float scale = 1;     // counts per gram
    float g = 0;         // mean of the last n samples, g
    float sd = 0;        // spread of those samples, g

    uint8_t n = 0;
};
void logbook_set_live(const Live &l);
Live logbook_live();
