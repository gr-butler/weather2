#pragma once

#include <Arduino.h>

#include "constants.h"

// Minimal LED helper — mirrors weather/led/led.go.
//
// Unlike the Go version (which sleeps inside a goroutine), flash() here is
// non-blocking: it drives the pin and records a timestamp; service() turns the
// LED off again after LEDFlashDurationMs. service() must be called regularly
// from loop(). This keeps flashing safe to trigger from time-critical code.
class Led {
public:
    Led() = default;

    void begin(const char *name, uint8_t pin) {
        name_ = name;
        pin_ = pin;
        pinMode(pin_, OUTPUT);
        digitalWrite(pin_, LOW);
        // Startup flicker so we can see the pin works.
        for (int i = 0; i < 3; i++) {
            digitalWrite(pin_, HIGH);
            delay(100);
            digitalWrite(pin_, LOW);
            delay(100);
        }
    }

    void on() {
        on_ = true;
        digitalWrite(pin_, HIGH);
    }

    void off() {
        on_ = false;
        flashing_ = false;
        digitalWrite(pin_, LOW);
    }

    bool isOn() const { return on_; }

    // Non-blocking flash: drive opposite of resting state for LEDFlashDurationMs.
    void flash() {
        if (flashing_) {
            return; // discard overlapping request (matches Go TryLock behaviour)
        }
        flashing_ = true;
        flashStartMs_ = millis();
        digitalWrite(pin_, on_ ? LOW : HIGH);
    }

    // Must be called frequently from loop() to end an in-progress flash.
    void service() {
        if (flashing_ && (millis() - flashStartMs_ >= LEDFlashDurationMs)) {
            flashing_ = false;
            digitalWrite(pin_, on_ ? HIGH : LOW);
        }
    }

private:
    const char *name_ = "";
    uint8_t pin_ = 0;
    bool on_ = false;
    bool flashing_ = false;
    unsigned long flashStartMs_ = 0;
};
