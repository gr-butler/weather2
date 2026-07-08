#include <unity.h>

#include "buffer.h"

// Comprehensive tests for the fixed-size circular SampleBuffer.
// Ported from weather/buffer/buffer_test.go and extended to lock down the
// wrap-around, prefill and edge-case behaviour the rain/wind maths rely on.

// ---------------------------------------------------------------------------
//  Original ported cases
// ---------------------------------------------------------------------------

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

// ---------------------------------------------------------------------------
//  Prefill / start-up behaviour
// ---------------------------------------------------------------------------

// A calm start (first value == 0) must pre-fill zeros, not spuriously report.
void test_first_item_zero_prefills_zeros(void) {
    SampleBuffer buf(60);
    buf.addItem(0);
    BufferStats s = buf.getAverageMinMaxSum();
    TEST_ASSERT_EQUAL_DOUBLE(0.0, s.sum);
    TEST_ASSERT_EQUAL_DOUBLE(0.0, s.average);
    TEST_ASSERT_EQUAL_DOUBLE(0.0, s.minimum);
    TEST_ASSERT_EQUAL_DOUBLE(0.0, s.maximum);
}

// The prefill only happens once: the second insert overwrites a single slot.
void test_prefill_happens_only_once(void) {
    SampleBuffer buf(4);
    buf.addItem(2); // prefills all four slots with 2 -> sum 8
    BufferStats s = buf.getAverageMinMaxSum();
    TEST_ASSERT_EQUAL_DOUBLE(8.0, s.sum);

    buf.addItem(6); // overwrites ONE slot: {6,2,2,2} (position wrapped to 1)
    s = buf.getAverageMinMaxSum();
    TEST_ASSERT_EQUAL_DOUBLE(12.0, s.sum);
    TEST_ASSERT_EQUAL_DOUBLE(2.0, s.minimum);
    TEST_ASSERT_EQUAL_DOUBLE(6.0, s.maximum);
}

// ---------------------------------------------------------------------------
//  Empty buffer / all-zero behaviour
// ---------------------------------------------------------------------------

void test_empty_buffer_is_all_zero(void) {
    SampleBuffer buf(8);
    BufferStats s = buf.getAverageMinMaxSum();
    TEST_ASSERT_EQUAL_DOUBLE(0.0, s.sum);
    TEST_ASSERT_EQUAL_DOUBLE(0.0, s.average);
    TEST_ASSERT_EQUAL_DOUBLE(0.0, s.minimum);
    TEST_ASSERT_EQUAL_DOUBLE(0.0, s.maximum);
    TEST_ASSERT_EQUAL_DOUBLE(0.0, buf.getLast());
}

// ---------------------------------------------------------------------------
//  Wrap-around correctness
// ---------------------------------------------------------------------------

// Push more than `size` items and confirm the oldest values are evicted and the
// running sum reflects only the newest `size` values.
void test_wrap_around_evicts_oldest(void) {
    SampleBuffer buf(3);
    buf.addItem(1); // prefill -> {1,1,1}
    buf.addItem(2); // {1,2,1} pos->2
    buf.addItem(3); // {1,2,3} pos->0
    buf.addItem(4); // {4,2,3} pos->1  (oldest '1' evicted)
    BufferStats s = buf.getAverageMinMaxSum();
    TEST_ASSERT_EQUAL_DOUBLE(9.0, s.sum); // 4+2+3
    TEST_ASSERT_EQUAL_DOUBLE(2.0, s.minimum);
    TEST_ASSERT_EQUAL_DOUBLE(4.0, s.maximum);

    buf.addItem(5); // {4,5,3} pos->2
    buf.addItem(6); // {4,5,6} pos->0
    s = buf.getAverageMinMaxSum();
    TEST_ASSERT_EQUAL_DOUBLE(15.0, s.sum); // 4+5+6
    TEST_ASSERT_EQUAL_DOUBLE(4.0, s.minimum);
    TEST_ASSERT_EQUAL_DOUBLE(6.0, s.maximum);
}

// getLast must follow the write position across a wrap.
void test_get_last_after_wrap(void) {
    SampleBuffer buf(3);
    buf.addItem(1); // prefill
    buf.addItem(2);
    buf.addItem(3);
    buf.addItem(99); // wraps to slot 0
    TEST_ASSERT_EQUAL_DOUBLE(99.0, buf.getLast());
}

// ---------------------------------------------------------------------------
//  sumMinMaxLast — including reverse wrap across the buffer start
// ---------------------------------------------------------------------------

