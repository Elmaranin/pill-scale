#pragma once
// Dose schedule: up to four moments a day with a common window around each.
// Portable code: no framework dependencies. Tested on the host (test/test_schedule).
//
// Time comes in as *local minutes*: local unix time / 60, i.e. minutes since
// 1970-01-01 00:00 local. A whole number line instead of "hours and minutes"
// makes midnight no special case: the window of a 23:30 moment with after=60
// simply ends at 00:30 of the next day. Day d starts at d*1440.
//
// Windows of two moments never overlap: add(), restore() and set_window()
// refuse such a schedule. Otherwise one dose could belong to two moments, and
// whichever rule picks one, the other stays "due" and beeps (2026-11-15:
// 17:46 and 17:50 with a 30/60 window — the 17:46 moment could never be taken).
// So at any minute at most one window is open.
//
// Each moment has one "current occurrence" — the day's instance whose window
// is still open (or next to open). A dose inside the window marks it taken;
// when the window closes untaken, the moment is reported MISSED once and the
// occurrence moves to the next day. Nothing is remembered about moments that
// passed before the clock was known: the scale did not watch them.
#include <stdint.h>
#include <string.h>

class Schedule {
public:
    static const int MAX_SLOTS = 4;
    static const int DAY = 1440;  // minutes

    // Stored in NVS as is: plain types only. minute[] sorted ascending, unique.
    struct State {
        uint8_t n;                    // moments in use
        uint16_t minute[MAX_SLOTS];   // minutes after midnight, 0..1439
        uint16_t before;              // window start: this many minutes before the moment
        uint16_t after;               // window end: this many minutes after the moment
    };

    struct Config {
        int every = 5;  // minutes between reminder beeps while the dose is due
    };

    // Verdict for one dose.
    enum Status { NO_SCHEDULE, ON_TIME, EXTRA, OFF_SCHEDULE };
    struct Dose { Status status; int slot; };  // slot 1..n for ON_TIME and EXTRA, otherwise 0

    // What the clock tick asks the glue to do.
    enum Action { REMIND, MISSED };
    struct Event { Action action; int slot; };

    // One moment as the page shows it for "today".
    enum SlotState { UPCOMING, DUE, TAKEN, MISSED_ST };
    struct View { uint16_t minute; SlotState state; int64_t at; };  // at: taken_at for TAKEN

    Schedule() : Schedule(Config()) {}
    explicit Schedule(const Config &cfg) : cfg_(cfg) { clear(); }

    // --- the stored schedule ---

    const State &state() const { return st_; }

    void clear() {
        memset(&st_, 0, sizeof(st_));
        st_.before = 30;
        st_.after = 60;
        forget();
    }

    // Replace the whole schedule (from NVS or the web form). Sorts and drops
    // duplicates; false — out of range (window up to 12 h, minute < 1440) or
    // windows overlap.
    bool restore(const State &s) {
        if (s.n > MAX_SLOTS || s.before > 720 || s.after > 720) return false;
        State t = {};
        t.before = s.before;
        t.after = s.after;
        for (int i = 0; i < s.n; i++) {
            if (s.minute[i] >= DAY) return false;
            insert(t, s.minute[i]);
        }
        if (overlap(t)) return false;
        st_ = t;
        forget();
        return true;
    }

    // false — full, already there, or its window overlaps a neighbour's.
    bool add(uint16_t minute) {
        if (minute >= DAY || st_.n >= MAX_SLOTS) return false;
        State t = st_;
        if (!insert(t, minute) || overlap(t)) return false;
        st_ = t;
        forget();
        return true;
    }

    bool remove(int slot) {  // 1..n
        if (slot < 1 || slot > st_.n) return false;
        for (int i = slot - 1; i + 1 < st_.n; i++) st_.minute[i] = st_.minute[i + 1];
        st_.n--;
        forget();
        return true;
    }

    bool set_window(int before, int after) {
        if (before < 0 || before > 720 || after < 0 || after > 720) return false;
        State t = st_;
        t.before = (uint16_t)before;
        t.after = (uint16_t)after;
        if (overlap(t)) return false;
        st_ = t;
        forget();
        return true;
    }

    // Do any two windows overlap? s.minute[] must be sorted. Moments a < b are
    // fine when b − a > before + after: the window of a ends at a + after, the
    // window of b starts at b − before, both ends inclusive. The last moment is
    // checked against the next day's first one, so a single moment also needs
    // before + after < 24 h. Returns the slot (1..n) whose window runs into
    // the next moment's, 0 — no overlap.
    static int overlap(const State &s) {
        int need = s.before + s.after;
        for (int i = 0; i < s.n; i++) {
            int a = s.minute[i];
            int b = i + 1 < s.n ? s.minute[i + 1] : s.minute[0] + DAY;
            if (b - a <= need) return i + 1;
        }
        return 0;
    }

