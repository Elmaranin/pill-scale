#pragma once
// Detector of "the weight moved from one settled level to another".
// Portable code: no framework dependencies. Tested on the host.
//
// Idea: compare the weight before and after a change, seconds apart, rather
// than absolute weight hours apart (load-cell drift exceeds a pill).
//
// The reference level ("before") follows drift slowly; blocks that clearly
// differ from it do not move it. Otherwise a light pill dropped mid-block gets
// "eaten" piece by piece: each piece smaller than min_delta.
#include <math.h>
#include "stats.h"

class StepDetector {
public:
    struct Config {
        int window = 10;          // samples per block (~1 s at 10 SPS)
        double calm_sd = 0.05;    // g: a block is calm if its spread is below this
        double k_sigma = 5.0;     // a difference above k standard errors is not noise
        double min_delta = 0.035; // g: an event is at least this — not a speck of dust.
                                  // ~half of the lightest pill (0.07 g);
                                  // k_sigma guards against noise, not this
        double track = 0.05;      // fraction by which the reference level is pulled
                                  // toward a calm block of the same weight (~20 blocks)
    };

    enum Event { NONE, ADDED, REMOVED };

    StepDetector() : StepDetector(Config()) {}
    explicit StepDetector(const Config &cfg) : cfg_(cfg) {}

    void reset() {
        block_.reset();
        have_ref_ = have_cand_ = false;
    }

    // Add a sample in grams. Returns an event if the weight changed.
    Event add(double g) {
        block_.add(g);
        if ((int)block_.count() < cfg_.window) return NONE;

        Level b = {block_.mean(), block_.variance() / block_.count()};
        bool calm = block_.sd() <= cfg_.calm_sd;
        block_.reset();

        // The platform is being touched — wait until it calms down.
        if (!calm) { have_cand_ = false; return NONE; }

        if (!have_ref_) { ref_ = b; have_ref_ = true; return NONE; }

        // Same weight: slowly pull the reference level along (drift).
        if (same(b, ref_)) {
            double a = cfg_.track;
            ref_.mean += a * (b.mean - ref_.mean);
            ref_.var = (1 - a) * (1 - a) * ref_.var + a * a * b.var;
            have_cand_ = false;
            return NONE;
        }

        // Different weight. The event fires when two consecutive calm blocks
        // agree with each other: the weight has settled at a new level.
        if (!have_cand_ || !same(b, cand_)) { cand_ = b; have_cand_ = true; return NONE; }
        have_cand_ = false;

        double d = b.mean - ref_.mean;  // the last block: only the new weight in it
        ref_ = b;
        if (fabs(d) < cfg_.min_delta) return NONE;  // a real change, but too small
        delta_ = d;
        return d > 0 ? ADDED : REMOVED;
    }

    double delta() const { return delta_; }      // last change, g
    double level() const { return ref_.mean; }   // current settled weight, g

private:
    // Level: mean and the variance of that mean (sd² / n).
    struct Level { double mean, var; };

    // Are two levels indistinguishable given the noise.

    bool same(const Level &x, const Level &y) const {
        return fabs(x.mean - y.mean) <= cfg_.k_sigma * sqrt(x.var + y.var);
    }

    Config cfg_;
    RunningStats block_;
    Level ref_ = {0, 0}, cand_ = {0, 0};
    bool have_ref_ = false, have_cand_ = false;
    double delta_ = 0;
};