void test_sum_min_max_last_reverse_wrap(void) {
    SampleBuffer buf(4);
    buf.addItem(10); // prefill -> {10,10,10,10}
    buf.addItem(20); // {10,20,10,10} pos->2
    buf.addItem(30); // {10,20,30,10} pos->3
    // position is 3; last 2 are slots 1 and 2 -> 20 and 30.
    BufferStats s = buf.sumMinMaxLast(2);
    TEST_ASSERT_EQUAL_DOUBLE(50.0, s.sum);
    TEST_ASSERT_EQUAL_DOUBLE(20.0, s.minimum);
    TEST_ASSERT_EQUAL_DOUBLE(30.0, s.maximum);

    // Ask for the last 3 while position==3: slots 0,1,2 -> 10,20,30.
    s = buf.sumMinMaxLast(3);
    TEST_ASSERT_EQUAL_DOUBLE(60.0, s.sum);
    TEST_ASSERT_EQUAL_DOUBLE(10.0, s.minimum);
    TEST_ASSERT_EQUAL_DOUBLE(30.0, s.maximum);
}

// When numberOfItems spans the whole buffer, sumMinMaxLast == getAverageMinMaxSum sum.
void test_sum_min_max_last_whole_buffer(void) {
    SampleBuffer buf(5);
    buf.addItem(1); // prefill
    buf.addItem(2);
    buf.addItem(3);
    buf.addItem(4);
    buf.addItem(5); // {1,2,3,4,5} then wraps pos->0
    BufferStats whole = buf.getAverageMinMaxSum();
    BufferStats last5 = buf.sumMinMaxLast(5);
    TEST_ASSERT_EQUAL_DOUBLE(whole.sum, last5.sum);
    TEST_ASSERT_EQUAL_DOUBLE(whole.minimum, last5.minimum);
    TEST_ASSERT_EQUAL_DOUBLE(whole.maximum, last5.maximum);
}

// ---------------------------------------------------------------------------
//  averageLast
// ---------------------------------------------------------------------------

void test_average_last_basic_and_wrap(void) {
    SampleBuffer buf(4);
    buf.addItem(2); // prefill {2,2,2,2}
    buf.addItem(4); // {2,4,2,2} pos->2
    buf.addItem(6); // {2,4,6,2} pos->3
    // last 2 -> slots 1,2 -> (4+6)/2 = 5
    TEST_ASSERT_EQUAL_DOUBLE(5.0, buf.averageLast(2));
    // last 4 -> (2+4+6+2)/4 = 3.5
    TEST_ASSERT_EQUAL_DOUBLE(3.5, buf.averageLast(4));
}

// ---------------------------------------------------------------------------
//  addValueToCurrentItem — accumulate into the not-yet-advanced slot
// ---------------------------------------------------------------------------

void test_add_value_to_current_item_sum(void) {
    SampleBuffer buf(3);
    buf.addItem(0); // prefill zeros, position now 1
    buf.addValueToCurrentItem(2.5); // slot 1 -> 2.5
    buf.addValueToCurrentItem(1.5); // slot 1 -> 4.0
    BufferStats s = buf.getAverageMinMaxSum();
    TEST_ASSERT_EQUAL_DOUBLE(4.0, s.sum); // {0,4,0}
    TEST_ASSERT_EQUAL_DOUBLE(4.0, s.maximum);
    TEST_ASSERT_EQUAL_DOUBLE(0.0, s.minimum);
}

// ---------------------------------------------------------------------------
//  Single-slot buffer edge case
// ---------------------------------------------------------------------------

void test_single_slot_buffer(void) {
    SampleBuffer buf(1);
    buf.addItem(5); // prefill (single slot) -> {5}
    BufferStats s = buf.getAverageMinMaxSum();
    TEST_ASSERT_EQUAL_DOUBLE(5.0, s.sum);
    TEST_ASSERT_EQUAL_DOUBLE(5.0, s.average);
    buf.addItem(9); // overwrites the only slot
    s = buf.getAverageMinMaxSum();
    TEST_ASSERT_EQUAL_DOUBLE(9.0, s.sum);
    TEST_ASSERT_EQUAL_DOUBLE(9.0, buf.getLast());
}

// ---------------------------------------------------------------------------
//  getSize passthrough
// ---------------------------------------------------------------------------

void test_get_size(void) {
    SampleBuffer buf(60);
    TEST_ASSERT_EQUAL_INT(60, buf.getSize());
}

int main(int, char **) {
    UNITY_BEGIN();
    RUN_TEST(test_add_item_fills_and_averages);
    RUN_TEST(test_min_after_full_replacement);
    RUN_TEST(test_sum_min_max_last);
    RUN_TEST(test_first_item_prefills_whole_buffer);
    RUN_TEST(test_get_last);
    RUN_TEST(test_first_item_zero_prefills_zeros);
    RUN_TEST(test_prefill_happens_only_once);
    RUN_TEST(test_empty_buffer_is_all_zero);
    RUN_TEST(test_wrap_around_evicts_oldest);
    RUN_TEST(test_get_last_after_wrap);
    RUN_TEST(test_sum_min_max_last_reverse_wrap);
    RUN_TEST(test_sum_min_max_last_whole_buffer);
    RUN_TEST(test_average_last_basic_and_wrap);
    RUN_TEST(test_add_value_to_current_item_sum);
    RUN_TEST(test_single_slot_buffer);
    RUN_TEST(test_get_size);
    return UNITY_END();
}
