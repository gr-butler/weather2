#pragma once

#include <Arduino.h>

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include "driver/twai.h"

#include "buffer.h"
#include "constants.h"
#include "wind_math.h"

// Anemometer — wind speed, gust and direction.
//
// Data-ingestion model: OPTION B (see AGENTS.md Rule 3).
// The remote masthead node pushes CAN frames (pulse_count + direction ADC).
// We accumulate pulse counts from any frames received and, on a fixed 250 ms
// timer (WindSamplesPerSecond = 4 Hz), insert exactly one sample per slot into
// the rolling buffers — inserting 0 if no frame arrived in that window. This
// guarantees the buffers are filled at exactly 4 Hz regardless of the remote
// node's transmit timing, which the speed/gust maths depend on.
//
// Threading: draining the TWAI RX FIFO and the 250 ms sampling both run in a
// dedicated FreeRTOS task pinned to the core the Arduino loop does NOT use
// (core 0). This keeps the FIFO drained and the 4 Hz cadence exact even when
// loop() (core 1) is blocked in a slow network call (river HTTPS, MQTT/WOW over
// TLS) — previously such stalls overflowed the FIFO and dropped frames. The
// rolling buffers are shared with the reader getters (called from loop() and
// the async web/reporting tasks), so buffer access is guarded by a mutex.
//
// The buffer logic and all calculations are unchanged from the Go reference
// (weather/sensors/Anemometer.go); only the data source differs (CAN vs I2C).
class Anemometer {
public:
    Anemometer()
        : speedBuf_(WindBufferSamples),
          gustBuf_(WindBufferSamples),
          dirBuf_(WindBufferSamples) {}

    // Install and start the TWAI (CAN) driver and spawn the RX task on core 0.
    // Returns false on failure.
    bool begin();

    double getSpeed() {
        MutexGuard g(mutex_);
        return windSpeedMph(speedBuf_);
    }
    double getGust() {
        MutexGuard g(mutex_);
        return windGustMph(gustBuf_, lastGust_);
    }
    double getDirection() {
        MutexGuard g(mutex_);
        return windDirectionRolling(
            dirBuf_, WindSamplesPerSecond * WindDirectionAverageSeconds);
    }
    void getAppWindSummary(double &dirDeg, double &speedMph, double &gustMph) {
        MutexGuard g(mutex_);
        const int window = WindSamplesPerSecond * AppWindSummarySeconds;
        dirDeg = windDirectionRolling(dirBuf_, window);
        speedMph = windSpeedMphLastWindow(speedBuf_, window);
        gustMph = windGustMphLastWindow(gustBuf_, window, lastAppGust_);
    }
    const char *getDirectionString() const { return dirStr_; }
    bool isOnline() const { return online_; }

private:
    static void canRxTask(void *arg); // trampoline -> taskLoop()
    void taskLoop();
    void processFrame(const twai_message_t &msg);
    void sampleSlot();
    void logCanStatus(unsigned long now);

    // RAII lock for the rolling-buffer mutex. A null handle (before begin())
    // is a no-op so the getters stay safe to call during startup.
    struct MutexGuard {
        SemaphoreHandle_t m;
        explicit MutexGuard(SemaphoreHandle_t mtx) : m(mtx) {
            if (m != nullptr) {
                xSemaphoreTake(m, portMAX_DELAY);
            }
        }
        ~MutexGuard() {
            if (m != nullptr) {
                xSemaphoreGive(m);
            }
        }
    };

    SampleBuffer speedBuf_;
    SampleBuffer gustBuf_;
    SampleBuffer dirBuf_;

    double lastGust_ = 0.0;
    double lastAppGust_ = 0.0;
    const char *dirStr_ = "N";
    double lastDirection_ = 0.0;

    // Accumulated within the current 250 ms window.
    uint32_t accumPulses_ = 0;
    bool frameThisWindow_ = false;
    bool haveDirection_ = false;
    double pendingDirection_ = 0.0;

    unsigned long lastSampleMs_ = 0;
    unsigned long lastMissedLogMs_ = 0;
    unsigned long lastStatusLogMs_ = 0;
    unsigned long lastFrameMs_ = 0; // millis() of the last decoded CAN frame
    bool haveFrameEver_ = false;    // true once any frame has been received
    bool online_ = false;
    bool driverInstalled_ = false;

    // Sequence tracking for lost-frame / masthead-reboot detection.
    uint32_t lastSeq_ = 0;
    bool haveSeq_ = false;

    TaskHandle_t task_ = nullptr;
    SemaphoreHandle_t mutex_ = nullptr;
};
