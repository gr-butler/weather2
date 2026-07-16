#include "webserver.h"

#include <time.h>

#include "weblog.h"
#define Serial Log // capture Serial output for the /logs web view

// Ported from the HTTP handler + Prometheus metrics in weather/main.go.

WeatherWebServer::WeatherWebServer(Atmosphere *atm, Rainmeter *rain,
                                   Anemometer *wind, RiverMonitor *river)
    : server_(80), atm_(atm), rain_(rain), wind_(wind), river_(river) {}

void WeatherWebServer::begin() {
    server_.on("/", HTTP_GET,
               [this](AsyncWebServerRequest *r) { handleRoot(r); });
    server_.on("/metrics", HTTP_GET,
               [this](AsyncWebServerRequest *r) { handleMetrics(r); });
    server_.on("/logs", HTTP_GET,
               [this](AsyncWebServerRequest *r) { handleLogsPage(r); });
    server_.on("/logs.json", HTTP_GET,
               [this](AsyncWebServerRequest *r) { handleLogsJson(r); });
    server_.begin();
    Serial.println("HTTP server started on :80 (/, /metrics, /logs)");
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

    // Per-sensor health (active/working vs inactive/not working). Additive to
    // the payload — existing field names/units are unchanged (AGENTS.md Rule 1).
    // river uses the last-scrape result so a stale-but-initialised monitor still
    // shows a fault when the API stops responding.
    bool phOk = atm_ && atm_->hasPressureHumidity();
    bool tempOk = atm_ && atm_->hasTemperature();
    bool windOk = wind_ && wind_->isOnline();
    bool rainOk = rain_ && rain_->isOnline();
    bool riverOk = river_ && river_->lastScrapeSuccess();

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
             "\"wind_gust\":%.2f,"
             "\"status\":{\"pressure_humidity\":%s,\"temperature\":%s,"
             "\"wind\":%s,\"rain\":%s,\"river\":%s}}",
             timeStr, tempC, humidity, pressure, rainHr, 0.0, rainDay, windDir,
             windSpeed, windGust, phOk ? "true" : "false",
             tempOk ? "true" : "false", windOk ? "true" : "false",
             rainOk ? "true" : "false", riverOk ? "true" : "false");

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

// GET /logs.json?after=<seq> — new log lines since the given sequence number.
void WeatherWebServer::handleLogsJson(AsyncWebServerRequest *request) {
    uint32_t after = 0;
    if (request->hasParam("after")) {
        after = static_cast<uint32_t>(
            strtoul(request->getParam("after")->value().c_str(), nullptr, 10));
    }
    request->send(200, "application/json", Log.jsonSince(after));
}

// GET /logs — self-contained live log viewer that polls /logs.json.
void WeatherWebServer::handleLogsPage(AsyncWebServerRequest *request) {
    static const char kPage[] PROGMEM = R"HTML(<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>weather2 logs</title>
<style>
  :root { color-scheme: dark; }
  body { margin: 0; font-family: system-ui, sans-serif; background: #0b0f14; color: #d7dde5; }
  header { display: flex; gap: .75rem; align-items: center; flex-wrap: wrap;
           padding: .6rem .9rem; background: #131a22; border-bottom: 1px solid #223; position: sticky; top: 0; }
  header h1 { font-size: 1rem; margin: 0; font-weight: 600; }
  header .grow { flex: 1; }
  header label { font-size: .85rem; display: inline-flex; gap: .3rem; align-items: center; }
  button, input[type=text] { background: #1c2530; color: #d7dde5; border: 1px solid #33404f;
           border-radius: 6px; padding: .35rem .6rem; font-size: .85rem; }
  button:hover { background: #263143; cursor: pointer; }
  #status { font-size: .8rem; color: #8b97a5; }
  #log { padding: .5rem .9rem; font-family: ui-monospace, Menlo, Consolas, monospace;
         font-size: .82rem; line-height: 1.4; white-space: pre-wrap; word-break: break-word; }
  #log div { border-bottom: 1px solid #10161d; padding: 1px 0; }
  #log div:nth-child(odd) { background: #0e141b; }
</style>
</head>
<body>
<header>
  <h1>weather2 &middot; live log</h1>
  <span id="status">connecting…</span>
  <span class="grow"></span>
  <input id="filter" type="text" placeholder="filter…" autocomplete="off">
  <label><input id="auto" type="checkbox" checked> autoscroll</label>
  <button id="pause">Pause</button>
  <button id="clear">Clear</button>
</header>
<div id="log"></div>
<script>
  const logEl = document.getElementById('log');
  const statusEl = document.getElementById('status');
  const autoEl = document.getElementById('auto');
  const filterEl = document.getElementById('filter');
  const pauseBtn = document.getElementById('pause');
  let after = 0, paused = false, filter = '';

  filterEl.addEventListener('input', () => {
    filter = filterEl.value.toLowerCase();
    for (const div of logEl.children) {
      div.style.display = (!filter || div.textContent.toLowerCase().includes(filter)) ? '' : 'none';
    }
  });
  document.getElementById('clear').onclick = () => { logEl.innerHTML = ''; };
  pauseBtn.onclick = () => {
    paused = !paused;
    pauseBtn.textContent = paused ? 'Resume' : 'Pause';
  };

  function append(lines) {
    const atBottom = autoEl.checked;
    const frag = document.createDocumentFragment();
    for (const l of lines) {
      const div = document.createElement('div');
      div.textContent = l.t;
      if (filter && !l.t.toLowerCase().includes(filter)) div.style.display = 'none';
      frag.appendChild(div);
    }
    logEl.appendChild(frag);
    while (logEl.children.length > 1000) logEl.removeChild(logEl.firstChild);
    if (atBottom) window.scrollTo(0, document.body.scrollHeight);
  }

  async function poll() {
    if (!paused) {
      try {
        const r = await fetch('/logs.json?after=' + after);
        const d = await r.json();
        if (d.seq < after) after = 0;      // device rebooted — resync
        if (d.lines && d.lines.length) { append(d.lines); }
        after = d.seq;
        statusEl.textContent = 'live · ' + new Date().toLocaleTimeString();
      } catch (e) {
        statusEl.textContent = 'disconnected — retrying…';
      }
    }
    setTimeout(poll, 1000);
  }
  poll();
</script>
</body>
</html>)HTML";
    request->send(200, "text/html", kPage);
}

