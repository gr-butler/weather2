#pragma once

#include <ESPAsyncWebServer.h>

#include "anemometer.h"
#include "atmosphere.h"
#include "rainmeter.h"
#include "reporting.h"
#include "river.h"

// HTTP endpoints — ported from weather/main.go, plus the browser UI.
//
//   GET /            -> the tabbed web UI (Live / Logs / Settings, web_ui.h)
//                       when the client asks for HTML, i.e. a browser. Any
//                       other client gets the JSON below, as GET / always has.
//   GET /data.json   -> JSON `webdata` (live sensor read). Field names/units
//                       MUST match the Go struct tags exactly (AGENTS.md Rule 1):
//                       time, hiResTemp_C, humidity_RH, pressure_hPa, rain_mm_hr,
//                       rain_rate, rain_day, wind_dir, wind_speed, wind_gust
//                       Additive fields: `river_level` (metres) and a `status`
//                       object with per-sensor health flags (pressure_humidity,
//                       temperature, wind, rain, river); true = active/working,
//                       false = inactive/not working.
//   GET /system.json -> device state for the Settings tab (firmware, uptime,
//                       network, MQTT, heap, boot reason, reboot history).
//   GET /settings.json  -> user-adjustable settings (wind direction offset)
//                          plus the wind direction before/after that offset.
//   POST /settings.json -> change a setting; form field wind_dir_offset
//                          (degrees). Needs an X-Requested-With header.
//   GET /logs.json   -> buffered log lines newer than ?after=<seq>.
//   GET /logs        -> redirect to the Logs tab (/#logs).
//   GET /metrics     -> Prometheus text format. Metric names/units MUST match
//                       the Go gauge definitions exactly (AGENTS.md Rule 1):
//                       atmospheric_pressure, rain_min_rate, rain_day,
//                       relative_humidity, temperature, windspeed, windgust,
//                       winddirection
//
// Uses ESPAsyncWebServer (already a dependency via ElegantOTA). The same server
// instance is shared with ElegantOTA in the main integration (Phase 10) via
// server(). Handlers run on the AsyncTCP task, not loop().
class WeatherWebServer {
public:
    WeatherWebServer(Atmosphere *atm, Rainmeter *rain, Anemometer *wind,
                     RiverMonitor *river, Reporting *reporting);

    // Register routes and start the async HTTP server on port 80.
    void begin();

    // No-op for the async server (requests are serviced on the AsyncTCP task);
    // kept for a uniform "service from loop()" interface.
    void handle() {}

    // Expose the underlying server so ElegantOTA can attach to it (Phase 10).
    AsyncWebServer &server() { return server_; }

private:
    void handleRoot(AsyncWebServerRequest *request);
    void handleData(AsyncWebServerRequest *request);
    void handleSystem(AsyncWebServerRequest *request);
    void handleSettings(AsyncWebServerRequest *request);
    void handleSettingsPost(AsyncWebServerRequest *request);
    void handleMetrics(AsyncWebServerRequest *request);
    void handleLogsJson(AsyncWebServerRequest *request);

    AsyncWebServer server_;
    Atmosphere *atm_;
    Rainmeter *rain_;
    Anemometer *wind_;
    RiverMonitor *river_;
    Reporting *reporting_;
    String rebootHistory_; // read from NVS once in begin()
};
