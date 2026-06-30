#pragma once

#include "buffer.h"
#include "constants.h"

// Pure wind calculations — ported from weather/sensors/Anemometer.go.
// Arduino-independent so they can be unit tested on the host. The CAN/TWAI
// ingestion path lives in anemometer.cpp; this header only does maths.

// Wrap an index into [0, size) the same way as getWrappedIndex() in the Go
// reference (assumes x < 2*size).
inline int windWrappedIndex(int x, int size) {
    if (x >= size) {
        return x - size;
    }
    return x;
}

// GetSpeed(): mean pulses/sample over the whole buffer, scaled to mph.
//   ticksPerSec = avg * WindSamplesPerSecond
//   speed       = MphPerTick * ticksPerSec
// Negative averages are treated as 0; speeds > WindSpeedMaxMph are errors -> 0.
inline double windSpeedMph(SampleBuffer &speedBuf) {
    BufferStats s = speedBuf.getAverageMinMaxSum();
    double avg = s.average;
    double ticksPerSec = avg * WindSamplesPerSecond;
    if (ticksPerSec < 0) {
        ticksPerSec = 0;
    }
    double speed = MphPerTick * ticksPerSec;
    if (speed > WindSpeedMaxMph) {
        speed = 0;
    }
    return speed;
}

// GetGust(): "the maximum three second average wind speed occurring in any
// period". Slides a 3-second window (WindSamplesPerSecond * 3 samples) across
// the whole buffer and takes the maximum sum.
//   gust = (threeSecMax / 3) * MphPerTick
// Values > WindGustMaxMph are rejected and the previous valid gust is reused.
inline double windGustMph(SampleBuffer &gustBuf, double &lastGust) {
    constexpr int threeSecond = 3;
    int size = 0;
    int position = 0;
    std::vector<double> data = gustBuf.getRawData(size, position);

    double threeSecMax = 0.0;
    const int window = WindSamplesPerSecond * threeSecond;
    for (int i = 0; i < size; i++) {
        double x = 0;
        for (int j = 0; j < window; j++) {
            x += data[windWrappedIndex(i + j, size)];
        }
        if (x > threeSecMax) {
            threeSecMax = x;
        }
    }
    double val = (threeSecMax / threeSecond) * MphPerTick;
    if (val > WindGustMaxMph) {
        val = lastGust;
    }
    lastGust = val;
    return val;
}

// GetDirection(): average of the direction buffer (degrees).
inline double windDirection(SampleBuffer &dirBuf) {
    BufferStats s = dirBuf.getAverageMinMaxSum();
    return s.average;
}

// voltToDegrees(): wind-vane voltage -> compass bearing. Thresholds are the
// exact midpoint values measured in the Go reference. outStr (optional)
// receives the cardinal-point label.
inline double windVoltToDegrees(double v, const char **outStr = nullptr) {
    double deg;
    const char *str;
    if (v < 0.376) {
        deg = 112.5; str = "ESE";
    } else if (v < 0.441) {
        deg = 67.5; str = "ENE";
    } else if (v < 0.548) {
        deg = 90.0; str = "E";
    } else if (v < 0.775) {
        deg = 157.5; str = "SSE";
    } else if (v < 1.069) {
        deg = 135.0; str = "SE";
    } else if (v < 1.324) {
        deg = 202.5; str = "SSW";
    } else if (v < 1.726) {
        deg = 180.0; str = "S";
    } else if (v < 2.161) {
        deg = 22.5; str = "NNE";
    } else if (v < 2.64) {
        deg = 45.0; str = "NE";
    } else if (v < 3.055) {
        deg = 247.5; str = "WSW";
    } else if (v < 3.315) {
        deg = 225.0; str = "SW";
    } else if (v < 3.705) {
        deg = 337.5; str = "NNW";
    } else if (v < 4.013) {
        deg = 0.0; str = "N";
    } else if (v < 4.258) {
        deg = 292.5; str = "WNW";
    } else if (v < 4.550) {
        deg = 315.0; str = "NW";
    } else {
        deg = 270.0; str = "W";
    }
    if (outStr != nullptr) {
        *outStr = str;
    }
    return deg;
}
