// Host test: see test/CMakeLists.txt
// Lift tracker on the real milestone 2 recordings (docs/data/) — the same ones
// analysed by tools/lifts.py in docs/experiments.md.
#include <unity.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <vector>
#include "lift_tracker.h"

void setUp() {}
void tearDown() {}

struct Lift { double delta; bool reliable; };

// Run a recording through the tracker, collect every "returned".
static std::vector<Lift> replay(const char *name, int *placed = nullptr, int *changed = nullptr) {
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", PILLSCALE_DATA_DIR, name);
    FILE *f = fopen(path, "r");
    TEST_ASSERT_NOT_NULL_MESSAGE(f, path);

    LiftTracker t;
    std::vector<Lift> out;
    char line[256];
    long ms;
    double raw, g;
    while (fgets(line, sizeof(line), f)) {
        if (sscanf(line, "%ld,%lf,%lf", &ms, &raw, &g) != 3) continue;  // # lines and header
        LiftTracker::Event e = t.add(g);
        if (e == LiftTracker::RETURNED) out.push_back({t.delta(), t.reliable()});
        if (e == LiftTracker::PLACED && placed) (*placed)++;
        if (e == LiftTracker::CHANGED && changed) (*changed)++;
    }
    fclose(f);
    return out;
}

// Series A: 16 lifts, nothing taken. All must be ~0.
static void test_series_a_nothing_taken() {
    int placed = 0, changed = 0;
    std::vector<Lift> v = replay("2026-10-21_lifts_a_no_pill.csv", &placed, &changed);
    TEST_ASSERT_EQUAL_INT(1, placed);
    TEST_ASSERT_EQUAL_INT(0, changed);  // nothing changed while the box stood
    TEST_ASSERT_EQUAL_INT(16, (int)v.size());
    for (const Lift &l : v) {
        TEST_ASSERT_TRUE(l.reliable);
        TEST_ASSERT_DOUBLE_WITHIN(0.035, 0.0, l.delta);  // within the threshold
    }
}

// Series B: 22 lifts with one pill each: 16 small (~0.14 g), 5 large
// (~0.54 g) and one −0.076 g. None looks like "nothing".
static void test_series_b_one_pill() {
    int changed = 0;
    std::vector<Lift> v = replay("2026-10-21_lifts_b_one_pill.csv", nullptr, &changed);
    TEST_ASSERT_EQUAL_INT(22, (int)v.size());
    TEST_ASSERT_EQUAL_INT(0, changed);  // pills were taken only with a lift
    int small = 0, big = 0, other = 0;
    for (const Lift &l : v) {
        TEST_ASSERT_TRUE(l.reliable);
        TEST_ASSERT_TRUE(l.delta < -0.035);
        if (l.delta > -0.20 && l.delta < -0.10) small++;
        else if (l.delta > -0.60 && l.delta < -0.48) big++;
        else other++;
    }
    TEST_ASSERT_EQUAL_INT(16, small);
    TEST_ASSERT_EQUAL_INT(5, big);
    TEST_ASSERT_EQUAL_INT(1, other);
}

// Synthetic: the box was removed and not returned — "missing" exactly once;
// returned later — the change is still computed.
static void test_missing_then_returned() {
    LiftTracker::Config c;
    c.missing_blocks = 50;
    LiftTracker t(c);
    int missing = 0, returned = 0;
    double delta = 0;
    // g0 → g1 linearly over n samples: drift is gradual, as in the recordings
    auto feed = [&](int n, double g0, double g1) {
        for (int i = 0; i < n; i++) {
            double x = g0 + (g1 - g0) * i / n + 0.01 * sin(i * 1.7);  // + a little "noise"
            LiftTracker::Event e = t.add(x);
            if (e == LiftTracker::MISSING) missing++;
            if (e == LiftTracker::RETURNED) { returned++; delta = t.delta(); }
        }
    };
    feed(100, 0.0, 0.0);     // empty platform
    feed(100, 30.0, 30.0);   // box placed
    // Removed and forgotten; the zero drifted by 0.1 g in ~85 s (~0.07 g/min — as
    // with an open window). Two zeros cancel the drift over the whole pause; only
    // the drift over the seconds between the transition and the zero window
    // remains — here ~0.006 g.
    feed(1000, 0.0, 0.1);
    TEST_ASSERT_EQUAL_INT(1, missing);
    feed(200, 30.0, 30.0);   // returned minus a 0.1 g dose: 30 − 0.1 + 0.1 (zero shift)
    TEST_ASSERT_EQUAL_INT(1, returned);
    TEST_ASSERT_DOUBLE_WITHIN(0.01, -0.1, delta);
}

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

