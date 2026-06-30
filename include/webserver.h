#pragma once

#include <ESPAsyncWebServer.h>

#include "anemometer.h"
#include "atmosphere.h"
#include "rainmeter.h"

// HTTP endpoints — ported from weather/main.go.
//
//   GET /        -> JSON `webdata` (live sensor read). Field names/units MUST
//                   match the Go struct tags exactly (AGENTS.md Rule 1):
//                   time, hiResTemp_C, humidity_RH, pressure_hPa, rain_mm_hr,
//                   rain_rate, rain_day, wind_dir, wind_speed, wind_gust
//   GET /metrics -> Prometheus text format. Metric names/units MUST match the
//                   Go gauge definitions exactly (AGENTS.md Rule 1):
//                   atmospheric_pressure, rain_min_rate, rain_day,
//                   relative_humidity, temperature, windspeed, windgust,
//                   winddirection
//
// Uses ESPAsyncWebServer (already a dependency via ElegantOTA). The same server
// instance is shared with ElegantOTA in the main integration (Phase 10) via
// server().
class WeatherWebServer {
public:
    WeatherWebServer(Atmosphere *atm, Rainmeter *rain, Anemometer *wind);

    // Register routes and start the async HTTP server on port 80.
    void begin();

    // No-op for the async server (requests are serviced on the AsyncTCP task);
    // kept for a uniform "service from loop()" interface.
    void handle() {}

    // Expose the underlying server so ElegantOTA can attach to it (Phase 10).
    AsyncWebServer &server() { return server_; }

private:
    void handleRoot(AsyncWebServerRequest *request);
    void handleMetrics(AsyncWebServerRequest *request);

    AsyncWebServer server_;
    Atmosphere *atm_;
    Rainmeter *rain_;
    Anemometer *wind_;
};
