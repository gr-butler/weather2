#pragma once

#include <cstdint>

// CAN bus wind-sensor protocol — see AGENTS.md Rule 3.
//
// The remote masthead node pushes wind frames to the ESP32 over CAN bus
// (Waveshare SN65HVD230 transceiver + built-in TWAI controller). This replaces
// the I2C polling of masthead 0x55 used in the Go reference.
//
// If this layout changes, update this header and document it — do NOT silently
// alter the buffer-insertion logic in anemometer.cpp.
//
// The full transmit-side contract for the masthead node (bit rate, frame ID,
// payload layout, 4 Hz timing, ADC scaling) is documented in
// docs/MASTHEAD_CAN_SPEC.md. Keep that document and this header in sync.

// CAN identifier for wind frames from the masthead node.
constexpr uint32_t WIND_CAN_ID = 0x100;

// Wind frame payload layout:
//   Byte 0    : pulse_count (uint8)  — pulses since last frame (<= 25 expected)
//   Bytes 1-2 : adc_raw     (uint16) — wind-direction ADC counts (little-endian)
//   Bytes 3-6 : seq         (uint32) — frame counter, little-endian. Resets to 0
//               on masthead boot; lets us distinguish lost frames (counter jumps
//               forward) from a masthead reboot (counter resets to ~0).
constexpr uint8_t WIND_FRAME_DLC = 7;

// Minimum payload length carrying valid wind data (pulse + ADC). Frames without
// the appended sequence counter are still decoded (seq reported as 0).
constexpr uint8_t WIND_FRAME_MIN_DLC = 3;

// Maximum plausible pulse count per frame; above this the value is treated as
// an error and clamped to 0 (mirrors the Go reference pulseCount > 25 check).
constexpr uint8_t WIND_PULSE_MAX = 25;

// CAN bus bit rate (must match the remote masthead node).
constexpr uint32_t WIND_CAN_BITRATE = 500000; // 500 kbit/s

// Wind-direction ADC conversion.
// The remote node sends raw ADC counts for the direction vane. The Go reference
// read an ADS1115 configured for a 5 V range and fed the resulting voltage to
// voltToDegrees(). We reproduce that by converting counts -> volts here.
// NOTE: confirm these against the remote node firmware; adjust if the remote
// changes ADC resolution or reference (see AGENTS.md Rule 3 and
// docs/MASTHEAD_CAN_SPEC.md §3.2 — update this header rather than the
// buffer-insertion logic).
constexpr float WIND_ADC_FULL_SCALE_COUNTS = 65535.0f; // 16-bit full scale
constexpr float WIND_ADC_REF_VOLTS = 5.0f;             // ADS1115 5 V range

inline float windAdcToVolts(uint16_t counts) {
    return (static_cast<float>(counts) / WIND_ADC_FULL_SCALE_COUNTS) *
           WIND_ADC_REF_VOLTS;
}

struct WindFrame {
    uint8_t pulseCount; // pulses since last frame
    uint16_t adcRaw;    // wind-direction ADC counts
    uint32_t seq;       // frame counter; resets to 0 on masthead boot
    bool hasSeq;        // true if the frame carried the sequence counter
};

// Decode a raw CAN payload into a WindFrame. Returns false if the payload is
// too short to contain valid wind data.
inline bool decodeWindFrame(const uint8_t *data, uint8_t len, WindFrame &out) {
    if (data == nullptr || len < WIND_FRAME_MIN_DLC) {
        return false;
    }
    out.pulseCount = data[0];
    out.adcRaw = static_cast<uint16_t>(data[1]) |
                 (static_cast<uint16_t>(data[2]) << 8);
    if (len >= WIND_FRAME_DLC) {
        out.seq = static_cast<uint32_t>(data[3]) |
                  (static_cast<uint32_t>(data[4]) << 8) |
                  (static_cast<uint32_t>(data[5]) << 16) |
                  (static_cast<uint32_t>(data[6]) << 24);
        out.hasSeq = true;
    } else {
        out.seq = 0;
        out.hasSeq = false;
    }
    return true;
}
