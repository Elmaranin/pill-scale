#pragma once
#include <stdint.h>
#include "load_cell.h"
#include "algo/stats.h"

// Raw counts to grams: grams = (raw - offset) / scale.
// offset — raw value of the empty platform (tare).
// scale  — counts per gram (calibration with a known weight).
// Both are stored in NVS and survive a reboot.
class Scale {
public:
    explicit Scale(LoadCell &cell) : cell_(cell) {}

    bool begin();

    // Collect n samples (blocks for ~n/sps seconds).
    // false — the ADC did not respond in time.
    bool collect(int n, RunningStats &out);

    // Zero at the current weight.
    bool tare(int n);

    // Calibrate with a weight of known_g grams. Requires tare() first.
    bool calibrate(float known_g, int n);

    void reset_calibration();

    float to_grams(double raw) const { return (float)((raw - offset_) / scale_); }

    int32_t offset() const { return offset_; }
    float scale() const { return scale_; }
    LoadCell &cell() { return cell_; }

private:
    void save();

    LoadCell &cell_;
    int32_t offset_ = 0;
    float scale_ = 1.0f;
};
