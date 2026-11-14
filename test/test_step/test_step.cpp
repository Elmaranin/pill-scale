// Host test: see test/CMakeLists.txt
// Synthetic data: noise ~0.02 g per sample, like the HX711 on this scale (see
// the stream on 10 / 40 recording). Real CSV tests live in test_lift.
#include <unity.h>
#include <stdint.h>
#include "step_detector.h"

void setUp() {}
void tearDown() {}

// Repeatable "Gaussian" noise: sum of 12 uniforms minus 6, sd = 1.
static uint32_t seed;
static double noise(double sd) {
    double s = 0;
    for (int i = 0; i < 12; i++) {
        seed = seed * 1664525u + 1013904223u;
        s += (seed >> 8) / 16777216.0;
    }
    return (s - 6.0) * sd;
}

struct Counts { int added = 0, removed = 0; double last_delta = 0; };

// Feed n samples at level g (+ noise and linear drift), count the events.
static void feed(StepDetector &d, Counts &c, int n, double g,
                 double sd = 0.02, double drift_per_sample = 0) {
    for (int i = 0; i < n; i++) {
        double x = g + noise(sd) + drift_per_sample * i;
        switch (d.add(x)) {
            case StepDetector::ADDED: c.added++; c.last_delta = d.delta(); break;
            case StepDetector::REMOVED: c.removed++; c.last_delta = d.delta(); break;
            default: break;
        }
    }
}

static void test_noise_only_no_events() {
    seed = 1;
    StepDetector d; Counts c;
    feed(d, c, 6000, 108.6);  // 10 minutes at 10 SPS
    TEST_ASSERT_EQUAL_INT(0, c.added + c.removed);
}

static void test_pill_added_and_removed() {
    seed = 2;
    StepDetector d; Counts c;
    feed(d, c, 100, 108.6);
    feed(d, c, 100, 108.8);   // 0.2 g added
    TEST_ASSERT_EQUAL_INT(1, c.added);
    TEST_ASSERT_DOUBLE_WITHIN(0.02, 0.2, c.last_delta);
    feed(d, c, 100, 108.6);   // removed
    TEST_ASSERT_EQUAL_INT(1, c.removed);
    TEST_ASSERT_DOUBLE_WITHIN(0.02, -0.2, c.last_delta);
}

static void test_tiny_change_ignored() {
    seed = 3;
    StepDetector d; Counts c;
    feed(d, c, 100, 108.6);
    feed(d, c, 100, 108.62);  // 0.02 g — below min_delta
    TEST_ASSERT_EQUAL_INT(0, c.added + c.removed);
}

// A light pill lands mid-block, gradually (~0.3 s). Such a step used to be
// "eaten" piece by piece: only 40% caught at 0.07 g. Check every phase.
static void test_light_pill_any_phase() {
    for (int phase = 0; phase < 10; phase++) {
        for (uint32_t k = 1; k <= 20; k++) {
            seed = k * 7919 + phase;
            StepDetector d; Counts c;
            feed(d, c, 100 + phase, 108.6);
            for (int i = 1; i <= 3; i++) feed(d, c, 1, 108.6 + 0.07 * i / 4);
            feed(d, c, 150, 108.67);
            TEST_ASSERT_EQUAL_INT(1, c.added);
            TEST_ASSERT_EQUAL_INT(0, c.removed);
            TEST_ASSERT_DOUBLE_WITHIN(0.025, 0.07, c.last_delta);
        }
    }
}

static void test_slow_drift_ignored() {
    seed = 4;
    StepDetector d; Counts c;
    // −0.012 g/min, as recorded after placing a weight: −0.12 g over 10 minutes
    feed(d, c, 6000, 108.6, 0.02, -0.012 / 600);
    TEST_ASSERT_EQUAL_INT(0, c.added + c.removed);
}

static void test_touch_without_change_ignored() {
    seed = 5;
    StepDetector d; Counts c;
    feed(d, c, 100, 108.6);
    feed(d, c, 20, 108.6, 2.0);  // the platform is being touched: large spread
    feed(d, c, 100, 108.6);
    TEST_ASSERT_EQUAL_INT(0, c.added + c.removed);
}

static void test_glitch_ignored() {
    seed = 6;
    StepDetector d; Counts c;
    feed(d, c, 100, 108.6);
    d.add(-117.4);               // a single garbage HX711 sample

    feed(d, c, 100, 108.6);
    TEST_ASSERT_EQUAL_INT(0, c.added + c.removed);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_noise_only_no_events);
    RUN_TEST(test_pill_added_and_removed);
    RUN_TEST(test_tiny_change_ignored);
    RUN_TEST(test_light_pill_any_phase);
    RUN_TEST(test_slow_drift_ignored);
    RUN_TEST(test_touch_without_change_ignored);
    RUN_TEST(test_glitch_ignored);
    return UNITY_END();
}
