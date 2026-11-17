#include "webserver.h"

#include <time.h>

#include "net.h"
#include "reboot_log.h"
#include "reset_reason.h"
#include "version.h"
#include "web_ui.h"
#include "weblog.h"
#define Serial Log // capture Serial output for the /logs web view

// Ported from the HTTP handler + Prometheus metrics in weather/main.go.

WeatherWebServer::WeatherWebServer(Atmosphere *atm, Rainmeter *rain,
                                   Anemometer *wind, RiverMonitor *river,
                                   Reporting *reporting)
    : server_(80), atm_(atm), rain_(rain), wind_(wind), river_(river),
      reporting_(reporting) {}

void WeatherWebServer::begin() {
    // The reboot history only changes at boot, so read it from NVS once here
    // rather than on the AsyncTCP task for every /system.json request.
    rebootHistory_ = rebootlog::historyCodes();

    server_.on("/", HTTP_GET,
               [this](AsyncWebServerRequest *r) { handleRoot(r); });
    server_.on("/data.json", HTTP_GET,
               [this](AsyncWebServerRequest *r) { handleData(r); });
    server_.on("/system.json", HTTP_GET,
               [this](AsyncWebServerRequest *r) { handleSystem(r); });
    server_.on("/settings.json", HTTP_GET,
               [this](AsyncWebServerRequest *r) { handleSettings(r); });
    server_.on("/settings.json", HTTP_POST,
               [this](AsyncWebServerRequest *r) { handleSettingsPost(r); });
    server_.on("/metrics", HTTP_GET,
               [this](AsyncWebServerRequest *r) { handleMetrics(r); });
    // The log viewer is now a tab of the UI; keep old /logs bookmarks working.
    server_.on("/logs", HTTP_GET,
               [](AsyncWebServerRequest *r) { r->redirect("/#logs"); });
    server_.on("/logs.json", HTTP_GET,
               [this](AsyncWebServerRequest *r) { handleLogsJson(r); });
    server_.begin();
    Serial.println("HTTP server started on :80 (web UI at /, /metrics)");
}

// GET / — the tabbed web UI for browsers. A client that does not ask for HTML
// (curl, scripts, anything written against the Go reference) still receives the
// JSON payload this path has always returned.
void WeatherWebServer::handleRoot(AsyncWebServerRequest *request) {
    const AsyncWebHeader *accept = request->getHeader("Accept");
    if (accept == nullptr || accept->value().indexOf("text/html") < 0) {
        handleData(request);
        return;
    }
    // Pointer + length overload: the page is streamed from flash in chunks
    // instead of being copied into a heap String.
    AsyncWebServerResponse *response = request->beginResponse(
        200, "text/html; charset=utf-8",
        reinterpret_cast<const uint8_t *>(kWebUiHtml), kWebUiHtmlLen);
    response->addHeader("Cache-Control", "no-cache");
    response->addHeader("Vary", "Accept");
    request->send(response);
}

// GET /data.json — live JSON payload (mirrors the Go `webdata` struct + handler).
void WeatherWebServer::handleData(AsyncWebServerRequest *request) {
    float pressure = 0.0f, humidity = 0.0f;
    if (atm_) {
        atm_->getHumidityAndPressure(pressure, humidity);
    }
    double tempC = atm_ ? atm_->getTemperature() : 0.0;
    double rainHr = rain_ ? rain_->getRate() : 0.0;
    double rainDay = rain_ ? rain_->getDayAccumulation() : 0.0;
    double windDir = wind_ ? wind_->getDirection() : 0.0;
    double windSpeed = wind_ ? wind_->getSpeed() : 0.0;
    double windGust = wind_ ? wind_->getGust() : 0.0;

    // Per-sensor health (active/working vs inactive/not working). Additive to
    // the payload — existing field names/units are unchanged (AGENTS.md Rule 1).
    // river uses the last-scrape result so a stale-but-initialised monitor still
    // shows a fault when the API stops responding.
    bool phOk = atm_ && atm_->hasPressureHumidity();
    bool tempOk = atm_ && atm_->hasTemperature();
    bool windOk = wind_ && wind_->isOnline();
    bool rainOk = rain_ && rain_->isOnline();
    bool riverOk = river_ && river_->lastScrapeSuccess();
    // Last known river level (metres). Additive, like `status`; it stays at
    // the last good value when a scrape fails, so read it with status.river.
    double riverLevel = river_ ? river_->level() : 0.0;

    // Go uses time.RFC822 = "02 Jan 06 15:04 MST".
    char timeStr[40];
    time_t tnow = time(nullptr);
    struct tm lt;
    localtime_r(&tnow, &lt);
    strftime(timeStr, sizeof(timeStr), "%d %b %y %H:%M %Z", &lt);

    // rain_rate mirrors the Go handler, which leaves webdata.RainRate unset (0);
    // only rain_mm_hr (GetRate) and rain_day (GetDayAccumulation) are populated.
    char body[512];
    snprintf(body, sizeof(body),
             "{\"time\":\"%s\",\"hiResTemp_C\":%.2f,\"humidity_RH\":%.2f,"
             "\"pressure_hPa\":%.2f,\"rain_mm_hr\":%.2f,\"rain_rate\":%.2f,"
             "\"rain_day\":%.2f,\"wind_dir\":%.2f,\"wind_speed\":%.2f,"
             "\"wind_gust\":%.2f,\"river_level\":%.3f,"
             "\"status\":{\"pressure_humidity\":%s,\"temperature\":%s,"
             "\"wind\":%s,\"rain\":%s,\"river\":%s}}",
             timeStr, tempC, humidity, pressure, rainHr, 0.0, rainDay, windDir,
             windSpeed, windGust, riverLevel, phOk ? "true" : "false",
             tempOk ? "true" : "false", windOk ? "true" : "false",
             rainOk ? "true" : "false", riverOk ? "true" : "false");

    request->send(200, "application/json", body);
}

