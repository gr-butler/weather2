#include "webserver.h"

#include <time.h>

// Ported from the HTTP handler + Prometheus metrics in weather/main.go.

WeatherWebServer::WeatherWebServer(Atmosphere *atm, Rainmeter *rain,
                                   Anemometer *wind, RiverMonitor *river)
    : server_(80), atm_(atm), rain_(rain), wind_(wind), river_(river) {}

void WeatherWebServer::begin() {
    server_.on("/", HTTP_GET,
               [this](AsyncWebServerRequest *r) { handleRoot(r); });
    server_.on("/metrics", HTTP_GET,
               [this](AsyncWebServerRequest *r) { handleMetrics(r); });
    server_.begin();
    Serial.println("HTTP server started on :80 (/ and /metrics)");
}

// GET / — live JSON payload (mirrors the Go `webdata` struct + handler).
void WeatherWebServer::handleRoot(AsyncWebServerRequest *request) {
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

    // Go uses time.RFC822 = "02 Jan 06 15:04 MST".
    char timeStr[40];
    time_t tnow = time(nullptr);
    struct tm lt;
    localtime_r(&tnow, &lt);
    strftime(timeStr, sizeof(timeStr), "%d %b %y %H:%M %Z", &lt);

    // rain_rate mirrors the Go handler, which leaves webdata.RainRate unset (0);
    // only rain_mm_hr (GetRate) and rain_day (GetDayAccumulation) are populated.
    char body[320];
    snprintf(body, sizeof(body),
             "{\"time\":\"%s\",\"hiResTemp_C\":%.2f,\"humidity_RH\":%.2f,"
             "\"pressure_hPa\":%.2f,\"rain_mm_hr\":%.2f,\"rain_rate\":%.2f,"
             "\"rain_day\":%.2f,\"wind_dir\":%.2f,\"wind_speed\":%.2f,"
             "\"wind_gust\":%.2f}",
             timeStr, tempC, humidity, pressure, rainHr, 0.0, rainDay, windDir,
             windSpeed, windGust);

    request->send(200, "application/json", body);
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

    char riverBuf[640];
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
