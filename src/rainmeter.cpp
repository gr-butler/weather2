#include "rainmeter.h"

#include "weblog.h"
#define Serial Log // capture Serial output for the /logs web view

// Ported from weather/sensors/Rainmeter.go.

Rainmeter *Rainmeter::instance_ = nullptr;

bool Rainmeter::begin(uint8_t pin, Led *led) {
    pin_ = pin;
    led_ = led;
    instance_ = this;

    pinMode(pin_, INPUT_PULLUP);
    attachInterrupt(digitalPinToInterrupt(pin_), isrTrampoline, FALLING);

    lastMinuteMs_ = millis();
    online_ = true;
    Serial.println("Rain sensor online");
    return online_;
}

void IRAM_ATTR Rainmeter::isrTrampoline() {
    if (instance_ != nullptr) {
        instance_->handleTip();
    }
}

void IRAM_ATTR Rainmeter::handleTip() {
    // Software debounce: ignore repeated edges within RainDebounceRepeatMs,
    // mirroring the Go gpioutil.Debounce(10ms glitch, 500ms repeat) settings.
    unsigned long now = millis();
    portENTER_CRITICAL_ISR(&mux_);
    if (now - lastTipMs_ >= RainDebounceRepeatMs) {
        lastTipMs_ = now;
        rainTipMinute_ += 1;     // for rate
        dayAccumulation_ += 1;   // for day total
        accumulationSince_ += 1; // for since-last-report accumulation
        tipFlashPending_ = true;
    }
    portEXIT_CRITICAL_ISR(&mux_);
}

void Rainmeter::update() {
    // Flash the tip LED outside the ISR.
    bool flash = false;
    portENTER_CRITICAL(&mux_);
    if (tipFlashPending_) {
        tipFlashPending_ = false;
        flash = true;
    }
    portEXIT_CRITICAL(&mux_);
    if (flash && led_ != nullptr) {
        led_->flash();
    }

    // Once per minute, push the minute's tip count into the rolling buffer and
    // reset the per-minute counter (matches the Go time.Tick(time.Minute) loop).
    unsigned long now = millis();
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
    BufferStats s = tipBuf_.getAverageMinMaxSum();
    return MmPerTip * static_cast<float>(s.sum);
}

float Rainmeter::getDayAccumulation() {
    uint32_t day;
    portENTER_CRITICAL(&mux_);
    day = dayAccumulation_;
    portEXIT_CRITICAL(&mux_);
    return MmPerTip * static_cast<float>(day);
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
    return MmPerTip * static_cast<float>(a);
}
