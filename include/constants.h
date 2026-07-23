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
// Rolling direction smoothing window. Kept long because the (physically small)
// vane is buffeted and occasionally spins; a longer window lets the dwelling
// bearing dominate the spurious spin scatter. See windDirectionFiltered().
constexpr int WindDirectionAverageSeconds = 60; // rolling direction smoothing window
constexpr int AppWindSummarySeconds = 30; // app-facing MQTT wind summary window
// Wind-direction scatter rejection: a sample whose bearing is more than this
// many degrees from the modal (most-frequent) bearing in the window is treated
// as spin scatter from the buffeted vane and discarded before averaging.
constexpr float WindDirectionOutlierDeg = 45.0f;

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
// minute); only the WOW/DB/persist work is gated on ReportFreqMin. Here the
// data topic is published every MqttPublishIntervalMs (retained), while the
// WOW/persist work stays gated on ReportFreqMin.
constexpr int ReportFreqMin = 15;

// MQTT data-topic publish cadence. Published as a retained message every 30 s so
// the mobile app receives the latest reading immediately on (re)subscribe
// instead of waiting for the next update.
constexpr unsigned long MqttPublishIntervalMs = 30000;

// MQTT — these MUST NOT change (consumed by downstream dashboards).
constexpr const char *MqttTopic = "culverhay/weather";       // AGENTS.md Rule 1
constexpr const char *MqttAppTopic = "culverhay/weather/app"; // app-only smoothed payload
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

// --- River monitor ------------------------------------------------------
// Ported from the riverMonitor Go reference: poll the UK Environment Agency
// flood-monitoring API and expose the level/period as Prometheus metrics.
// Station id is a build flag (RIVER_STATION_ID) so it can be overridden per
// deployment; it defaults to the reference station (53125).
#ifndef RIVER_STATION_ID
#define RIVER_STATION_ID 53125
#endif
#define RIVER_STR_HELPER(x) #x
#define RIVER_STR(x) RIVER_STR_HELPER(x)

// Full measures endpoint for the configured station (matches the Go default).
constexpr const char *RiverApiUrl =
    "https://environment.data.gov.uk/flood-monitoring/id/stations/" RIVER_STR(
        RIVER_STATION_ID) "/measures";

// API poll interval — matches the Go reference default (-poll-interval 7m).
constexpr unsigned long RiverPollIntervalMs = 7UL * 60UL * 1000UL;

// HTTP timeouts for the river API fetch (ms). Kept well under the 30 s
// hardware watchdog so a stalled request cannot panic-reboot the device.
constexpr unsigned long RiverHttpConnectTimeoutMs = 10000;
constexpr unsigned long RiverHttpTimeoutMs = 15000;

// --- Telnet console -----------------------------------------------------
constexpr uint16_t TelnetPort = 23;
