#pragma once

// Ported from weather/env/constants.go (Go reference implementation).
// These values MUST match the reference exactly — see AGENTS.md Rules 2, 4, 5.

// --- Wind ---------------------------------------------------------------
// mph per anemometer pulse (tick)
constexpr float MphPerTick = 1.429f;

// Wind is sampled at high frequency (every 0.25 sec) per Met Office guidance.
// https://www.metoffice.gov.uk/weather/guides/observations/how-we-measure-wind
constexpr int WindSamplesPerSecond = 4;     // one sample every 250 ms
constexpr int WindBufferLengthSeconds = 60; // 1-minute rolling window

// Number of slots in the wind circular buffers: 4 * 60 = 240
constexpr int WindBufferSamples = WindSamplesPerSecond * WindBufferLengthSeconds;

// Sanity clamps (mph)
constexpr float WindSpeedMaxMph = 100.0f; // above this, speed treated as error -> 0
constexpr float WindGustMaxMph = 120.0f;  // above this, reuse previous valid gust

// --- Rain ---------------------------------------------------------------
// mm of rainfall per tipping-bucket tip.
// https://www.robotics.org.za/WH-SP-RG
constexpr float MmPerTip = 0.3537f;

// Rain buffer: one slot per minute for the last hour.
constexpr int RainBufferMinutes = 60;

// Bucket-tip debounce (matches Go gpioutil.Debounce parameters).
constexpr unsigned long RainDebounceGlitchMs = 10;  // ignore glitches < 10 ms
constexpr unsigned long RainDebounceRepeatMs = 500; // ignore repeated edges within 500 ms

// --- Unit conversions (for WOW MetOffice API) ---------------------------
constexpr float HPaToInHg = 0.02953f; // hPa -> inches of mercury
constexpr float MmToInch = 25.4f;     // mm  -> inches (divide mm by this)

// --- Reporting ----------------------------------------------------------
// WOW MetOffice / DB / persistence interval (minutes). Matches env.ReportFreqMin.
// NOTE: the Go reference publishes MQTT every minute (its loop ticks once a
// minute); only the WOW/DB/persist work is gated on ReportFreqMin. We mirror
// that here — MQTT publishes every minute, WOW/persist every ReportFreqMin.
constexpr int ReportFreqMin = 15;

// MQTT — these MUST NOT change (consumed by downstream dashboards).
constexpr const char *MqttTopic = "culverhay/weather";       // AGENTS.md Rule 1
constexpr const char *MqttClientId = "weather-mqtt-client";  // matches Go clientID
constexpr const char *MqttClientName = "weather_station";    // Go dataMap "name"

// MQTT control plane (modelled on the Waterbutt project). These are NEW topics
// and do not affect the locked data topic above.
constexpr const char *MqttStationId = "weatherstation";              // device identity
constexpr const char *MqttCommandTopic = "culverhay/weather/command"; // subscribe: commands in
constexpr const char *MqttStatusTopic = "culverhay/weather/status";   // publish: command replies
constexpr const char *MqttBeaconTopic = "beacon/weatherstation";      // publish: periodic presence
constexpr unsigned long MqttBeaconIntervalMs = 30000;    // broadcast presence every 30 s
constexpr unsigned long MqttReconnectIntervalMs = 15000; // throttle reconnect attempts

// --- LEDs / heartbeat ---------------------------------------------------
constexpr unsigned long LEDFlashDurationMs = 50; // matches env.LEDFlashDuration
constexpr unsigned long HeartbeatIntervalMs = 60000; // heartbeat flash every 60 s
