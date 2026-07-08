#pragma once

#include <Arduino.h>

#include "buffer.h"
#include "constants.h"
#include "led.h"

// Rain meter — ported from weather/sensors/Rainmeter.go.
//
// Hardware change from reference: a tipping-bucket reed switch on RAIN_PIN
// triggers a FALLING-edge interrupt (the Pi version used periph.io debounced
// edge waiting). Software debounce mirrors the Go gpioutil.Debounce settings:
// a candidate edge is only counted if the input is still LOW after
// RainDebounceGlitchMs (10 ms) — rejecting noise spikes such as an input held
// at the supply rail — and repeated edges within RainDebounceRepeatMs (500 ms)
// are ignored. Each accepted tip is logged and flashes the tip LED.
//
// Counting model (identical to Go):
//   * each valid tip increments three counters: per-minute, day, since-last-report
//   * once per minute the per-minute count is pushed into a 60-slot buffer
//   * GetRate()  = MmPerTip * sum(buffer)            (mm over the last hour)
//   * GetDayAccumulation() = MmPerTip * dayAccumulation
//   * GetAccumulation()    = MmPerTip * accumulationSince, then resets it
class Rainmeter {
public:
    Rainmeter() : tipBuf_(RainBufferMinutes) {}

    // Attach the interrupt and start the per-minute bookkeeping. led may be
    // nullptr if no tip LED is fitted. When readPin is false the meter is fully
    // initialised and all buffer/rate logic runs, but the interrupt is NOT
    // attached so the pin is never read (guaranteed zero input) — used by
    // DISABLE_RAIN_PIN builds.
    bool begin(uint8_t pin, Led *led, bool readPin = true);

    // Call frequently from loop(): advances the per-minute buffer and flashes
    // the tip LED when a tip has been registered.
    void update();

    // mm of rain over the last hour (MmPerTip * sum of last 60 one-minute counts).
    float getRate();

    // mm accumulated since 09:01 (day total).
    float getDayAccumulation();
    void resetDayAccumulation();

    // mm accumulated since this was last called; resets the running counter.
    float getAccumulation();

    bool isOnline() const { return online_; }

private:
    static void IRAM_ATTR isrTrampoline();
    void IRAM_ATTR handleTip();

    SampleBuffer tipBuf_;       // one slot per minute, last hour
    Led *led_ = nullptr;
    uint8_t pin_ = 0;
    bool online_ = false;
    bool pinEnabled_ = true;    // false = interrupt not attached, pin never read
    unsigned long lastMinuteMs_ = 0;

    // Counters shared with the ISR. Guarded by mux_.
    volatile uint32_t rainTipMinute_ = 0;     // tips in the current minute
    volatile uint32_t dayAccumulation_ = 0;   // tips since last 9am reset
    volatile uint32_t accumulationSince_ = 0; // tips since last GetAccumulation
    volatile unsigned long lastEdgeMs_ = 0;   // last accepted candidate edge (repeat debounce)
    volatile unsigned long pendingTipMs_ = 0; // timestamp of candidate awaiting glitch confirm
    volatile bool tipConfirmPending_ = false; // a candidate edge awaits glitch confirmation

    portMUX_TYPE mux_ = portMUX_INITIALIZER_UNLOCKED;

    static Rainmeter *instance_; // for the static ISR trampoline
};
