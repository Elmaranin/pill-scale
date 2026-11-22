#pragma once
#include "stats.h"

// This collector averages separate blocks without waiting for new samples.
class SampleBlock {
public:
    static bool valid_size(int n) {
        return n == 1 || n == 5 || n == 10 || n == 15 || n == 30 || n == 50;
    }

    bool configure(int n, uint32_t now_ms) {
        if (!valid_size(n)) return false;
        if (started_ && n == samples_) return true;
        samples_ = n;
        started_ = true;
        start_ms_ = now_ms;
        current_.reset();
        result_.reset();
        duration_ms_ = 0;
        return true;
    }

    bool add(double raw, uint32_t now_ms) {
        if (!started_) return false;
        current_.add(raw);
        if (current_.count() < (uint32_t)samples_) return false;
        result_ = current_;
        duration_ms_ = now_ms - start_ms_;
        current_.reset();
        start_ms_ = now_ms;
        return true;
    }

    int samples() const { return samples_; }
    uint32_t collected() const { return current_.count(); }
    const RunningStats &result() const { return result_; }
    uint32_t duration_ms() const { return duration_ms_; }

private:
    int samples_ = 10;
    bool started_ = false;
    uint32_t start_ms_ = 0, duration_ms_ = 0;
    RunningStats current_, result_;
};
