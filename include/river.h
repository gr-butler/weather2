#pragma once

#include <Arduino.h>

// RiverMonitor — ported from the riverMonitor Go reference (../riverMonitor).
//
// The Go service polls the UK Environment Agency flood-monitoring API and
// exposes the latest river level (metres) and measurement period as Prometheus
// metrics. This class reproduces that behaviour on the ESP32: it fetches the
// station's `/measures` document every RiverPollIntervalMs, picks the item with
// the most recent reading (a station exposes several measures, some stale) and
// stores the level/period for the /metrics endpoint to serve.
//
// Metric names are preserved exactly from the Go reference so existing
// Prometheus/Grafana dashboards keep working (same philosophy as AGENTS.md
// Rule 1): riverlevel, period, river_monitor_api_fetch_errors_total,
// river_monitor_api_fetch_success_total, river_monitor_last_scrape_success.
class RiverMonitor {
public:
    // Perform an initial fetch (best-effort) and arm the poll timer.
    void begin();

    // Call frequently from loop(): fetches when the poll interval elapses and
    // the network is up. Non-blocking between polls.
    void service();

    // Latest values (metres / period). Valid once hasData() is true.
    double level() const { return level_; }
    double period() const { return period_; }

    // True once at least one successful fetch has populated level_/period_.
    bool hasData() const { return hasData_; }

    // 1 if the most recent scrape succeeded, else 0 (mirrors the Go gauge).
    bool lastScrapeSuccess() const { return lastScrapeSuccess_; }

    // Cumulative fetch outcome counters (mirror the Go counters).
    unsigned long fetchSuccesses() const { return fetchSuccesses_; }
    unsigned long fetchErrors() const { return fetchErrors_; }

private:
    // Fetch + parse + update state. Returns true on success.
    bool fetchAndUpdate();

    // Parse the measures JSON body; on success sets outLevel/outPeriod to the
    // most-recent reading. Returns false if no valid reading is present.
    bool parseRiverData(Stream &body, double &outLevel, double &outPeriod);

    double level_ = 0.0;
    double period_ = 0.0;
    bool hasData_ = false;
    bool lastScrapeSuccess_ = false;
    unsigned long fetchSuccesses_ = 0;
    unsigned long fetchErrors_ = 0;

    unsigned long lastPollMs_ = 0;
    bool armed_ = false; // force a fetch on the first service() after begin()
};
