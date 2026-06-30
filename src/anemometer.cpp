#include "anemometer.h"

#include "driver/twai.h"

#include "can_protocol.h"

// CAN/TWAI wind-data ingestion. See anemometer.h for the OPTION B sampling
// model. Calculations live in wind_math.h (ported from Anemometer.go).

namespace {
constexpr unsigned long WindSampleIntervalMs = 1000 / WindSamplesPerSecond; // 250 ms
constexpr int MaxMissedSlots = 5; // 5 * 250 ms = 1.25 s -> mark offline
} // namespace

bool Anemometer::begin() {
    twai_general_config_t g_config = TWAI_GENERAL_CONFIG_DEFAULT(
        static_cast<gpio_num_t>(TX_GPIO_NUM),
        static_cast<gpio_num_t>(RX_GPIO_NUM), TWAI_MODE_NORMAL);

    twai_timing_config_t t_config = TWAI_TIMING_CONFIG_500KBITS();
    twai_filter_config_t f_config = TWAI_FILTER_CONFIG_ACCEPT_ALL();

    if (twai_driver_install(&g_config, &t_config, &f_config) != ESP_OK) {
        Serial.println("Failed to install TWAI driver");
        return false;
    }
    if (twai_start() != ESP_OK) {
        Serial.println("Failed to start TWAI driver");
        twai_driver_uninstall();
        return false;
    }
    driverInstalled_ = true;
    lastSampleMs_ = millis();
    Serial.printf("Wind CAN driver online (TX=%d RX=%d @ %lu bps)\n",
                  (int)TX_GPIO_NUM, (int)RX_GPIO_NUM,
                  (unsigned long)WIND_CAN_BITRATE);
    return true;
}

void Anemometer::drainCanFrames() {
    twai_message_t msg;
    // Non-blocking drain of all queued frames.
    while (twai_receive(&msg, 0) == ESP_OK) {
        if (msg.identifier != WIND_CAN_ID || msg.rtr) {
            continue;
        }
        WindFrame frame;
        if (!decodeWindFrame(msg.data, msg.data_length_code, frame)) {
            continue;
        }
        // Per-frame pulse sanity clamp (mirrors Go pulseCount > 25 -> 0).
        uint8_t pulses = frame.pulseCount;
        if (pulses > WIND_PULSE_MAX) {
            Serial.printf("Pulse count error [%u]\n", (unsigned)pulses);
            pulses = 0;
        }
        accumPulses_ += pulses;
        frameThisWindow_ = true;
        // Keep the most recent direction reading for this window.
        pendingDirection_ = windVoltToDegrees(windAdcToVolts(frame.adcRaw), &dirStr_);
        haveDirection_ = true;
    }
}

void Anemometer::sampleSlot() {
    // Insert exactly one sample for the elapsed 250 ms slot.
    double pulses = static_cast<double>(accumPulses_);

    speedBuf_.addItem(pulses);
    gustBuf_.addItem(pulses);

    // Direction: only trust it when the wind is actually turning the cups,
    // otherwise hold the last valid bearing (matches the Go zero-wind rule).
    if (accumPulses_ > 0 && haveDirection_) {
        lastDirection_ = pendingDirection_;
        dirBuf_.addItem(pendingDirection_);
    } else {
        dirBuf_.addItem(dirBuf_.getLast());
    }

    if (frameThisWindow_) {
        missedSlots_ = 0;
        online_ = true;
    } else {
        missedSlots_++;
        //Serial.println("Wind: no CAN frame in 250 ms slot, inserted 0");
        if (missedSlots_ >= MaxMissedSlots) {
            if (online_) {
                Serial.println("Wind sensor OFFLINE (5 consecutive missed frames)");
            }
            online_ = false;
        }
    }

    // Reset the window accumulators.
    accumPulses_ = 0;
    frameThisWindow_ = false;
    haveDirection_ = false;
}

void Anemometer::update() {
    if (!driverInstalled_) {
        return;
    }
    drainCanFrames();

    // Advance the 250 ms sampling timer, catching up if loop() was delayed so
    // the buffer is always filled at exactly 4 Hz.
    unsigned long now = millis();
    while (now - lastSampleMs_ >= WindSampleIntervalMs) {
        lastSampleMs_ += WindSampleIntervalMs;
        sampleSlot();
    }
}
