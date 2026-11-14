#include <stdio.h>
#include <string.h>
#include <time.h>
#include "board/flash_partition.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "logbook.h"
#include "nvs.h"

// NVS: namespace "logbook" — power-on counter and time zone.
static const char *NS = "logbook";

static PartitionFlash flash;
static Journal journal(flash);
static BootClock clock_;  // time of records written before the clock was set
static bool ok = false;
static SemaphoreHandle_t mtx;

static uint32_t boot = 0;
static bool time_known = false;
static int64_t unix_offset = 0;  // unix − uptime_s, once the time is known
static int tz_min = 180;          // Moscow by default
static Snapshot snap;
static Live live;

struct Lock {
    Lock() { xSemaphoreTake(mtx, portMAX_DELAY); }
    ~Lock() { xSemaphoreGive(mtx); }
};

uint32_t logbook_uptime_s() { return (uint32_t)(esp_timer_get_time() / 1000000); }

void logbook_begin() {
    mtx = xSemaphoreCreateMutex();
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_get_u32(h, "boot", &boot);
        boot++;
        nvs_set_u32(h, "boot", boot);
        int32_t tz;
        if (nvs_get_i32(h, "tz", &tz) == ESP_OK) tz_min = tz;
        nvs_commit(h);
        nvs_close(h);
    }
    ok = flash.begin();
    if (ok) {
        journal.begin();
        Record r;
        for (uint32_t i = journal.count(); i-- > 0;)  // oldest to newest
            if (journal.recent(i, r)) clock_.learn(r);
    }
    logbook_add(Journal::BOOT);
}

bool logbook_ok() { return ok; }
uint32_t logbook_boot() { return boot; }
int logbook_tz() { return tz_min; }
bool logbook_time_known() { return time_known; }

uint32_t logbook_now_unix() {
    return time_known ? (uint32_t)(unix_offset + logbook_uptime_s()) : 0;
}

void logbook_add(uint8_t type, uint8_t kind, uint16_t off_s, float delta, float weight) {
    if (!ok) return;
    Record r;
    memset(&r, 0, sizeof(r));
    r.boot = boot;
    r.uptime_s = logbook_uptime_s();
    r.unix = logbook_now_unix();
    r.type = type;
    r.kind = kind;
    r.off_s = off_s;
    r.delta = delta;
    r.weight = weight;
    Lock l;
    journal.append(r);
    clock_.learn(r);
}

bool logbook_recent(uint32_t i, Record &out) {
    if (!ok) return false;
    Lock l;
    return journal.recent(i, out);
}

uint32_t logbook_count() {
    if (!ok) return 0;
    Lock l;
    return journal.count();
}

uint32_t logbook_total() {
    if (!ok) return 0;
    Lock l;
    return journal.last_seq();
}

bool logbook_clear() {
    if (!ok) return false;
    Lock l;
    clock_.clear();
    return journal.clear();
}

void logbook_set_time(uint32_t unix, int tz) {
    {
        Lock l;
        unix_offset = (int64_t)unix - logbook_uptime_s();
        time_known = true;
        tz_min = tz;
    }
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_i32(h, "tz", tz);
        nvs_commit(h);
        nvs_close(h);
    }
    logbook_add(Journal::TIMESET);
}

void logbook_format_time(const Record &r, char *buf, size_t n) {
    // Record time: its own if known at write time; otherwise derived from a record
    // of the same power-on written after the time was set (BootClock); otherwise unknown.

    int64_t t;
    {
        Lock l;
        t = clock_.utc(r);
    }
    if (!t) { snprintf(buf, n, "-"); return; }
    time_t local = (time_t)(t + tz_min * 60);
    struct tm tm;
    gmtime_r(&local, &tm);
    strftime(buf, n, "%Y-%m-%dT%H:%M:%S", &tm);
}

void logbook_set_snapshot(const Snapshot &s) {
    Lock l;
    snap = s;
}

Snapshot logbook_snapshot() {
    Lock l;
    return snap;
}

void logbook_set_live(const Live &v) {
    Lock l;
    live = v;
}

Live logbook_live() {
    Lock l;
    return live;
}
