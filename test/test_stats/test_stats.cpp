// Host test: see test/CMakeLists.txt
#include <unity.h>
#include "stats.h"

void setUp() {}
void tearDown() {}

static void test_empty() {
    RunningStats s;
    TEST_ASSERT_EQUAL_UINT32(0, s.count());
    TEST_ASSERT_EQUAL_DOUBLE(0.0, s.sd());
}

static void test_known_values() {
    // 2,4,4,4,5,5,7,9: mean 5, sample sd = sqrt(32/7)
    RunningStats s;
    const double xs[] = {2, 4, 4, 4, 5, 5, 7, 9};
    for (double x : xs) s.add(x);
    TEST_ASSERT_DOUBLE_WITHIN(1e-9, 5.0, s.mean());
    TEST_ASSERT_DOUBLE_WITHIN(1e-9, sqrt(32.0 / 7.0), s.sd());
}

static void test_large_offset() {
    // Typical raw counts: large offset, small spread.
    // The naive formula loses precision here, Welford does not.

    RunningStats s;
    const double base = 8000000.0;
    const double xs[] = {1, 2, 3, 4, 5};
    for (double x : xs) s.add(base + x);
    TEST_ASSERT_DOUBLE_WITHIN(1e-6, base + 3.0, s.mean());
    TEST_ASSERT_DOUBLE_WITHIN(1e-6, sqrt(2.5), s.sd());
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_empty);
    RUN_TEST(test_known_values);
    RUN_TEST(test_large_offset);
    return UNITY_END();
}