// GET /system.json — device state for the Settings tab. Everything here is
// safe to read from the AsyncTCP task: no I2C, no NVS, no MQTT client calls.
void WeatherWebServer::handleSystem(AsyncWebServerRequest *request) {
    bool mqttUp = reporting_ && reporting_->mqttLinkUp();
    bool recovery = reporting_ && reporting_->isRecoveryMode();

    char body[512];
    snprintf(body, sizeof(body),
             "{\"version\":\"%s\",\"build\":%d,\"git\":\"%s\",\"uptime_s\":%lu,"
             "\"iface\":\"%s\",\"ip\":\"%s\",\"rssi\":%d,\"mqtt\":%s,"
             "\"recovery\":%s,\"heap_free\":%u,\"heap_min\":%u,"
             "\"heap_largest\":%u,\"boot_reason\":\"%s\",\"reboots\":\"%s\"}",
             _SEMVER_CORE, static_cast<int>(_VERSION_BUILD), _VERSION_GITHASH,
             millis() / 1000UL, net::iface(), net::ip().toString().c_str(),
             net::rssi(), mqttUp ? "true" : "false",
             recovery ? "true" : "false",
             static_cast<unsigned>(ESP.getFreeHeap()),
             static_cast<unsigned>(ESP.getMinFreeHeap()),
             static_cast<unsigned>(ESP.getMaxAllocHeap()), bootReasonString(),
             rebootHistory_.c_str());

    request->send(200, "application/json", body);
}

// GET /settings.json — the user-adjustable settings, plus the live wind
// direction before and after the mounting offset so the Settings tab can show
// what a new offset would do.
void WeatherWebServer::handleSettings(AsyncWebServerRequest *request) {
    int offset = wind_ ? wind_->getDirOffsetDeg() : 0;
    double raw = wind_ ? wind_->getRawDirection() : 0.0;
    bool windOk = wind_ && wind_->isOnline();

    char body[160];
    snprintf(body, sizeof(body),
             "{\"wind_dir_offset\":%d,\"wind_dir_raw\":%.1f,\"wind_dir\":%.1f,"
             "\"wind_online\":%s}",
             offset, raw, windApplyDirectionOffset(raw, offset),
             windOk ? "true" : "false");

    request->send(200, "application/json", body);
}

// POST /settings.json — change a setting (form-encoded body). Replies with the
// same document as the GET so the caller sees the value actually stored.
void WeatherWebServer::handleSettingsPost(AsyncWebServerRequest *request) {
    // A web page on another site can make a browser send a plain form post
    // here, but it cannot add a custom header without a CORS preflight, which
    // this server never grants. Requiring one stops a stray page changing
    // settings behind the user's back.
    if (!request->hasHeader("X-Requested-With")) {
        request->send(403, "text/plain", "X-Requested-With header required");
        return;
    }
    if (!request->hasParam("wind_dir_offset", /*post=*/true)) {
        request->send(400, "text/plain", "no known setting in request");
        return;
    }
    if (!wind_) {
        request->send(503, "text/plain", "wind sensor not available");
        return;
    }

    const String &value = request->getParam("wind_dir_offset", true)->value();
    char *end = nullptr;
    long deg = strtol(value.c_str(), &end, 10);
    if (value.length() == 0 || *end != '\0' || deg < -360 || deg > 360) {
        request->send(400, "text/plain",
                      "wind_dir_offset must be a whole number of degrees from "
                      "-360 to 360");
        return;
    }
    wind_->setDirOffsetDeg(static_cast<int>(deg));
    handleSettings(request);
}

