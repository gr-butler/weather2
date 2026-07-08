#include <unity.h>

#include "buffer.h"
#include "constants.h"
#include "rain_math.h"

// Tests for the pure rain calculations in include/rain_math.h.
// These mirror the maths used by Rainmeter (rate / day total / since-last
// accumulation) so the firmware and host tests share one source of truth.

// MmPerTip is a float constant; promote it exactly the same way rain_math.h
// does so expected values match bit-for-bit.
static double mmPerTip() { return static_cast<double>(MmPerTip); }

// ---------------------------------------------------------------------------
//  Rate — MmPerTip * sum(60-minute buffer)
// ---------------------------------------------------------------------------

// A fresh buffer (nothing pushed) reports zero rain.
void test_rate_empty_buffer_is_zero(void) {
    SampleBuffer tipBuf(RainBufferMinutes);
    TEST_ASSERT_EQUAL_DOUBLE(0.0, rainRateMmPerHour(tipBuf));
}

// Calm start-up: the first minute has 0 tips, which pre-fills zeros. Rate = 0.
void test_rate_calm_start_is_zero(void) {
    SampleBuffer tipBuf(RainBufferMinutes);
    tipBuf.addItem(0); // first push pre-fills all 60 slots with 0
    TEST_ASSERT_EQUAL_DOUBLE(0.0, rainRateMmPerHour(tipBuf));
    // Several more calm minutes keep it at zero.
    for (int i = 0; i < 10; i++) {
        tipBuf.addItem(0);
    }
    TEST_ASSERT_EQUAL_DOUBLE(0.0, rainRateMmPerHour(tipBuf));
}

// Steady drizzle: after the calm prefill, one tip per minute for the whole
// hour gives sum == 60 tips -> rate = MmPerTip * 60.
void test_rate_one_tip_per_minute_full_hour(void) {
    SampleBuffer tipBuf(RainBufferMinutes);
    tipBuf.addItem(0); // calm prefill
    for (int i = 0; i < RainBufferMinutes; i++) {
        tipBuf.addItem(1); // overwrite every slot with 1 tip
    }
    double expected = mmPerTip() * 60.0;
    TEST_ASSERT_EQUAL_DOUBLE(expected, rainRateMmPerHour(tipBuf));
}

// The reported real-world figure: 7 tips spread across the last hour
// -> 7 * 0.3537 = 2.4759 mm/hr. Confirms the 2.48 mm reading corresponds to
// exactly 7 counted tips (i.e. the rate maths itself is not inventing rain).
void test_rate_seven_tips_matches_reported_2_48mm(void) {
    SampleBuffer tipBuf(RainBufferMinutes);
    tipBuf.addItem(0); // calm prefill (rest of the hour stays 0)
    // Seven separate minutes each register a single tip.
    for (int i = 0; i < 7; i++) {
        tipBuf.addItem(1);
        tipBuf.addItem(0); // an idle minute in between
    }
    double rate = rainRateMmPerHour(tipBuf);
    TEST_ASSERT_EQUAL_DOUBLE(mmPerTip() * 7.0, rate);
    // ~2.48 mm/hr to 2 dp.
    TEST_ASSERT_DOUBLE_WITHIN(0.005, 2.48, rate);
}

// Start-up prefill gotcha: a SINGLE tip counted in the very first minute is
// replicated across all 60 slots, briefly inflating the rate 60x. This locks
// in (and documents) the inherited Go behaviour so any future change is caught.
void test_rate_first_minute_tip_inflates_60x(void) {
    SampleBuffer tipBuf(RainBufferMinutes);
    tipBuf.addItem(1); // first push -> every slot becomes 1 -> sum 60
    double rate = rainRateMmPerHour(tipBuf);
    TEST_ASSERT_EQUAL_DOUBLE(mmPerTip() * 60.0, rate);
    // The inflation flushes out as calm minutes overwrite the prefilled slots.
    for (int i = 0; i < RainBufferMinutes; i++) {
        tipBuf.addItem(0);
    }
    TEST_ASSERT_EQUAL_DOUBLE(0.0, rainRateMmPerHour(tipBuf));
}

// Mixed minute counts sum correctly (bursts of more than one tip per minute).
void test_rate_mixed_counts_sum(void) {
    SampleBuffer tipBuf(RainBufferMinutes);
    tipBuf.addItem(0); // calm prefill
    tipBuf.addItem(3);
    tipBuf.addItem(0);
    tipBuf.addItem(5);
    tipBuf.addItem(2);
    // sum of tips = 10
    TEST_ASSERT_EQUAL_DOUBLE(mmPerTip() * 10.0, rainRateMmPerHour(tipBuf));
}

// ---------------------------------------------------------------------------
//  Accumulation — MmPerTip * tips (day total and since-last-report)
// ---------------------------------------------------------------------------

void test_mm_from_tips_zero(void) {
    TEST_ASSERT_EQUAL_DOUBLE(0.0, rainMmFromTips(0));
}

void test_mm_from_tips_one(void) {
    TEST_ASSERT_EQUAL_DOUBLE(mmPerTip() * 1.0, rainMmFromTips(1));
}

void test_mm_from_tips_scales_linearly(void) {
    TEST_ASSERT_EQUAL_DOUBLE(mmPerTip() * 10.0, rainMmFromTips(10));
    TEST_ASSERT_EQUAL_DOUBLE(mmPerTip() * 283.0, rainMmFromTips(283));
    // A full day of moderate rain ~ 100 tips = 35.37 mm.
    TEST_ASSERT_DOUBLE_WITHIN(0.001, 35.37, rainMmFromTips(100));
}

int main(int, char **) {
    UNITY_BEGIN();
    RUN_TEST(test_rate_empty_buffer_is_zero);
    RUN_TEST(test_rate_calm_start_is_zero);
    RUN_TEST(test_rate_one_tip_per_minute_full_hour);
    RUN_TEST(test_rate_seven_tips_matches_reported_2_48mm);
    RUN_TEST(test_rate_first_minute_tip_inflates_60x);
    RUN_TEST(test_rate_mixed_counts_sum);
    RUN_TEST(test_mm_from_tips_zero);
    RUN_TEST(test_mm_from_tips_one);
    RUN_TEST(test_mm_from_tips_scales_linearly);
    return UNITY_END();
}
