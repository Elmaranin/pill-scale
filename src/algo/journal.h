#pragma once
// Ring journal of fixed-size records in flash memory.
// Portable code: flash access goes through the Flash interface; on the host it
// is replaced by an in-memory array (test/test_journal).
//
// How flash works: only a whole sector can be erased (4 KB, all bytes become
// 0xFF), and a write can only turn ones into zeros. Hence:
//   - 32-byte records, 128 per sector;
//   - a sector is erased before its first record — the 128 oldest are lost;
//   - at power-on the journal is scanned to find the newest record by seq.
// Each sector is erased once per full circle (~a month) — flash can take that.
#include <stddef.h>
#include <stdint.h>
#include <string.h>

class Flash {
public:
    static const uint32_t SECTOR = 4096;
    virtual ~Flash() = default;
    virtual uint32_t size() const = 0;  // multiple of SECTOR
    virtual bool read(uint32_t offset, void *buf, size_t len) = 0;
    virtual bool write(uint32_t offset, const void *buf, size_t len) = 0;
    virtual bool erase_sector(uint32_t offset) = 0;
};

// One journal record. Exactly 32 bytes, plain types only.
struct Record {
    uint32_t seq;       // record number: always grows, 0xFFFFFFFF — empty
    uint32_t boot;      // power-on number
    uint32_t uptime_s;  // seconds since power-on
    uint32_t unix;      // UTC time, 0 — unknown
    uint8_t type;       // what happened (Journal::Type)
    uint8_t kind;       // dose kind, 0 — none
    uint16_t off_s;     // seconds the box was away (for lifts)
    float delta;        // weight change, g
    float weight;       // weight on the platform, g
    uint32_t crc;       // checksum of the first 28 bytes
};
static_assert(sizeof(Record) == 32, "journal record must be 32 bytes");

class Journal {
public:
    enum Type : uint8_t {
        BOOT = 1, TIMESET, PLACED, REMOVED, MISSING,
        NONE, DOSE, DOUBLE, REFILL, NEWBOX, UNSURE, STATUS,
        MISSED,  // a schedule window closed without a dose
    };

    static const char *name(uint8_t t) {
        static const char *n[] = {"?", "boot", "time", "placed", "removed", "missing",
                                  "none", "dose", "double", "refill", "newbox", "unsure", "status",
                                  "missed"};
        return t < sizeof(n) / sizeof(n[0]) ? n[t] : "?";
    }

    // The record has no spare byte, so the schedule verdict shares Record::kind
    // with the dose kind: bits 0-2 dose kind (0-7), bits 3-4 schedule status,
    // bits 5-7 schedule slot (1-4, 0 none). Records written before the schedule
    // existed have kind 0-4 and read back as "no status".
    enum Sched : uint8_t { S_NONE = 0, S_ON = 1, S_EXTRA = 2, S_OFF = 3 };

    static uint8_t pack(uint8_t dose_kind, uint8_t sched, uint8_t slot) {
        return (uint8_t)((dose_kind & 7) | ((sched & 3) << 3) | ((slot & 7) << 5));
    }
    static uint8_t dose_kind(uint8_t kind) { return kind & 7; }
    static uint8_t sched(uint8_t kind) { return (kind >> 3) & 3; }
    static uint8_t slot(uint8_t kind) { return kind >> 5; }

    static const char *sched_name(uint8_t s) {
        static const char *n[] = {"", "on", "extra", "off"};
        return n[s & 3];
    }

    explicit Journal(Flash &f) : f_(f) {}

    // Find the newest record. Call once at power-on when flash is ready: the
    // capacity is computed here, not in the constructor — the journal object
    // may be created before the flash partition is found (size still 0).
    void begin() {
        cap_ = f_.size() / sizeof(Record);
        next_ = 0;
        last_seq_ = 0;
        count_ = 0;
        Record r;
        bool any = false;
        uint32_t newest = 0;
        for (uint32_t i = 0; i < cap_; i++) {
            if (!at(i, r)) continue;
            count_++;
            if (!any || r.seq > last_seq_) { last_seq_ = r.seq; newest = i; any = true; }
        }
        if (any) next_ = (newest + 1) % cap_;
    }

