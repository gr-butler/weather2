#include <unity.h>

#include "wind_math.h"

// Ported from weather/sensors/Anemometer_test.go and the gust/direction logic,
// verifying the C++ wind maths match the Go reference exactly.

void test_speed_zero_then_one_pulse(void) {
    SampleBuffer speedBuf(WindBufferSamples);

    // Empty buffer -> 0 mph.
    TEST_ASSERT_EQUAL_DOUBLE(0.0, windSpeedMph(speedBuf));

    // First insert fills the whole buffer with that value (Go "first" rule).
    speedBuf.addItem(1.0);

    // avg = 1, ticksPerSec = 1 * 4 = 4, speed = 4 * MphPerTick.
    double expected = 4.0 * MphPerTick;
    TEST_ASSERT_EQUAL_DOUBLE(expected, windSpeedMph(speedBuf));
}

void test_speed_clamps_above_max(void) {
    SampleBuffer speedBuf(WindBufferSamples);
    // avg pulses such that speed > 100 mph -> clamped to 0.
    // speed = MphPerTick * avg * 4. For avg = 50: 1.429*50*4 = 285.8 -> 0.
    speedBuf.addItem(50.0);
    TEST_ASSERT_EQUAL_DOUBLE(0.0, windSpeedMph(speedBuf));
}

void test_gust_three_second_window(void) {
    SampleBuffer gustBuf(WindBufferSamples);
    // Fill with 1 pulse/sample. Any 12-sample (3 s) window sums to 12.
    gustBuf.addItem(1.0); // prefills all 240 slots with 1
    double lastGust = 0.0;
    // gust = (12 / 3) * MphPerTick = 4 * MphPerTick
    double expected = 4.0 * MphPerTick;
    TEST_ASSERT_EQUAL_DOUBLE(expected, windGustMph(gustBuf, lastGust));
}

void test_gust_clamps_and_reuses_last(void) {
    SampleBuffer gustBuf(WindBufferSamples);
    // Large values so (sum/3)*MphPerTick > 120 -> reuse lastGust.
    gustBuf.addItem(100.0); // window sum = 1200; (1200/3)*1.429 = 571.6 > 120
    double lastGust = 7.5;  // previous valid value
    double val = windGustMph(gustBuf, lastGust);
    TEST_ASSERT_EQUAL_DOUBLE(7.5, val);
    TEST_ASSERT_EQUAL_DOUBLE(7.5, lastGust);
}

void test_volt_to_degrees_cardinals(void) {
    const char *str = nullptr;
    TEST_ASSERT_EQUAL_DOUBLE(112.5, windVoltToDegrees(0.0, &str));   // < 0.376 ESE
    TEST_ASSERT_EQUAL_STRING("ESE", str);
    TEST_ASSERT_EQUAL_DOUBLE(90.0, windVoltToDegrees(0.5, &str));    // E
    TEST_ASSERT_EQUAL_STRING("E", str);
    TEST_ASSERT_EQUAL_DOUBLE(0.0, windVoltToDegrees(3.9, &str));     // N
    TEST_ASSERT_EQUAL_STRING("N", str);
    TEST_ASSERT_EQUAL_DOUBLE(270.0, windVoltToDegrees(5.0, &str));   // default W
    TEST_ASSERT_EQUAL_STRING("W", str);
}

void test_direction_average(void) {
    SampleBuffer dirBuf(WindBufferSamples);
    dirBuf.addItem(180.0); // prefills all slots with 180
    TEST_ASSERT_EQUAL_DOUBLE(
        180.0,
        windDirectionRolling(dirBuf,
                             WindSamplesPerSecond * WindDirectionAverageSeconds));
}

void test_direction_rolling_wraparound(void) {
    SampleBuffer dirBuf(WindBufferSamples);
    dirBuf.addItem(0.0); // prefill with north

    // Fill the 5 s rolling window with values around north that would break a
    // naive linear average but should circular-average near 0 degrees.
    for (int i = 0; i < 10; i++) {
        dirBuf.addItem(359.0);
        dirBuf.addItem(1.0);
    }

    double deg = windDirectionRolling(
        dirBuf, WindSamplesPerSecond * WindDirectionAverageSeconds);
    TEST_ASSERT_TRUE((deg <= 2.0) || (deg >= 358.0));
}

void test_direction_filtered_rejects_spin_scatter(void) {
    SampleBuffer dirBuf(WindBufferSamples);
    dirBuf.addItem(90.0); // prefill: light wind from the east (E)

    // Simulate a buffeted vane: mostly dwelling at E (90) with occasional
    // spins scattering samples all around the compass. Fill the full averaging
    // window so the modal filter has plenty to work with.
    const int window = WindSamplesPerSecond * WindDirectionAverageSeconds; // 120
    const double spins[] = {0.0, 45.0, 180.0, 225.0, 315.0, 270.0};
    int spinIdx = 0;
    for (int i = 0; i < window; i++) {
        if (i % 5 == 0) { // ~20% spurious scatter
            dirBuf.addItem(spins[spinIdx % 6]);
            spinIdx++;
        } else {
            dirBuf.addItem(90.0); // true bearing: E
        }
    }

    // A plain circular mean is dragged away by the scatter; the filtered
    // result should stay locked on to E (90 degrees).
    double filtered = windDirectionFiltered(dirBuf, window);
    TEST_ASSERT_TRUE((filtered >= 85.0) && (filtered <= 95.0));
}

void test_direction_filtered_follows_adjacent_wander(void) {
    SampleBuffer dirBuf(WindBufferSamples);
    dirBuf.addItem(90.0);

    // Wind genuinely wandering between E (90) and ENE (67.5) should average to
    // a bearing between them, not be discarded as scatter.
    const int window = WindSamplesPerSecond * WindDirectionAverageSeconds;
    for (int i = 0; i < window; i++) {
        dirBuf.addItem((i % 2 == 0) ? 90.0 : 67.5);
    }

    double filtered = windDirectionFiltered(dirBuf, window);
    TEST_ASSERT_TRUE((filtered >= 67.5) && (filtered <= 90.0));
}

int main(int, char **) {
    UNITY_BEGIN();
    RUN_TEST(test_speed_zero_then_one_pulse);
    RUN_TEST(test_speed_clamps_above_max);
    RUN_TEST(test_gust_three_second_window);
    RUN_TEST(test_gust_clamps_and_reuses_last);
    RUN_TEST(test_volt_to_degrees_cardinals);
    RUN_TEST(test_direction_average);
    RUN_TEST(test_direction_rolling_wraparound);
    RUN_TEST(test_direction_filtered_rejects_spin_scatter);
    RUN_TEST(test_direction_filtered_follows_adjacent_wander);
    return UNITY_END();
}
