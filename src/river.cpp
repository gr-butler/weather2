#include "river.h"

#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>

#include "constants.h"
#include "net.h"

// Ported from riverMonitor/main.go (fetchAndUpdate + parseRiverData + pollLoop).
// Ingestion path differs (ESP32 HTTPClient vs Go net/http) but the selection
// logic — pick the item whose latestReading is newest — is preserved.

void RiverMonitor::begin() {
    lastPollMs_ = millis();
    armed_ = true; // trigger an initial fetch on the first service() tick
    Serial.printf("River monitor ready (url=%s)\n", RiverApiUrl);
}

void RiverMonitor::service() {
    unsigned long now = millis();
    if (!armed_ && (now - lastPollMs_ < RiverPollIntervalMs)) {
        return;
    }
    // Skip (and re-arm for the next tick) while the network is down, so the
    // first successful poll happens as soon as connectivity returns.
    if (!net::isUp()) {
        lastPollMs_ = now;
        return;
    }
    armed_ = false;
    lastPollMs_ = now;

    if (!fetchAndUpdate()) {
        // Error path already logged + counters bumped in fetchAndUpdate().
    }
}

bool RiverMonitor::fetchAndUpdate() {
    // The Environment Agency API is HTTPS only. We don't pin its certificate
    // (public, read-only flood data), so validate-none keeps this simple and
    // robust to CA rotation — matching the "just fetch it" Go client.
    WiFiClientSecure client;
    client.setInsecure();

    HTTPClient http;
    http.setConnectTimeout(RiverHttpConnectTimeoutMs);
    http.setTimeout(RiverHttpTimeoutMs);
    if (!http.begin(client, RiverApiUrl)) {
        Serial.println("River: failed to begin HTTPS request");
        fetchErrors_++;
        lastScrapeSuccess_ = false;
        return false;
    }

    int code = http.GET();
    if (code != HTTP_CODE_OK) {
        Serial.printf("River: unexpected status code %d\n", code);
        http.end();
        fetchErrors_++;
        lastScrapeSuccess_ = false;
        return false;
    }

    double level = 0.0, period = 0.0;
    bool ok = parseRiverData(http.getStream(), level, period);
    http.end();

    if (!ok) {
        Serial.println("River: no valid readings in response");
        fetchErrors_++;
        lastScrapeSuccess_ = false;
        return false;
    }

    level_ = level;
    period_ = period;
    hasData_ = true;
    fetchSuccesses_++;
    lastScrapeSuccess_ = true;
    Serial.printf("River level=%.3f period=%.3f\n", level_, period_);
    return true;
}

bool RiverMonitor::parseRiverData(Stream &body, double &outLevel,
                                  double &outPeriod) {
    // A station exposes several measures; only keep the fields we need so the
    // (potentially multi-KB) document parses within the ESP32 heap budget.
    JsonDocument filter;
    filter["items"][0]["period"] = true;
    filter["items"][0]["latestReading"]["dateTime"] = true;
    filter["items"][0]["latestReading"]["value"] = true;

    JsonDocument doc;
    DeserializationError err =
        deserializeJson(doc, body, DeserializationOption::Filter(filter));
    if (err) {
        Serial.printf("River: JSON parse error: %s\n", err.c_str());
        return false;
    }

    JsonArray items = doc["items"].as<JsonArray>();
    if (items.isNull() || items.size() == 0) {
        Serial.println("River: response contains no items");
        return false;
    }

    // Pick the item with the newest latestReading. RFC3339 UTC timestamps in a
    // fixed format ("2023-11-14T12:00:00Z") are lexicographically ordered, so a
    // plain string compare reproduces the Go time.After() selection without a
    // date parser. Some items have a stale/missing reading and are skipped.
    bool found = false;
    const char *bestTime = nullptr;
    for (JsonObject item : items) {
        JsonObject reading = item["latestReading"];
        if (reading.isNull()) {
            continue;
        }
        const char *dt = reading["dateTime"];
        if (dt == nullptr) {
            continue;
        }
        if (!found || strcmp(dt, bestTime) > 0) {
            found = true;
            bestTime = dt;
            outLevel = reading["value"] | 0.0;
            outPeriod = item["period"] | 0.0;
        }
    }

    return found;
}