    // Append a record: seq and checksum are set here.
    bool append(Record r) {
        if (cap_ == 0) return false;  // begin() not called or no flash
        // The slot must be erased: if power was lost mid-write it holds garbage,
        // and flash cannot be rewritten without erasing — skip it.
        for (uint32_t tries = 0; tries <= per_sector(); tries++) {
            uint32_t off = next_ * sizeof(Record);
            if (off % Flash::SECTOR == 0 && !erase_at(next_)) return false;
            if (blank(next_)) {
                r.seq = ++last_seq_;
                r.crc = crc32(&r, offsetof(Record, crc));
                if (!f_.write(off, &r, sizeof(r))) return false;
                next_ = (next_ + 1) % cap_;
                count_++;
                return true;
            }
            next_ = (next_ + 1) % cap_;
        }
        return false;
    }

    // i-th record from the end: 0 — the newest. false — no such record.
    bool recent(uint32_t i, Record &out) {
        if (i >= count_) return false;
        uint32_t idx = (next_ + cap_ - 1 - i) % cap_;
        return at(idx, out);
    }

    uint32_t count() const { return count_; }      // how many records are stored
    uint32_t capacity() const { return cap_; }
    uint32_t last_seq() const { return last_seq_; }

    // Erase everything.
    bool clear() {
        for (uint32_t off = 0; off < f_.size(); off += Flash::SECTOR)
            if (!f_.erase_sector(off)) return false;
        next_ = 0;
        count_ = 0;
        return true;  // last_seq_ is kept: record numbers never repeat
    }

private:
    uint32_t per_sector() const { return Flash::SECTOR / sizeof(Record); }

    // Erase the sector starting at slot i: the oldest records are lost.
    bool erase_at(uint32_t i) {
        Record old;
        uint32_t lost = 0;
        for (uint32_t k = i; k < i + per_sector(); k++)
            if (at(k, old)) lost++;
        if (!f_.erase_sector(i * sizeof(Record))) return false;
        count_ -= lost;
        return true;
    }

    bool blank(uint32_t i) {
        uint8_t b[sizeof(Record)];
        if (!f_.read(i * sizeof(Record), b, sizeof(b))) return false;
        for (uint8_t x : b) if (x != 0xFF) return false;
        return true;
    }

    // Read and verify the record in slot i.
    bool at(uint32_t i, Record &r) {
        if (!f_.read(i * sizeof(Record), &r, sizeof(r))) return false;
        if (r.seq == UINT32_MAX) return false;  // erased
        return r.crc == crc32(&r, offsetof(Record, crc));
    }

    static uint32_t crc32(const void *data, size_t len) {
        const uint8_t *p = (const uint8_t *)data;
        uint32_t c = 0xFFFFFFFF;
        for (size_t i = 0; i < len; i++) {
            c ^= p[i];
            for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320 & (0 - (c & 1)));
        }
        return ~c;
    }

    Flash &f_;
    uint32_t cap_ = 0;
    uint32_t next_ = 0;      // where the next record goes
    uint32_t last_seq_ = 0;  // seq of the newest
    uint32_t count_ = 0;     // how many are stored
};

// Time of records written before the scale learned the time.
//
// Within one power-on "unix − seconds since power-on" is a constant: any record
// of that power-on with a known time gives it for all the others, both before
// the time was set and after a reboot. The flash record is not rewritten
// (flash cannot be rewritten without erasing) — it is recomputed on display.
//
// If the time was never learned during a power-on (power was lost first), it
// cannot be recovered: nobody knows how long the scale was off.
class BootClock {
public:
    static const int N = 64;  // that many recent power-ons with a known time are remembered

    // Take a record into account: feed in write order (oldest to newest) — for a
    // power-on the offset of its most recent timed record is kept.
    void learn(const Record &r) {
        if (!r.unix) return;
        int slot = -1, oldest = 0;
        for (int i = 0; i < n_; i++) {
            if (e_[i].boot == r.boot) { slot = i; break; }
            if (e_[i].boot < e_[oldest].boot) oldest = i;
        }
        if (slot < 0) {
            if (n_ < N) slot = n_++;
            else if (r.boot > e_[oldest].boot) slot = oldest;  // forget the oldest power-on
            else return;
        }
        e_[slot].boot = r.boot;
        e_[slot].offset = (int64_t)r.unix - r.uptime_s;
    }

    // Record time, UTC: its own or recovered; 0 — unknown.

    uint32_t utc(const Record &r) const {
        if (r.unix) return r.unix;
        for (int i = 0; i < n_; i++)
            if (e_[i].boot == r.boot) return (uint32_t)(e_[i].offset + r.uptime_s);
        return 0;
    }

    void clear() { n_ = 0; }

private:
    struct Entry {
        uint32_t boot;
        int64_t offset;  // unix − uptime_s
    };
    Entry e_[N];
    int n_ = 0;
};
