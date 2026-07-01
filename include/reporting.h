#pragma once

#include <Arduino.h>
#include <Preferences.h>
#include <PubSubClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>

#include "anemometer.h"
#include "atmosphere.h"
#include "rainmeter.h"
#include "river.h"

// Computed weather values — mirrors the Go `weatherData` struct populated by
// prepData() in weather/reporting.go. Shared by MQTT (Phase 7), the HTTP/metrics
// endpoints (Phase 8) and the WOW upload (Phase 9).
struct WeatherValues {
    // Accumulating / persisted across reboots (NVS).
    double rainMM = 0.0;    // mm since last WOW send
    double rainIn = 0.0;    // inches since last WOW send
    double rainDayIn = 0.0; // inches today (reset 09:00)

    // Instantaneous.
    double pressureHpa = 0.0;  // station pressure (hPa)
    double pressureIn = 0.0;   // mean-sea-level pressure (inHg)
    double tempC = 0.0;        // outdoor temperature (C)
    double tempF = 0.0;        // outdoor temperature (F)
    double humidity = 0.0;     // % RH
    double dewPointF = 0.0;    // dew point (F)
    double windDir = 0.0;      // degrees
    double windSpeedMph = 0.0; // mph
    double windGustMph = 0.0;  // mph
};

// Reporting — ported from weather/reporting.go + the MQTT setup in main.go.
//
// Scheduling mirrors the Go reference: the work runs once per minute. Every
// minute prepData() refreshes the values and an MQTT message is published to
// `culverhay/weather`. The WOW MetOffice upload, DB write (n/a here) and NVS
// persistence happen every ReportFreqMin (15) minutes. The daily rain total is
// reset at 09:00 local time.
class Reporting {
public:
    Reporting(Atmosphere *atm, Rainmeter *rain, Anemometer *wind,
              RiverMonitor *river);

    // Load persisted rain totals and configure the MQTT client.
    void begin();

    // Call frequently from loop(): keeps MQTT alive and runs the per-minute work.
    void service();

    const WeatherValues &values() const { return v_; }
    bool mqttConnected() { return mqtt_.connected(); }

    // Recovery mode: when true the main loop suspends all sensor processing and
    // only MQTT (command/control) + OTA stay alive. Toggled by the "recover" /
    // "resume" MQTT commands.
    bool isRecoveryMode() const { return recoveryMode_; }

    // Force an immediate sensor refresh + MQTT publish (+ WOW upload when the
    // clock is valid). Triggered by the "report" MQTT command.
    void forceReport();

private:
    void prepData();
    void publishMqtt();
    bool mqttReconnect();
    void sendToWow();
    void loadPersisted();
    void savePersisted();

    // --- MQTT control plane (command / status / beacon) ---
    void onMqttMessage(char *topic, uint8_t *payload, unsigned int length);
    void handleCommand(String cmd);
    void publishStatus();
    void publishBeacon();

    Atmosphere *atm_;
    Rainmeter *rain_;
    Anemometer *wind_;
    RiverMonitor *river_;

    WiFiClientSecure net_;
    PubSubClient mqtt_;
    Preferences prefs_;
    WeatherValues v_;

    bool recoveryMode_ = false;
    unsigned long lastMinuteMs_ = 0;
    unsigned long lastBeaconMs_ = 0;
    unsigned long lastReconnectMs_ = 0;

    // Unique-per-device MQTT client ID (base name + chip MAC suffix). The
    // broker allows only one connection per client ID, so a shared/static ID
    // collides with the legacy Go station (or a second board) and the two kick
    // each other off. Built once in begin().
    String clientId_;
};
