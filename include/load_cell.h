#pragma once
#include <stdint.h>

// Common interface of the load-cell ADC.
// Every ADC driver implements it the same way; the logic above does not know
// which chip is installed.
//
// The driver returns raw counts only. Conversion to grams, tare and
// calibration live in Scale (src/scale.*) so they are not duplicated per driver.
class LoadCell {
public:
    virtual ~LoadCell() = default;

    // Initialise. false — the ADC does not respond.
    virtual bool begin() = 0;

    // Is a new sample ready. Non-blocking.
    virtual bool ready() = 0;

    // Read one sample. Call only after ready() == true.
    virtual int32_t read() = 0;

    // Driver name for INFO.
    virtual const char *name() const = 0;

    // Nominal sample rate, samples per second.
    virtual int sps() const = 0;
};

// Factory: implemented in exactly one *_cell.cpp, selected at build time.
LoadCell &load_cell_instance();
