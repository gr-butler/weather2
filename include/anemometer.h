#pragma once

#include <Arduino.h>

#include "buffer.h"
#include "constants.h"
#include "wind_math.h"

// Anemometer — wind speed, gust and direction.
//
// Data-ingestion model: OPTION B (see AGENTS.md Rule 3).
// The remote masthead node pushes CAN frames (pulse_count + direction ADC).
// We accumulate pulse counts from any frames received and, on a fixed 250 ms
// local timer (WindSamplesPerSecond = 4 Hz), insert exactly one sample per slot
// into the rolling buffers — inserting 0 if no frame arrived in that window.
// This guarantees the buffers are filled at exactly 4 Hz regardless of the
// remote node's transmit timing, which the speed/gust maths depend on.
//
// The buffer logic and all calculations are unchanged from the Go reference
// (weather/sensors/Anemometer.go); only the data source differs (CAN vs I2C).
class Anemometer {
public:
    Anemometer()
        : speedBuf_(WindBufferSamples),
          gustBuf_(WindBufferSamples),
          dirBuf_(WindBufferSamples) {}

    // Install and start the TWAI (CAN) driver. Returns false on failure.
    bool begin();

    // Call frequently from loop(): drains received CAN frames and advances the
    // 250 ms sampling timer.
    void update();

    double getSpeed() { return windSpeedMph(speedBuf_); }
    double getGust() { return windGustMph(gustBuf_, lastGust_); }
    double getDirection() { return windDirection(dirBuf_); }
    const char *getDirectionString() const { return dirStr_; }
    bool isOnline() const { return online_; }

private:
    void drainCanFrames();
    void sampleSlot();

    SampleBuffer speedBuf_;
    SampleBuffer gustBuf_;
    SampleBuffer dirBuf_;

    double lastGust_ = 0.0;
    const char *dirStr_ = "N";
    double lastDirection_ = 0.0;

    // Accumulated within the current 250 ms window.
    uint32_t accumPulses_ = 0;
    bool frameThisWindow_ = false;
    bool haveDirection_ = false;
    double pendingDirection_ = 0.0;

    unsigned long lastSampleMs_ = 0;
    int missedSlots_ = 0;
    bool online_ = false;
    bool driverInstalled_ = false;
};
