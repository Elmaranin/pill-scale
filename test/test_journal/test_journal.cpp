// Host test: see test/CMakeLists.txt
// Ring journal on an "in-memory flash" that behaves like the real one:
// erase only by sector, a write only turns ones into zeros.
#include <unity.h>
#include <stdio.h>
#include <string.h>
#include <vector>
#include "journal.h"

void setUp() {}
void tearDown() {}

class RamFlash : public Flash {
public:
    explicit RamFlash(uint32_t sectors) : mem_(sectors * SECTOR, 0x00) {}  // "dirty" flash
    uint32_t size() const override { return (uint32_t)mem_.size(); }
    bool read(uint32_t off, void *buf, size_t len) override {
        if (off + len > mem_.size()) return false;
        memcpy(buf, &mem_[off], len);
        return true;
    }
    bool write(uint32_t off, const void *buf, size_t len) override {
        if (off + len > mem_.size()) return false;
        const uint8_t *p = (const uint8_t *)buf;
        for (size_t i = 0; i < len; i++) mem_[off + i] &= p[i];  // like NOR: only 1 → 0
        writes++;
        return true;
    }
    bool erase_sector(uint32_t off) override {
        if (off % SECTOR || off >= mem_.size()) return false;
        memset(&mem_[off], 0xFF, SECTOR);
        erases++;
        return true;
    }
    std::vector<uint8_t> mem_;
    int writes = 0, erases = 0;
};

static Record rec(uint8_t type, float weight) {
    Record r;
    memset(&r, 0, sizeof(r));
    r.boot = 1;
    r.type = type;
    r.weight = weight;
    return r;
}

static void test_append_and_read_back() {
    RamFlash f(4);  // 4 sectors = 512 records
    Journal j(f);
    j.clear();
    j.begin();
    TEST_ASSERT_EQUAL_UINT32(0, j.count());
    for (int i = 0; i < 10; i++) TEST_ASSERT_TRUE(j.append(rec(Journal::DOSE, (float)i)));
    TEST_ASSERT_EQUAL_UINT32(10, j.count());
    Record r;
    TEST_ASSERT_TRUE(j.recent(0, r));
    TEST_ASSERT_EQUAL_FLOAT(9.0f, r.weight);  // the newest
    TEST_ASSERT_EQUAL_UINT32(10, r.seq);
    TEST_ASSERT_TRUE(j.recent(9, r));
    TEST_ASSERT_EQUAL_FLOAT(0.0f, r.weight);  // the oldest
    TEST_ASSERT_FALSE(j.recent(10, r));
}

// After a reboot the journal finds where it stopped.
static void test_survives_reboot() {
    RamFlash f(4);
    {
        Journal j(f);
        j.clear();
        j.begin();
        for (int i = 0; i < 300; i++) j.append(rec(Journal::STATUS, (float)i));
    }
    Journal j2(f);  // "powered on again"
    j2.begin();
    TEST_ASSERT_EQUAL_UINT32(300, j2.count());
    TEST_ASSERT_EQUAL_UINT32(300, j2.last_seq());
    j2.append(rec(Journal::BOOT, 0));
    Record r;
    j2.recent(0, r);
    TEST_ASSERT_EQUAL_UINT32(301, r.seq);
    TEST_ASSERT_EQUAL_UINT8(Journal::BOOT, r.type);
    j2.recent(1, r);
    TEST_ASSERT_EQUAL_FLOAT(299.0f, r.weight);
}

// Wrap-around: new records erase the sector of the oldest ones, the order holds.
static void test_wraps_around() {
    RamFlash f(4);  // capacity 512
    Journal j(f);
    j.clear();
    j.begin();
    for (int i = 0; i < 2000; i++) TEST_ASSERT_TRUE(j.append(rec(Journal::STATUS, (float)i)));
    TEST_ASSERT_TRUE(j.count() >= 512 - 128 && j.count() <= 512);  // minus the erased sector
    Record r;
    j.recent(0, r);
    TEST_ASSERT_EQUAL_FLOAT(1999.0f, r.weight);
    // all stored records are consecutive, newest to oldest
    for (uint32_t i = 0; i < j.count(); i++) {
        TEST_ASSERT_TRUE(j.recent(i, r));
        TEST_ASSERT_EQUAL_FLOAT((float)(1999 - (int)i), r.weight);
    }
    // and the same after a reboot
    Journal j2(f);
    j2.begin();
    TEST_ASSERT_EQUAL_UINT32(j.count(), j2.count());
    j2.recent(0, r);
    TEST_ASSERT_EQUAL_UINT32(2000, r.seq);
}

