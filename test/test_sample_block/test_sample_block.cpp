#include <unity.h>
#include <initializer_list>
#include "sample_block.h"

void setUp() {}
void tearDown() {}

static void test_presets_and_timing() {
    for (int n : {1, 5, 10, 15, 30, 50}) {
        SampleBlock block;
        TEST_ASSERT_TRUE(block.configure(n, 1000));
        for (int i = 1; i <= n; i++) {
            TEST_ASSERT_EQUAL(i == n, block.add(8000000 + i, 1000 + i * 100));
            TEST_ASSERT_EQUAL_UINT32(i % n, block.collected());
        }
        TEST_ASSERT_EQUAL_UINT32(n, block.result().count());
        TEST_ASSERT_DOUBLE_WITHIN(1e-6, 8000000 + (n + 1) / 2.0, block.result().mean());
        TEST_ASSERT_EQUAL_UINT32(n * 100, block.duration_ms());
    }
}

static void test_blocks_do_not_overlap() {
    SampleBlock block;
    block.configure(5, 0);
    for (int i = 1; i <= 5; i++) block.add(i, i * 100);
    TEST_ASSERT_EQUAL_DOUBLE(3, block.result().mean());
    TEST_ASSERT_DOUBLE_WITHIN(1e-9, sqrt(2.5), block.result().sd());
    TEST_ASSERT_FALSE(block.add(100, 600));
    TEST_ASSERT_EQUAL_DOUBLE(3, block.result().mean());
    TEST_ASSERT_EQUAL_UINT32(1, block.collected());
    for (int i = 7; i <= 10; i++) block.add(100, i * 100);
    TEST_ASSERT_EQUAL_DOUBLE(100, block.result().mean());
    TEST_ASSERT_EQUAL_DOUBLE(0, block.result().sd());
    TEST_ASSERT_EQUAL_UINT32(500, block.duration_ms());
}

static void test_changing_size_discards_old_samples() {
    SampleBlock block;
    block.configure(5, 0);
    for (int i = 1; i <= 5; i++) block.add(999, i * 100);
    block.add(999, 600);
    TEST_ASSERT_TRUE(block.configure(10, 650));
    TEST_ASSERT_EQUAL_UINT32(0, block.result().count());
    TEST_ASSERT_EQUAL_UINT32(0, block.collected());
    TEST_ASSERT_EQUAL_UINT32(0, block.duration_ms());
    for (int i = 1; i <= 10; i++) block.add(42, 650 + i * 100);
    TEST_ASSERT_EQUAL_DOUBLE(42, block.result().mean());
    TEST_ASSERT_EQUAL_UINT32(1000, block.duration_ms());
}

static void test_rejected_and_unchanged_settings_keep_progress() {
    SampleBlock block;
    TEST_ASSERT_FALSE(block.add(99, 0));
    block.configure(5, 0);
    block.add(2, 100);
    for (int n : {-1, 0, 2, 8, 51, 256}) TEST_ASSERT_FALSE(block.configure(n, 100));
    TEST_ASSERT_TRUE(block.configure(5, 100));
    TEST_ASSERT_EQUAL_INT(5, block.samples());
    TEST_ASSERT_EQUAL_UINT32(1, block.collected());
    for (int i = 2; i <= 5; i++) block.add(2, i * 100);
    TEST_ASSERT_EQUAL_DOUBLE(2, block.result().mean());
    TEST_ASSERT_EQUAL_UINT32(500, block.duration_ms());
}

static void test_timer_wrap() {
    SampleBlock block;
    block.configure(1, UINT32_MAX - 49);
    TEST_ASSERT_TRUE(block.add(123, 50));
    TEST_ASSERT_EQUAL_UINT32(100, block.duration_ms());
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_presets_and_timing);
    RUN_TEST(test_blocks_do_not_overlap);
    RUN_TEST(test_changing_size_discards_old_samples);
    RUN_TEST(test_rejected_and_unchanged_settings_keep_progress);
    RUN_TEST(test_timer_wrap);
    return UNITY_END();
}