struct Counts { int changed = 0, other = 0; double delta = 0; };

static void feed(LiftTracker &t, Counts &c, int n, double g, double sd = 0.013, double drift = 0) {
    for (int i = 0; i < n; i++) {
        LiftTracker::Event e = t.add(g + noise(sd) + drift * i);
        if (e == LiftTracker::CHANGED) { c.changed++; c.delta = t.delta(); }
        else if (e == LiftTracker::REMOVED || e == LiftTracker::RETURNED) c.other++;
    }
}

// Step 3: a pill taken without lifting the box. The hand touches the box for
// ~2 s (large spread), then the weight is 0.14 g lower. The pill is taken at
// any moment of a block — check every phase.
static void test_take_without_lifting() {
    for (int phase = 0; phase < 10; phase++) {
        seed = 100 + phase;
        LiftTracker t; Counts c;
        feed(t, c, 50, 0.0);              // empty platform
        feed(t, c, 200 + phase, 26.4);    // box placed, standing
        feed(t, c, 20, 26.4, 1.5);        // cell opened, hand touching
        feed(t, c, 150, 26.26);           // 0.14 g taken
        TEST_ASSERT_EQUAL_INT(1, c.changed);
        TEST_ASSERT_EQUAL_INT(0, c.other);  // this is not a lift
        TEST_ASSERT_DOUBLE_WITHIN(0.02, -0.14, c.delta);
    }
}


// A different, lighter box after a heavy object. The "standing / empty"
// threshold must not depend on the previous object's weight: 100 g removed, a
// 27 g pillbox placed — below half of 100 g, and the tracker never saw it.
static void test_lighter_box_after_heavy_one() {
    seed = 7;
    LiftTracker t; Counts c;
    int placed = 0, returned = 0;
    auto run = [&](int n, double g) {
        for (int i = 0; i < n; i++) {
            LiftTracker::Event e = t.add(g + noise(0.013));
            if (e == LiftTracker::PLACED) placed++;
            if (e == LiftTracker::RETURNED) returned++;
        }
    };
    run(50, 0.0);
    run(100, 100.0);   // a weight: placed
    run(100, 0.0);     // removed
    run(100, 27.0);    // the pillbox: must be noticed (returned, classified as newbox)
    TEST_ASSERT_EQUAL_INT(1, placed);
    TEST_ASSERT_EQUAL_INT(1, returned);
    run(100, 0.0);     // and the tracker stays alive: lift and return of the same box
    run(100, 26.86);
    TEST_ASSERT_EQUAL_INT(2, returned);
    TEST_ASSERT_DOUBLE_WITHIN(0.02, -0.14, t.delta());
}

// Drift while the box stands is not an event. 0.05 g/min, like warm-up in series A.
static void test_drift_on_platform_no_event() {
    seed = 7;
    LiftTracker t; Counts c;
    feed(t, c, 50, 0.0);
    feed(t, c, 7000, 26.4, 0.013, 0.05 / 700);  // 10 minutes at 11.7 SPS
    TEST_ASSERT_EQUAL_INT(0, c.changed);
}

// Box touched, nothing taken — not an event.

static void test_touch_without_take_no_event() {
    seed = 8;
    LiftTracker t; Counts c;
    feed(t, c, 50, 0.0);
    feed(t, c, 200, 26.4);
    for (int i = 0; i < 5; i++) { feed(t, c, 15, 26.4, 2.0); feed(t, c, 100, 26.4); }
    TEST_ASSERT_EQUAL_INT(0, c.changed);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_series_a_nothing_taken);
    RUN_TEST(test_series_b_one_pill);
    RUN_TEST(test_missing_then_returned);
    RUN_TEST(test_take_without_lifting);
    RUN_TEST(test_lighter_box_after_heavy_one);
    RUN_TEST(test_drift_on_platform_no_event);
    RUN_TEST(test_touch_without_take_no_event);
    return UNITY_END();
}
