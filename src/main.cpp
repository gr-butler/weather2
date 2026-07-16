#include <Arduino.h>
#include <ElegantOTA.h>
#include <Wire.h>
#include <esp_task_wdt.h>
#include <time.h>

#include "anemometer.h"
#include "atmosphere.h"
#include "constants.h"
#include "led.h"
#include "net.h"
#include "rainmeter.h"
#include "reporting.h"
#include "reset_reason.h"
#include "river.h"
#include "secrets.h"
#include "version.h"
#include "weblog.h"
#include "webserver.h"
#define Serial Log // capture Serial output for the /logs web view

// =============================================================================
//  Weather Station — ESP32 main loop.
//  Ports the wiring/orchestration of weather/main.go: init sensors, connect
//  WiFi, sync NTP, start the web service + OTA, and run the reporting loop.
// =============================================================================

namespace {
// Watchdog: panic-reboot if loop() stalls for this long.
constexpr uint32_t kWatchdogTimeoutS = 30;

// Timezone for Europe/London (GMT/BST). localtime_r() then gives local time
// (used for the 09:00 rain reset + MQTT timestamp); gmtime_r() gives UTC
// (used for the WOW dateutc field). Matches the Pi reference's local clock.
constexpr const char *kTimezone = "GMT0BST,M3.5.0/1,M10.5.0";

// Hardware objects.
Led heartbeatLed;
Led rainTipLed;
Atmosphere atmosphere;
Rainmeter rainmeter;
Anemometer anemometer;
RiverMonitor riverMonitor;

// DISABLE_RAIN_PIN=1 keeps the rain meter fully initialised (all buffer/rate
// logic runs) but never attaches the interrupt, so the pin is never read and
// the tip count is guaranteed to stay 0. Defaults to enabled (pin read).
#ifndef DISABLE_RAIN_PIN
#define DISABLE_RAIN_PIN 0
#endif

Reporting reporting(&atmosphere, &rainmeter, &anemometer, &riverMonitor);
WeatherWebServer webServer(&atmosphere, &rainmeter, &anemometer, &riverMonitor);

unsigned long lastHeartbeatMs = 0;

void syncTime() {
    // configTzTime sets the TZ and starts SNTP. Non-blocking; we wait briefly.
    configTzTime(kTimezone, "pool.ntp.org", "time.nist.gov", "time.google.com");
    Serial.print("Waiting for NTP time");
    time_t now = time(nullptr);
    for (int i = 0; i < 20 && now < 1700000000; i++) {
        delay(500);
        Serial.print('.');
        now = time(nullptr);
    }
    if (now >= 1700000000) {
        struct tm lt;
        localtime_r(&now, &lt);
        char buf[32];
        strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &lt);
        Serial.printf("\nNTP time synced: %s\n", buf);
    } else {
        Serial.println("\nNTP sync failed — wall-clock features deferred");
    }
}

// Log why the ESP32 just booted — power-on / deliberate reboot vs an abnormal
// crash, brownout or watchdog. Printed first thing in setup() so the cause of
// any unexpected restart is always visible in the serial log and the /logs view.
void logResetReason() {
    if (bootReasonAbnormal()) {
        Serial.printf("*** Reset reason: %s  <-- ABNORMAL RESTART ***\n",
                      bootReasonString());
    } else {
        Serial.printf("Reset reason: %s\n", bootReasonString());
    }
}
} // namespace

void setup() {
    Serial.begin(115200);
    delay(2000);
    Serial.flush();
    Serial.printf("\n\nStarting weather station [%s]\n", _VERSION);
    logResetReason();

    // LEDs first so we get visible startup feedback.
    heartbeatLed.begin("Heartbeat LED", HEARTBEAT_LED_PIN);
    rainTipLed.begin("Rain Tip LED", RAIN_TIP_LED_PIN);

    // I2C bus for the atmospheric sensors (BME280 + MCP9808).
    Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);

    Serial.println("Initializing sensors...");
    if (atmosphere.begin()) {
        Serial.println("  Atmosphere: online");
    } else {
        Serial.println("  Atmosphere: OFFLINE (continuing without it)");
    }
#if DISABLE_RAIN_PIN
    rainmeter.begin(RAIN_PIN, &rainTipLed, /*readPin=*/false);
    Serial.println("  Rain: online (PIN DISABLED — interrupt not attached, no input)");
#else
    if (rainmeter.begin(RAIN_PIN, &rainTipLed)) {
        Serial.println("  Rain: online");
    } else {
        Serial.println("  Rain: OFFLINE (continuing without it)");
    }
#endif
    if (anemometer.begin()) {
        Serial.println("  Wind (CAN): online");
    } else {
        Serial.println("  Wind (CAN): OFFLINE (continuing without it)");
    }

    net::begin();
    syncTime();

    reporting.begin();

    // River monitor: polls the Environment Agency flood API (needs network).
    riverMonitor.begin();

    // Web service: live JSON (/) + Prometheus (/metrics), plus OTA at /update.
    webServer.begin();
    ElegantOTA.begin(&webServer.server());
    Serial.println("ElegantOTA available at /update");

    // Watchdog — reboot if loop() stalls (e.g. a hung network call).
    esp_task_wdt_init(kWatchdogTimeoutS, /*panic=*/true);
    esp_task_wdt_add(nullptr);

    lastHeartbeatMs = millis();
    Serial.println("Setup complete");
}

void loop() {
    ElegantOTA.loop();

    // Keep the network up: Ethernet is primary, WiFi is the fallback. Reboots
    // only as a last resort if BOTH links stay down (see src/net.cpp).
    net::service();

    // In recovery mode we suspend all sensor processing and keep only MQTT
    // (command/control) + OTA alive, so the device stays remotely reachable.
    if (!reporting.isRecoveryMode()) {
        // Rain bookkeeping. Wind (CAN) is serviced by its own core-0 task, so
        // it keeps running even here and needs no per-loop servicing.
        rainmeter.update();
    }

    // Reporting: MQTT command/beacon servicing always runs (incl. recovery);
    // the per-minute publish + 15-min WOW upload + 09:00 rain reset are gated
    // internally on recovery mode.
    reporting.service();

    // River monitor: fetch the Environment Agency river level on its own poll
    // schedule (every 7 min). Suspended in recovery mode like the sensors.
    if (!reporting.isRecoveryMode()) {
        riverMonitor.service();
    }

    // Heartbeat LED every 60 s (matches the Go reference).
    unsigned long now = millis();
    if (now - lastHeartbeatMs >= HeartbeatIntervalMs) {
        lastHeartbeatMs += HeartbeatIntervalMs;
        heartbeatLed.flash();
    }
    heartbeatLed.service();
    rainTipLed.service();

    esp_task_wdt_reset();
}