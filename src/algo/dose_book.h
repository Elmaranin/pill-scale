#pragma once
// Classification of a box weight change by learned dose kinds.
// Portable code: no framework dependencies. Tested on the host.
//
// A dose kind is the content of one cell (one pill or several). The scale
// learns kinds by itself, without setup: a new weight is a new kind, a repeat
// confirms the kind. One box may have several kinds: morning/evening, pills.
//
// A limit that weight alone cannot overcome: two small doses may weigh the
// same as one large one. So we first look for a match with a whole single
// kind, and only then for a double dose of a confirmed kind.
#include <math.h>
#include <stdint.h>

class DoseBook {
public:
    static const int MAX_KINDS = 4;

    struct Config {
        double min_delta = 0.035;   // g: below this — "nothing taken" (as in detect)
        double noise_sd = 0.012;    // g: spread of a single lift (series A — 11 mg)
        double rel_tol = 0.10;      // fraction of a dose: pills of one kind differ slightly
        double newbox_frac = 0.5;   // a change above this fraction of the box — another box
        int avg_cap = 20;           // kind mean over the last ~20 doses (learns slowly)
    };

    // Stored in NVS as is: plain types only, no pointers.
    struct Kind { float mean; uint16_t count; };   // weight of one dose, g; times seen
    struct State { uint8_t n; Kind k[MAX_KINDS]; };

    enum Verdict { NONE, DOSE, DOUBLE, REFILL, NEWBOX };
    struct Result {
        Verdict v;
        int kind;     // 1..MAX_KINDS for DOSE and DOUBLE, otherwise 0
        int count;    // times this kind was seen, including this one
        bool fresh;   // new kind (DOSE, count == 1)
    };

    DoseBook() : DoseBook(Config()) {}
    explicit DoseBook(const Config &cfg) : cfg_(cfg) { clear(); }

    void clear() { st_ = State(); st_.n = 0; }
    const State &state() const { return st_; }
    void restore(const State &s) { st_ = s; if (st_.n > MAX_KINDS) st_.n = 0; }

    // delta — box weight change (negative — something taken), box — its weight before.
    // Learns from every dose: updates a kind or creates a new one.
    Result classify(double delta, double box) {
        if (box > 0 && fabs(delta) > cfg_.newbox_frac * box) return {NEWBOX, 0, 0, false};
        if (fabs(delta) < cfg_.min_delta) return {NONE, 0, 0, false};
        if (delta > 0) return {REFILL, 0, 0, false};

        double m = -delta;  // how much was taken, g

        // 1) A whole single kind — the closest of those that fit
        int best = -1;
        double best_err = 0;
        for (int i = 0; i < st_.n; i++) {
            double err = fabs(m - st_.k[i].mean);
            if (err <= tol(st_.k[i].mean) && (best < 0 || err < best_err)) { best = i; best_err = err; }
        }
        if (best >= 0) {
            Kind &k = st_.k[best];
            int n = k.count < cfg_.avg_cap ? k.count + 1 : cfg_.avg_cap;
            k.mean += (float)((m - k.mean) / n);
            if (k.count < UINT16_MAX) k.count++;
            return {DOSE, best + 1, k.count, false};
        }

        // 2) A double dose of a confirmed kind
        for (int i = 0; i < st_.n; i++) {
            if (st_.k[i].count >= 2 && fabs(m - 2 * st_.k[i].mean) <= tol(2 * st_.k[i].mean))
                return {DOUBLE, i + 1, st_.k[i].count, false};
        }

        // 3) A new kind. No room — replace the rarest one.
        int slot = st_.n;
        if (slot == MAX_KINDS) {
            slot = 0;
            for (int i = 1; i < MAX_KINDS; i++)
                if (st_.k[i].count < st_.k[slot].count) slot = i;
        } else {
            st_.n++;
        }
        st_.k[slot] = {(float)m, 1};
        return {DOSE, slot + 1, 1, true};
    }

private:
    // Match tolerance for weight w: lift noise (3σ) plus the spread of the pills themselves.

    double tol(double w) const { return 3 * cfg_.noise_sd + cfg_.rel_tol * w; }

    Config cfg_;
    State st_;
};