// GET /metrics — Prometheus text exposition (metric names/types match main.go).
void WeatherWebServer::handleMetrics(AsyncWebServerRequest *request) {
    float pressure = 0.0f, humidity = 0.0f;
    if (atm_) {
        atm_->getHumidityAndPressure(pressure, humidity);
    }
    double tempC = atm_ ? atm_->getTemperature() : 0.0;
    double rainRate = rain_ ? rain_->getRate() : 0.0;
    double rainDay = rain_ ? rain_->getDayAccumulation() : 0.0;
    double windDir = wind_ ? wind_->getDirection() : 0.0;
    double windSpeed = wind_ ? wind_->getSpeed() : 0.0;
    double windGust = wind_ ? wind_->getGust() : 0.0;

    char buf[1024];
    snprintf(
        buf, sizeof(buf),
        "# HELP atmospheric_pressure Atmospheric pressure hPa\n"
        "# TYPE atmospheric_pressure gauge\n"
        "atmospheric_pressure %.4f\n"
        "# HELP rain_min_rate The rain rate based on the last 1 minutes\n"
        "# TYPE rain_min_rate gauge\n"
        "rain_min_rate %.4f\n"
        "# HELP rain_day The rain total today (9.01am - 9am)\n"
        "# TYPE rain_day counter\n"
        "rain_day %.4f\n"
        "# HELP relative_humidity Relative Humidity\n"
        "# TYPE relative_humidity gauge\n"
        "relative_humidity %.4f\n"
        "# HELP temperature Temperature C\n"
        "# TYPE temperature gauge\n"
        "temperature %.4f\n"
        "# HELP windspeed Average Wind Speed mph\n"
        "# TYPE windspeed gauge\n"
        "windspeed %.4f\n"
        "# HELP windgust Instant wind speed mph\n"
        "# TYPE windgust gauge\n"
        "windgust %.4f\n"
        "# HELP winddirection Wind Direction Deg\n"
        "# TYPE winddirection gauge\n"
        "winddirection %.4f\n",
        pressure, rainRate, rainDay, humidity, tempC, windSpeed, windGust,
        windDir);

    // River metrics — names preserved from the riverMonitor Go reference so
    // existing Prometheus/Grafana dashboards keep working. Appended to the same
    // exposition body via a second buffer.
    double riverLevel = river_ ? river_->level() : 0.0;
    double riverPeriod = river_ ? river_->period() : 0.0;
    double riverScrapeOk = (river_ && river_->lastScrapeSuccess()) ? 1.0 : 0.0;
    unsigned long riverSuccesses = river_ ? river_->fetchSuccesses() : 0;
    unsigned long riverErrors = river_ ? river_->fetchErrors() : 0;

    char riverBuf[1024];
    snprintf(
        riverBuf, sizeof(riverBuf),
        "# HELP riverlevel River level in meters.\n"
        "# TYPE riverlevel gauge\n"
        "riverlevel %.4f\n"
        "# HELP period River period.\n"
        "# TYPE period gauge\n"
        "period %.4f\n"
        "# HELP river_monitor_api_fetch_errors_total Total API fetch or parse errors.\n"
        "# TYPE river_monitor_api_fetch_errors_total counter\n"
        "river_monitor_api_fetch_errors_total %lu\n"
        "# HELP river_monitor_api_fetch_success_total Total successful API fetches.\n"
        "# TYPE river_monitor_api_fetch_success_total counter\n"
        "river_monitor_api_fetch_success_total %lu\n"
        "# HELP river_monitor_last_scrape_success 1 if the last scrape succeeded, otherwise 0.\n"
        "# TYPE river_monitor_last_scrape_success gauge\n"
        "river_monitor_last_scrape_success %.0f\n",
        riverLevel, riverPeriod, riverErrors, riverSuccesses, riverScrapeOk);

    AsyncResponseStream *response =
        request->beginResponseStream("text/plain; version=0.0.4");
    response->print(buf);
    response->print(riverBuf);
    request->send(response);
}

// GET /logs.json?after=<seq>[&limit=<n>] — log lines newer than the given
// sequence number, oldest first. `limit` caps the lines per response so the
// Logs tab pages through the buffer instead of pulling all of it at once: the
// whole 200-line buffer is ~15 KB of JSON and is held twice while sending.
void WeatherWebServer::handleLogsJson(AsyncWebServerRequest *request) {
    uint32_t after = 0;
    if (request->hasParam("after")) {
        after = static_cast<uint32_t>(
            strtoul(request->getParam("after")->value().c_str(), nullptr, 10));
    }
    size_t limit = 0;
    if (request->hasParam("limit")) {
        limit = static_cast<size_t>(
            strtoul(request->getParam("limit")->value().c_str(), nullptr, 10));
    }
    request->send(200, "application/json", Log.jsonSince(after, limit));
}

