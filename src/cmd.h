#pragma once
#include "scale.h"
#include "algo/step_detector.h"
#include "algo/lift_tracker.h"
#include "algo/dose_book.h"

// Text protocol over USB. Description: docs/api.md.
// Changes go in this order: the document, then the code, then tools/scale.py.
class CommandLine {
public:
    explicit CommandLine(Scale &scale) : scale_(scale) {}

    // Call from the main loop. Non-blocking: reads the port, executes complete
    // lines, and feeds the new sample to the stream or the detector if enabled.
    void poll();

    // Start the lift tracker (if the scale is calibrated). Called at power-on so
    // events are journaled without a computer.
    void begin_tracking();

private:
    void read_input();
    void stream(int32_t raw);
    void detect(int32_t raw);
    void track(int32_t raw);
    void live(int32_t raw);      // current weight for the "Scale" tab
    void judge(uint16_t off_s);  // classify returned/changed by learned doses → journal
    void snapshot();             // state for the web page
    void load_doses();
    void save_doses();
    void execute(char *line);

    Scale &scale_;
    char buf_[96];
    int len_ = 0;

    bool streaming_ = false;
    int stream_n_ = 1;      // samples averaged into one stream line
    RunningStats block_;    // samples of the current block

    bool detecting_ = false;
    StepDetector detector_;

    bool tracking_ = false;   // lift tracker running
    bool print_events_ = false;  // event on: print E events to USB
    uint32_t status_s_ = 0;   // when the weight was last journaled (every 10 min)
    int snap_n_ = 0;
    LiftTracker tracker_;
    uint32_t removed_ms_ = 0; // when the box was removed — for off_s
    DoseBook doses_;          // learned dose kinds, stored in NVS

    static const int LIVE_N = 8;  // samples averaged for the "Scale" tab (~0.7 s)
    int32_t live_raw_[LIVE_N] = {};
    int live_i_ = 0, live_n_ = 0;
    bool doses_loaded_ = false;
};
