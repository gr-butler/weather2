#include <unity.h>

#include "buffer.h"

// Ported from weather/buffer/buffer_test.go to verify the C++ SampleBuffer
// matches the Go reference behaviour exactly.

void test_add_item_fills_and_averages(void) {
    SampleBuffer buf(10);

    for (int i = 0; i < 10; i++) {
        buf.addItem(1);
    }

    BufferStats s = buf.getAverageMinMaxSum();
    TEST_ASSERT_EQUAL_DOUBLE(1.0, s.average);
    TEST_ASSERT_EQUAL_DOUBLE(1.0, s.minimum);
    TEST_ASSERT_EQUAL_DOUBLE(1.0, s.maximum);
    TEST_ASSERT_EQUAL_DOUBLE(10.0, s.sum);

    buf.addItem(10);
    s = buf.getAverageMinMaxSum();
    TEST_ASSERT_EQUAL_DOUBLE(1.9, s.average);
    TEST_ASSERT_EQUAL_DOUBLE(1.0, s.minimum);
    TEST_ASSERT_EQUAL_DOUBLE(10.0, s.maximum);
    TEST_ASSERT_EQUAL_DOUBLE(19.0, s.sum);

    buf.addItem(5);
    s = buf.getAverageMinMaxSum();
    TEST_ASSERT_EQUAL_DOUBLE(2.3, s.average);
    TEST_ASSERT_EQUAL_DOUBLE(1.0, s.minimum);
    TEST_ASSERT_EQUAL_DOUBLE(10.0, s.maximum);
    TEST_ASSERT_EQUAL_DOUBLE(23.0, s.sum);
}

void test_min_after_full_replacement(void) {
    SampleBuffer buf(10);
    for (int i = 0; i < 10; i++) {
        buf.addItem(1);
    }
    buf.addItem(10);
    buf.addItem(5);
    buf.addItem(30);
    buf.addItem(8);
    buf.addItem(5);
    buf.addItem(9);
    buf.addItem(4.1);
    buf.addItem(5);
    buf.addItem(155);
    buf.addItem(88);
    buf.addItem(17);
    buf.addItem(9);

    BufferStats s = buf.getAverageMinMaxSum();
    TEST_ASSERT_EQUAL_DOUBLE(4.1, s.minimum);
}

void test_sum_min_max_last(void) {
    SampleBuffer buf(10);
    for (int i = 0; i < 10; i++) {
        buf.addItem(1);
    }
    buf.addItem(10);
    buf.addItem(5);
    buf.addItem(30);
    buf.addItem(8);
    buf.addItem(5);
    buf.addItem(9);
    buf.addItem(4.1);
    buf.addItem(5);
    buf.addItem(155);
    buf.addItem(88);
    buf.addItem(17);
    buf.addItem(9);

    BufferStats s = buf.sumMinMaxLast(2);
    TEST_ASSERT_EQUAL_DOUBLE(9.0, s.minimum);
}

void test_first_item_prefills_whole_buffer(void) {
    SampleBuffer buf(4);
    buf.addItem(7);
    // After a single insert, every slot should hold 7 (Go "first" behaviour).
    BufferStats s = buf.getAverageMinMaxSum();
    TEST_ASSERT_EQUAL_DOUBLE(7.0, s.average);
    TEST_ASSERT_EQUAL_DOUBLE(28.0, s.sum);
    TEST_ASSERT_EQUAL_DOUBLE(7.0, s.minimum);
    TEST_ASSERT_EQUAL_DOUBLE(7.0, s.maximum);
}

void test_get_last(void) {
    SampleBuffer buf(5);
    buf.addItem(3); // prefills
    buf.addItem(9);
    TEST_ASSERT_EQUAL_DOUBLE(9.0, buf.getLast());
}

int main(int, char **) {
    UNITY_BEGIN();
    RUN_TEST(test_add_item_fills_and_averages);
    RUN_TEST(test_min_after_full_replacement);
    RUN_TEST(test_sum_min_max_last);
    RUN_TEST(test_first_item_prefills_whole_buffer);
    RUN_TEST(test_get_last);
    return UNITY_END();
}
