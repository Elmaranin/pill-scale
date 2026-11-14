#pragma once
// Portable code: no framework dependencies. Tested on the host.
#include <math.h>
#include <stdint.h>

// Running mean and standard deviation (Welford's algorithm).
// Does not store the samples and does not lose precision on large numbers,
// unlike the naive "sum of squares minus square of sum" — and raw ADC counts
// are large (hundreds of thousands).

class RunningStats {
public:
    void reset() { n_ = 0; mean_ = 0; m2_ = 0; }

    void add(double x) {
        n_++;
        double d = x - mean_;
        mean_ += d / n_;
        m2_ += d * (x - mean_);
    }

    uint32_t count() const { return n_; }
    double mean() const { return mean_; }
    double variance() const { return n_ > 1 ? m2_ / (n_ - 1) : 0.0; }
    double sd() const { return sqrt(variance()); }

private:
    uint32_t n_ = 0;
    double mean_ = 0;
    double m2_ = 0;
};
