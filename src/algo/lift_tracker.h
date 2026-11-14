#pragma once
// State machine "box standing → removed → returned" and the change of its weight.
// Portable code: no framework dependencies. Tested on the host against the
// milestone 2 recordings (test/test_lift).
//
// The box weight change is computed with two zeros:
//
//     delta = (R2 − Z2) − (R1 − Z1)
//
// R1 — box before removal, Z1 — zero right after removal, Z2 — zero right before
// return, R2 — box after return. Zero drift while the box is away cancels out,
// however long it was away (docs/experiments.md).
//
// Samples are grouped into ~1 s blocks. A block is calm if its spread is small.
// Only calm blocks go into R1, Z1, Z2, R2 — the hand and bumps never get in.
// Same method as tools/lifts.py, but sample by sample, as in the firmware.
//
// While the box stands we watch the weight too: a pill taken without lifting
// the box settles the weight at a new level (event CHANGED). The reference
// level follows drift slowly; blocks while the box is touched are not calm and
// are skipped; two consecutive calm blocks agreeing with each other but not
// with the reference mean a new weight.
#include <math.h>
#include "stats.h"

class LiftTracker {
public:
    struct Config {
        int window = 10;              // samples per block (~1 s at 10 SPS)
        double calm_sd = 0.05;        // g: a block is calm if its spread is below this
        double min_box = 2.0;         // g: anything lighter is not a box
        int ref_blocks = 3;           // blocks in each of R1, Z1, Z2, R2 (~3 s)
        int settle_blocks = 3;        // skip ~3 s after the return
        int return_timeout = 30;      // blocks: still not calm — "after" not found
        int missing_blocks = 600;     // blocks without the box (~8.5 min) — "box missing"
        double k_sigma = 5.0;         // a difference above k standard errors is not noise
        double min_change = 0.035;    // g: below this it is drift, not an event (as in detect)
        double track = 0.1;           // fraction by which the reference level is pulled
                                      // toward a calm block of the same weight (~10 blocks)
    };

    enum Event { NONE, PLACED, REMOVED, RETURNED, MISSING, CHANGED };

    LiftTracker() : LiftTracker(Config()) {}
    explicit LiftTracker(const Config &cfg) : cfg_(cfg) {}

    // Add a sample in grams. Returns an event if one happened.
    Event add(double g) {
        block_.add(g);
        if ((int)block_.count() < cfg_.window) return NONE;
        Block b = {block_.mean(), block_.variance() / block_.count(),
                   block_.sd() <= cfg_.calm_sd, false};
        block_.reset();
        level_ = b.mean;
        b.loaded = b.mean > threshold();
        Event e = step(b);
        push(hist_, b);
        return e;
    }

    // After RETURNED and CHANGED: box weight before, after, and the change
    double before() const { return r1_; }        // R1, g
    double after() const { return r2_; }         // R2, g
    double delta() const { return delta_; }      // box weight change, g
    bool reliable() const { return reliable_; }  // R2 was found in calm blocks
    double box() const { return base_; }         // current box weight, g
    int off_blocks() const { return off_; }      // how many blocks the box was away
    double level() const { return level_; }      // weight on the platform by the last block, g
    bool box_on() const { return state_ == ON || state_ == BACK; }  // the box is standing

private:
    enum State { NO_BOX, ON, OFF, BACK };
    struct Block { double mean, var; bool calm, loaded; };  // var — variance of the mean
    static const int HIST = 8;
    struct Ring { Block b[HIST]; int n = 0; };  // recent blocks, newest last

    static void push(Ring &r, const Block &b) {
        if (r.n < HIST) { r.b[r.n++] = b; return; }
        for (int i = 1; i < HIST; i++) r.b[i - 1] = r.b[i];
        r.b[HIST - 1] = b;
    }

    // Mean of the last calm blocks of the wanted kind, skipping the `skip` most
    // recent ones (the hand is still near the transition). None found — false.
    bool recent(const Ring &r, bool loaded, int skip, double &out) const {
        double sum = 0;
        int k = 0;
        for (int i = r.n - 1 - skip; i >= 0 && k < cfg_.ref_blocks; i--) {
            if (r.b[i].calm && r.b[i].loaded == loaded) { sum += r.b[i].mean; k++; }
        }
        if (k == 0) return false;
        out = sum / k;
        return true;
    }

    // Threshold "box standing / empty". While the box stands — halfway between
    // zero and its weight: removed when the weight drops below half (margin
    // against bumps). While empty — anything heavier than min_box: a different,
    // lighter box may come back, and half of the old weight would miss it.
    double threshold() const {
        double half = (state_ == ON) ? 0.5 * (base_ - zero_) : cfg_.min_box;
        return zero_ + (half > cfg_.min_box ? half : cfg_.min_box);
    }