// Power lost mid-write: the slot holds garbage. The journal does not count it
// and continues in a clean slot.
static void test_torn_write_is_skipped() {
    RamFlash f(4);
    Journal j(f);
    j.clear();
    j.begin();
    for (int i = 0; i < 5; i++) j.append(rec(Journal::DOSE, (float)i));
    uint32_t torn = 5 * sizeof(Record);
    uint8_t half[16];
    memset(half, 0x12, sizeof(half));
    f.write(torn, half, sizeof(half));  // a half-written sixth record
    Journal j2(f);
    j2.begin();
    TEST_ASSERT_EQUAL_UINT32(5, j2.count());
    TEST_ASSERT_TRUE(j2.append(rec(Journal::DOSE, 99.0f)));
    Record r;
    j2.recent(0, r);
    TEST_ASSERT_EQUAL_FLOAT(99.0f, r.weight);
    TEST_ASSERT_EQUAL_UINT32(6, r.seq);
    Journal j3(f);
    j3.begin();
    j3.recent(0, r);
    TEST_ASSERT_EQUAL_FLOAT(99.0f, r.weight);
}

// The flash is "dirty" by itself (not 0xFF): the first record in a sector erases it.
static void test_dirty_flash_without_clear() {
    RamFlash f(2);
    Journal j(f);
    j.begin();
    TEST_ASSERT_EQUAL_UINT32(0, j.count());  // garbage is not taken for records
    TEST_ASSERT_TRUE(j.append(rec(Journal::BOOT, 1.0f)));
    TEST_ASSERT_EQUAL_UINT32(1, j.count());
}

// As in the firmware: the journal object is created before the flash partition
// is found (size still 0). The capacity must be computed in begin(), otherwise
// old records are invisible after power-on and the first new one erases them
// (bug seen on the scale 2026-10-25).
class LateFlash : public RamFlash {
public:
    using RamFlash::RamFlash;
    bool ready = false;
    uint32_t size() const override { return ready ? RamFlash::size() : 0; }
};

static void test_flash_ready_after_construction() {
    LateFlash f(4);
    {
        Journal j(f);  // flash not ready yet
        f.ready = true;
        j.begin();
        j.clear();
        for (int i = 0; i < 10; i++) TEST_ASSERT_TRUE(j.append(rec(Journal::DOSE, (float)i)));
    }
    f.ready = false;
    Journal j2(f);  // "powered on again" — again before the flash is ready
    f.ready = true;
    j2.begin();
    TEST_ASSERT_EQUAL_UINT32(10, j2.count());  // old records are in place
    TEST_ASSERT_TRUE(j2.append(rec(Journal::BOOT, 0)));
    TEST_ASSERT_EQUAL_UINT32(11, j2.count());  // and the new one did not erase them
    Record r;
    j2.recent(10, r);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, r.weight);
}


// ---- BootClock: time of records written before the clock was set ----

static Record at_time(uint32_t boot, uint32_t up, uint32_t unix, uint8_t type = Journal::DOSE) {
    Record r;
    memset(&r, 0, sizeof(r));
    r.boot = boot;
    r.uptime_s = up;
    r.unix = unix;
    r.type = type;
    return r;
}

// Records before the time was set get it from a record of the same power-on.
static void test_clock_fills_before_timeset() {
    BootClock c;
    Record early = at_time(5, 10, 0);
    c.learn(early);
    TEST_ASSERT_EQUAL_UINT32(0, c.utc(early));                // time still unknown
    c.learn(at_time(5, 100, 1000100, Journal::TIMESET));      // time arrived at 100 s
    TEST_ASSERT_EQUAL_UINT32(1000010, c.utc(early));          // 10 s after power-on
    TEST_ASSERT_EQUAL_UINT32(1000100, c.utc(at_time(5, 100, 1000100)));  // its own — as is
    TEST_ASSERT_EQUAL_UINT32(0, c.utc(at_time(4, 10, 0)));    // another power-on — impossible
}

// As in the firmware: after a reboot the table is rebuilt by a pass over the
// journal, and records of a past power-on written before the time was set keep
// it. A power-on during which the time was never learned stays without time.
static void test_clock_survives_reboot() {
    RamFlash f(4);
    Journal j(f);
    j.clear();
    j.begin();
    j.append(at_time(7, 0, 0, Journal::BOOT));
    j.append(at_time(7, 5, 0, Journal::PLACED));
    j.append(at_time(7, 600, 1793000600, Journal::TIMESET));
    j.append(at_time(7, 700, 1793000700, Journal::DOSE));
    j.append(at_time(8, 0, 0, Journal::BOOT));     // power-on 8: time never arrived
    j.append(at_time(8, 30, 0, Journal::PLACED));

    Journal j2(f);  // "powered on again"
    j2.begin();
    BootClock c;
    Record r;
    for (uint32_t i = j2.count(); i-- > 0;)  // oldest to newest, like logbook_begin
        if (j2.recent(i, r)) c.learn(r);

    j2.recent(4, r);  // placed of power-on 7
    TEST_ASSERT_EQUAL_UINT8(Journal::PLACED, r.type);
    TEST_ASSERT_EQUAL_UINT32(0, r.unix);
    TEST_ASSERT_EQUAL_UINT32(1793000005, c.utc(r));
    j2.recent(0, r);  // placed of power-on 8
    TEST_ASSERT_EQUAL_UINT32(0, c.utc(r));
}

