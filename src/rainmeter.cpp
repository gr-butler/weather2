#include "rainmeter.h"

#include "rain_math.h"
#include "weblog.h"
#define Serial Log // capture Serial output for the /logs web view

// Ported from weather/sensors/Rainmeter.go.

Rainmeter *Rainmeter::instance_ = nullptr;

bool Rainmeter::begin(uint8_t pin, Led *led, bool readPin) {
    pin_ = pin;
    led_ = led;
    instance_ = this;
    pinEnabled_ = readPin;

    // When readPin is false the whole meter still initialises and all the
    // per-minute bookkeeping / buffer logic keeps running — we just never wire
    // up the interrupt, so the pin is never read and the tip count stays 0.
    if (pinEnabled_) {
        pinMode(pin_, INPUT_PULLUP);
        attachInterrupt(digitalPinToInterrupt(pin_), isrTrampoline, FALLING);
    }

    lastMinuteMs_ = millis();
    online_ = true;
    Serial.println(pinEnabled_ ? "Rain sensor online"
                              : "Rain sensor online (pin disabled — no input)");
    return online_;
}

void IRAM_ATTR Rainmeter::isrTrampoline() {
    if (instance_ != nullptr) {
        instance_->handleTip();
    }
}

void IRAM_ATTR Rainmeter::handleTip() {
    // The ISR does NOT count the tip. It only records a candidate falling edge;
    // update() confirms the input is still LOW after RainDebounceGlitchMs before
    // counting, which rejects noise spikes (e.g. an input held at the supply
    // rail). This mirrors the Go gpioutil.Debounce(10ms glitch, 500ms repeat).
    unsigned long now = millis();
    portENTER_CRITICAL_ISR(&mux_);
    // Ignore edges that arrive within the repeat window of the last candidate.
    if (now - lastEdgeMs_ >= RainDebounceRepeatMs) {
        lastEdgeMs_ = now;
        pendingTipMs_ = now;
        tipConfirmPending_ = true;
    }
    portEXIT_CRITICAL_ISR(&mux_);
}

void Rainmeter::update() {
    unsigned long now = millis();

    // Confirm a candidate tip once the glitch window has elapsed. A genuine
    // bucket tip holds the reed switch closed (input LOW) well beyond the glitch
    // window; a noise spike will already have returned HIGH and is discarded.
    bool confirm = false;
    portENTER_CRITICAL(&mux_);
    if (tipConfirmPending_ && (now - pendingTipMs_ >= RainDebounceGlitchMs)) {
        tipConfirmPending_ = false;
        confirm = true;
    }
    portEXIT_CRITICAL(&mux_);

    if (confirm) {
        if (digitalRead(pin_) == LOW) {
            uint32_t dayTips;
            portENTER_CRITICAL(&mux_);
            rainTipMinute_ += 1;     // for rate
            dayAccumulation_ += 1;   // for day total
            accumulationSince_ += 1; // for since-last-report accumulation
            dayTips = dayAccumulation_;
            portEXIT_CRITICAL(&mux_);
            Serial.printf("Rain tip detected: +%.4f mm (day total %.2f mm)\n", MmPerTip,
                          MmPerTip * static_cast<float>(dayTips));
            if (led_ != nullptr) {
                led_->flash();
            }
        } else {
            Serial.println("Rain tip rejected: input HIGH after debounce (glitch/noise)");
        }
    }

    // Once per minute, push the minute's tip count into the rolling buffer and
    // reset the per-minute counter (matches the Go time.Tick(time.Minute) loop).
    if (now - lastMinuteMs_ >= 60000UL) {
        lastMinuteMs_ += 60000UL;
        uint32_t tips;
        portENTER_CRITICAL(&mux_);
        tips = rainTipMinute_;
        rainTipMinute_ = 0;
        portEXIT_CRITICAL(&mux_);
        tipBuf_.addItem(static_cast<double>(tips));
    }
}

float Rainmeter::getRate() {
    return static_cast<float>(rainRateMmPerHour(tipBuf_));
}

float Rainmeter::getDayAccumulation() {
    uint32_t day;
    portENTER_CRITICAL(&mux_);
    day = dayAccumulation_;
    portEXIT_CRITICAL(&mux_);
    return static_cast<float>(rainMmFromTips(day));
}

void Rainmeter::resetDayAccumulation() {
    portENTER_CRITICAL(&mux_);
    dayAccumulation_ = 0;
    portEXIT_CRITICAL(&mux_);
}

float Rainmeter::getAccumulation() {
    uint32_t a;
    portENTER_CRITICAL(&mux_);
    a = accumulationSince_;
    accumulationSince_ = 0;
    portEXIT_CRITICAL(&mux_);
    return static_cast<float>(rainMmFromTips(a));
}