    bool same(const Block &x, const Block &y) const {
        return fabs(x.mean - y.mean) <= cfg_.k_sigma * sqrt(x.var + y.var);
    }

    // The box stands: did its weight change (something taken without lifting).
    Event watch(const Block &b) {
        if (!b.calm) { have_cand_ = false; return NONE; }  // being touched — wait
        if (same(b, ref_)) {
            // same weight — slowly pull the reference level along (drift)
            double a = cfg_.track;
            ref_.mean += a * (b.mean - ref_.mean);
            ref_.var = (1 - a) * (1 - a) * ref_.var + a * a * b.var;
            base_ = ref_.mean;
            have_cand_ = false;
            return NONE;
        }
        if (!have_cand_ || !same(b, cand_)) { cand_ = b; have_cand_ = true; return NONE; }
        // two consecutive calm blocks at the new level — the weight has settled
        have_cand_ = false;
        double d = b.mean - ref_.mean;
        r1_ = ref_.mean;
        ref_ = b;
        base_ = b.mean;
        if (fabs(d) < cfg_.min_change) return NONE;  // too small — drift, no event
        r2_ = b.mean;
        delta_ = d;
        reliable_ = true;
        return CHANGED;
    }

    Event step(const Block &b) {
        switch (state_) {
        case NO_BOX:
            if (!b.loaded) {
                calm_on_ = 0;
                if (b.calm) zero_ = b.mean;  // empty and calm — a fresh zero
                return NONE;
            }
            if (!b.calm) { calm_on_ = 0; return NONE; }
            if (++calm_on_ < 2) return NONE;  // two consecutive calm blocks — it stands
            base_ = b.mean;
            ref_ = {b.mean, b.var, true, true};
            have_cand_ = false;
            state_ = ON;
            off_ = -1;
            return PLACED;

        case ON:
            if (b.loaded) return watch(b);
            // Removed. "Before" — the last calm blocks with the box, except the
            // most recent one (the hand is already in it).
            if (!recent(hist_, true, 1, r1_) && !recent(hist_, true, 0, r1_)) r1_ = base_;
            state_ = OFF;
            off_ = 1;
            z1_n_ = 0;
            z1_sum_ = 0;
            empty_ = Ring();
            return REMOVED;

        case OFF:
            if (!b.loaded) {
                off_++;
                // Z1 — the first calm blocks, skipping the first empty one (the hand leaves)
                if (b.calm && off_ > 2 && z1_n_ < cfg_.ref_blocks) { z1_sum_ += b.mean; z1_n_++; }
                push(empty_, b);
                if (off_ == cfg_.missing_blocks) return MISSING;
                return NONE;
            }
            // Returned. Z2 — the last calm empty blocks, except the most recent one.
            z1_ = z1_n_ ? z1_sum_ / z1_n_ : zero_;
            if (!recent(empty_, false, 1, z2_) && !recent(empty_, false, 0, z2_)) z2_ = z1_;
            if (!z1_n_) z1_ = z2_;  // empty for a very short time — one zero for both
            zero_ = z2_;
            state_ = BACK;
            back_ = 0;
            r2_n_ = 0;
            r2_sum_ = 0;
            return NONE;

        case BACK:
            if (!b.loaded) {
                // Put down and immediately lifted again — continue the same lift
                state_ = OFF;
                off_++;
                push(empty_, b);
                return NONE;
            }
            back_++;
            if (back_ > cfg_.settle_blocks && b.calm) { r2_sum_ += b.mean; r2_n_++; }
            if (r2_n_ < cfg_.ref_blocks && back_ < cfg_.settle_blocks + cfg_.return_timeout)
                return NONE;
            reliable_ = r2_n_ > 0;
            r2_ = reliable_ ? r2_sum_ / r2_n_ : b.mean;
            delta_ = (r2_ - z2_) - (r1_ - z1_);
            base_ = r2_;
            ref_ = {r2_, b.var, true, true};
            have_cand_ = false;
            state_ = ON;
            return RETURNED;
        }
        return NONE;
    }

    Config cfg_;
    RunningStats block_;
    Ring hist_, empty_;
    Block ref_ = {0, 0, true, true}, cand_ = {0, 0, true, true};  // while the box stands
    bool have_cand_ = false;
    State state_ = NO_BOX;
    double zero_ = 0, base_ = 0, level_ = 0;
    double r1_ = 0, z1_ = 0, z2_ = 0, r2_ = 0, delta_ = 0;
    double z1_sum_ = 0, r2_sum_ = 0;
    int z1_n_ = 0, r2_n_ = 0;
    int calm_on_ = 0, back_ = 0;
    int off_ = -1;  // blocks without the box since removal; −1 — not counting

    bool reliable_ = false;
};
