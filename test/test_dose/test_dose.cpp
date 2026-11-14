// Host test: see test/CMakeLists.txt
// Lift classification by learned doses: real milestone 2 recordings through the
// tracker (lift_tracker) and the dose book (dose_book), plus synthetic rare cases.
#include <unity.h>
#include <stdio.h>
#include <vector>
#include "lift_tracker.h"
#include "dose_book.h"

void setUp() {}
void tearDown() {}

// Collect the classification of every lift in a file.
static std::vector<DoseBook::Result> replay(const char *name, DoseBook &book) {
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", PILLSCALE_DATA_DIR, name);
    FILE *f = fopen(path, "r");
    TEST_ASSERT_NOT_NULL_MESSAGE(f, path);
    LiftTracker t;
    std::vector<DoseBook::Result> out;
    char line[256];
    long ms;
    double raw, g;
    while (fgets(line, sizeof(line), f)) {
        if (sscanf(line, "%ld,%lf,%lf", &ms, &raw, &g) != 3) continue;
        double box = t.box();  // box weight before the lift
        if (t.add(g) == LiftTracker::RETURNED) out.push_back(book.classify(t.delta(), box));
    }
    fclose(f);
    return out;
}

static void test_series_a_all_none() {
    DoseBook book;
    for (const DoseBook::Result &r : replay("2026-10-21_lifts_a_no_pill.csv", book))
        TEST_ASSERT_EQUAL_INT(DoseBook::NONE, r.v);
    TEST_ASSERT_EQUAL_INT(0, book.state().n);
}

// Series B: three pill kinds in the box — large (~0.54), small (~0.14) and the
// smallest (~0.08, lift #12). Every lift is one dose, no doubles.
static void test_series_b_three_kinds() {
    DoseBook book;
    std::vector<DoseBook::Result> v = replay("2026-10-21_lifts_b_one_pill.csv", book);
    TEST_ASSERT_EQUAL_INT(22, (int)v.size());
    int fresh = 0;
    for (const DoseBook::Result &r : v) {
        TEST_ASSERT_EQUAL_INT(DoseBook::DOSE, r.v);
        if (r.fresh) fresh++;
    }
    TEST_ASSERT_EQUAL_INT(3, fresh);  // "new kind" three times
    const DoseBook::State &s = book.state();
    TEST_ASSERT_EQUAL_INT(3, s.n);
    // order of appearance: large (lift 1), small (2), smallest (12)
    TEST_ASSERT_DOUBLE_WITHIN(0.02, 0.544, s.k[0].mean);
    TEST_ASSERT_EQUAL_INT(5, s.k[0].count);
    TEST_ASSERT_DOUBLE_WITHIN(0.01, 0.143, s.k[1].mean);
    TEST_ASSERT_EQUAL_INT(16, s.k[1].count);
    TEST_ASSERT_DOUBLE_WITHIN(0.01, 0.076, s.k[2].mean);
    TEST_ASSERT_EQUAL_INT(1, s.k[2].count);
}

static void test_double_refill_newbox() {
    DoseBook book;
    for (int i = 0; i < 3; i++) book.classify(-0.140, 27.0);  // learned a 0.14 dose
    DoseBook::Result r = book.classify(-0.285, 27.0);
    TEST_ASSERT_EQUAL_INT(DoseBook::DOUBLE, r.v);
    TEST_ASSERT_EQUAL_INT(1, r.kind);
    TEST_ASSERT_EQUAL_INT(DoseBook::REFILL, book.classify(+4.9, 27.0).v);
    TEST_ASSERT_EQUAL_INT(DoseBook::NONE, book.classify(-0.020, 27.0).v);
    TEST_ASSERT_EQUAL_INT(DoseBook::NONE, book.classify(+0.030, 27.0).v);
    TEST_ASSERT_EQUAL_INT(DoseBook::NEWBOX, book.classify(-20.0, 27.0).v);
}

// Double only for a confirmed kind: 0.14 seen once, then 0.28 — that is a new
// kind, not a "double".
static void test_double_needs_confirmed_kind() {
    DoseBook book;
    book.classify(-0.140, 27.0);
    DoseBook::Result r = book.classify(-0.280, 27.0);
    TEST_ASSERT_EQUAL_INT(DoseBook::DOSE, r.v);
    TEST_ASSERT_TRUE(r.fresh);
}

static void test_full_book_replaces_rarest() {
    DoseBook book;
    const double w[] = {0.10, 0.30, 0.60, 1.00};
    for (double x : w) book.classify(-x, 50.0);
    for (int i = 0; i < 3; i++) { book.classify(-0.10, 50.0); book.classify(-0.30, 50.0); book.classify(-1.00, 50.0); }
    // the fifth kind replaces 0.60 (seen once). Not 2.0: that is a double of 1.00
    DoseBook::Result r = book.classify(-3.0, 50.0);
    TEST_ASSERT_TRUE(r.fresh);
    TEST_ASSERT_EQUAL_INT(3, r.kind);
    TEST_ASSERT_DOUBLE_WITHIN(0.001, 3.0, book.state().k[2].mean);
}

static void test_state_roundtrip() {
    DoseBook a;
    a.classify(-0.140, 27.0);
    a.classify(-0.540, 27.0);
    DoseBook::State s = a.state();  // goes to NVS and back the same way

    DoseBook b;
    b.restore(s);
    DoseBook::Result r = b.classify(-0.138, 27.0);
    TEST_ASSERT_EQUAL_INT(DoseBook::DOSE, r.v);
    TEST_ASSERT_EQUAL_INT(1, r.kind);
    TEST_ASSERT_FALSE(r.fresh);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_series_a_all_none);
    RUN_TEST(test_series_b_three_kinds);
    RUN_TEST(test_double_refill_newbox);
    RUN_TEST(test_double_needs_confirmed_kind);
    RUN_TEST(test_full_book_replaces_rarest);
    RUN_TEST(test_state_roundtrip);
    return UNITY_END();
}