    // Insert keeping the order; false — already there. Public so the glue can
    // try a change on a copy and say *why* it is refused.
    static bool insert(State &st, uint16_t minute) {
        for (int i = 0; i < st.n; i++) if (st.minute[i] == minute) return false;
        int i = st.n;
        while (i > 0 && st.minute[i - 1] > minute) { st.minute[i] = st.minute[i - 1]; i--; }
        st.minute[i] = minute;
        st.n++;
        return true;
    }

    // --- events ---

    // A dose was taken at local minute t. Windows do not overlap, so at most
    // one moment's window contains t.
    Dose dose(int64_t t) {
        if (st_.n == 0) return {NO_SCHEDULE, 0};
        int k = -1;
        int64_t occ = 0, best = 0;
        for (int i = 0; i < st_.n; i++) {
            int64_t o;
            if (!occurrence_around(i, t, o)) continue;
            int64_t dist = o > t ? o - t : t - o;
            if (k < 0 || dist < best) { k = i; occ = o; best = dist; }
        }
        if (k < 0) return {OFF_SCHEDULE, 0};
        Slot &s = sl_[k];
        if (s.occ != occ) { s = Slot(); s.occ = occ; s.set = true; }  // a window we had not entered yet
        if (s.taken) return {EXTRA, k + 1};
        s.taken = true;
        s.taken_at = t;
        return {ON_TIME, k + 1};
    }

    // The box was lifted at t: the person is at the scale, hold the next beep.
    void lift(int64_t t) {
        for (int i = 0; i < st_.n; i++)
            if (sl_[i].set && !sl_[i].taken && t >= sl_[i].occ && t <= sl_[i].occ + st_.after)
                sl_[i].last_remind = t;
    }

    // Clock tick at local minute t (call once per minute). Fills out[] with up
    // to max events and returns how many.
    int tick(int64_t t, Event *out, int max) {
        int n = 0;
        for (int i = 0; i < st_.n; i++) {
            Slot &s = sl_[i];
            if (s.set && t > s.occ + st_.after) {           // window closed
                if (!s.taken && n < max) out[n++] = {MISSED, i + 1};
                prev_[i] = s;
                s = Slot();
            }
            if (!s.set) {                                    // pick the occurrence whose window is open or next
                s.occ = next_occurrence(i, t);
                s.set = true;
                s.last_remind = INT64_MIN;
            }
            if (!s.taken && t >= s.occ && (s.last_remind == INT64_MIN || t - s.last_remind >= cfg_.every)) {
                s.last_remind = t;
                if (n < max) out[n++] = {REMIND, i + 1};
            }
        }
        return n;
    }

    // Some moment is past its time, inside its window and untaken (for the LED).
    bool due(int64_t t) const {
        for (int i = 0; i < st_.n; i++)
            if (sl_[i].set && !sl_[i].taken && t >= sl_[i].occ && t <= sl_[i].occ + st_.after) return true;
        return false;
    }

    // How moment k (0-based) stands on the day of t.
    View view(int k, int64_t t) const {
        View v = {st_.minute[k], UPCOMING, 0};
        if (k < 0 || k >= st_.n) return v;
        const Slot &s = sl_[k];
        int64_t day = t / DAY;
        if (s.set && (s.occ / DAY == day || (t >= s.occ - st_.before && t <= s.occ + st_.after))) {
            if (s.taken) { v.state = TAKEN; v.at = s.taken_at; }
            else if (t >= s.occ - st_.before) v.state = DUE;
            return v;
        }
        if (prev_[k].set && prev_[k].occ / DAY == day) {
            if (prev_[k].taken) { v.state = TAKEN; v.at = prev_[k].taken_at; }
            else v.state = MISSED_ST;
        }
        return v;
    }

    // Local minutes → minutes after midnight.
    static int minute_of_day(int64_t t) { return (int)(((t % DAY) + DAY) % DAY); }

private:
    struct Slot {
        bool set = false;      // occ is valid
        int64_t occ = 0;       // local minute of the current occurrence
        bool taken = false;
        int64_t taken_at = 0;
        int64_t last_remind = INT64_MIN;
    };

    // The schedule changed: the per-slot bookkeeping no longer matches it.
    void forget() {
        for (int i = 0; i < MAX_SLOTS; i++) { sl_[i] = Slot(); prev_[i] = Slot(); }
    }

    // The earliest occurrence of moment i whose window has not closed by t.
    int64_t next_occurrence(int i, int64_t t) const {
        int64_t o = (t / DAY) * DAY + st_.minute[i] - DAY;
        while (o + st_.after < t) o += DAY;
        return o;
    }

    // The occurrence of moment i whose window contains t, if any.
    bool occurrence_around(int i, int64_t t, int64_t &out) const {
        int64_t base = (t / DAY) * DAY + st_.minute[i];
        for (int64_t o = base - DAY; o <= base + DAY; o += DAY) {
            if (t >= o - st_.before && t <= o + st_.after) { out = o; return true; }
        }
        return false;
    }

    Config cfg_;
    State st_;
    Slot sl_[MAX_SLOTS];    // current occurrence of each moment
    Slot prev_[MAX_SLOTS];  // the one before it (today's missed/taken after the window closed)
};