// The table remembers the last N power-ons: the oldest are evicted.
static void test_clock_keeps_newest_boots() {
    BootClock c;
    for (uint32_t b = 1; b <= BootClock::N + 6; b++) c.learn(at_time(b, 100, 1000000 * b));
    TEST_ASSERT_EQUAL_UINT32(0, c.utc(at_time(1, 50, 0)));  // forgotten
    uint32_t last = BootClock::N + 6;
    TEST_ASSERT_EQUAL_UINT32(1000000 * last - 50, c.utc(at_time(last, 50, 0)));
    c.learn(at_time(2, 100, 2000000));                      // an old power-on does not evict new ones
    TEST_ASSERT_EQUAL_UINT32(1000000 * last - 50, c.utc(at_time(last, 50, 0)));
}

// Days since 1970-01-01 from a civil date (Gregorian) — to not depend on timegm.
static int64_t days_from_civil(int y, int m, int d) {
    y -= m <= 2;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    int64_t yoe = y - era * 400;
    int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

// The real journal of the week 2026-10-25..11-01, downloaded from the scale's page.
// Before the fix 19 records had no time; 3 of them are from power-ons where the
// time was set later (after a reboot the firmware "forgot" them). One more is
// from the power-on current at download time: the firmware already showed its
// time, and the recovered one must match it.
static void test_clock_on_week_log() {
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", PILLSCALE_DATA_DIR, "pillscale_log.csv");
    FILE *f = fopen(path, "r");
    TEST_ASSERT_NOT_NULL_MESSAGE(f, path);
    std::vector<Record> recs;
    std::vector<int64_t> shown;  // local_time from the file in seconds, −1 — "-"
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        unsigned long seq, boot, up, unix;
        char t[32];
        if (sscanf(line, "%lu,%lu,%lu,%lu,%31[^,]", &seq, &boot, &up, &unix, t) != 5) continue;  // header
        recs.push_back(at_time((uint32_t)boot, (uint32_t)up, (uint32_t)unix));
        int Y, M, D, h, mi, sec;
        if (sscanf(t, "%d-%d-%dT%d:%d:%d", &Y, &M, &D, &h, &mi, &sec) == 6)
            shown.push_back(days_from_civil(Y, M, D) * 86400 + h * 3600 + mi * 60 + sec);
        else
            shown.push_back(-1);
    }
    fclose(f);
    TEST_ASSERT_EQUAL(1107, (int)recs.size());

    BootClock c;
    for (const Record &r : recs) c.learn(r);  // the file is oldest to newest
    int no_unix = 0, filled = 0, still_unknown = 0, checked = 0;
    for (size_t i = 0; i < recs.size(); i++) {
        if (recs[i].unix) continue;
        no_unix++;
        uint32_t t = c.utc(recs[i]);
        if (!t) { still_unknown++; continue; }
        filled++;
        if (shown[i] >= 0) {  // the firmware showed a time back then — compare (tz +180 min)
            TEST_ASSERT_EQUAL_INT64(shown[i], (int64_t)t + 180 * 60);
            checked++;
        }
    }
    TEST_ASSERT_EQUAL(20, no_unix);
    TEST_ASSERT_EQUAL(4, filled);          // 3 "forgotten" after reboot + 1 of the current power-on
    TEST_ASSERT_EQUAL(16, still_unknown);  // time was never learned during those power-ons

    TEST_ASSERT_EQUAL(1, checked);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_append_and_read_back);
    RUN_TEST(test_survives_reboot);
    RUN_TEST(test_wraps_around);
    RUN_TEST(test_torn_write_is_skipped);
    RUN_TEST(test_dirty_flash_without_clear);
    RUN_TEST(test_flash_ready_after_construction);
    RUN_TEST(test_clock_fills_before_timeset);
    RUN_TEST(test_clock_survives_reboot);
    RUN_TEST(test_clock_keeps_newest_boots);
    RUN_TEST(test_clock_on_week_log);
    return UNITY_END();
}
