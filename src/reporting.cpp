#include "reporting.h"

#include <HTTPClient.h>
#include <ctype.h>
#include <time.h>

#include "constants.h"
#include "conversions.h"
#include "mqtt_ca_cert.h"
#include "net.h"
#include "secrets.h"
#include "version.h"
#include "weblog.h"
#define Serial Log // capture Serial output for the /logs web view

// Ported from weather/reporting.go (prepData + Reporting loop + WOW upload) and
// the MQTT setup in weather/main.go. See reporting.h for the scheduling model.

namespace {
// NVS namespace + keys for persisting rain totals across reboots.
constexpr const char *kPrefsNamespace = "weather";
constexpr const char *kKeyRainMM = "rainMM";
constexpr const char *kKeyRainIn = "rainIn";
constexpr const char *kKeyRainDayIn = "rainDayIn";

// Only trust wall-clock decisions (09:00 reset, ReportFreqMin gating) once NTP
// has set the clock — before then time() returns a 1970 epoch.
constexpr time_t kClockValidEpoch = 1700000000; // 2023-11-14

// WOW MetOffice upload (weather/reporting.go baseUrl + softwaretype).
constexpr const char *kWowBaseUrl =
    "http://wow.metoffice.gov.uk/automaticreading?";
constexpr const char *kSoftwareType = "weather2-esp32/" _SEMVER_CORE;

// Percent-encode a query value (matches Go url.Values.Encode: space->%20,
// '+'->%2B, ':'->%3A; unreserved chars -_.~ pass through).
String urlEncode(const char *s) {
    String out;
    for (const char *p = s; *p; ++p) {
        char c = *p;
        if (isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_' ||
            c == '.' || c == '~') {
            out += c;
        } else {
            char buf[4];
            snprintf(buf, sizeof(buf), "%%%02X", static_cast<unsigned char>(c));
            out += buf;
        }
    }
    return out;
}

// Fixed-precision numeric query parameter (digits, '.', '-' are URL-safe).
String floatParam(double v, int decimals) {
    char buf[24];
    snprintf(buf, sizeof(buf), "%.*f", decimals, v);
    return String(buf);
}
} // namespace

Reporting::Reporting(Atmosphere *atm, Rainmeter *rain, Anemometer *wind,
                     RiverMonitor *river)
    : atm_(atm), rain_(rain), wind_(wind), river_(river), mqtt_(net_) {}

void Reporting::begin() {
    loadPersisted();
    // TLS transport: validate the broker against its CA certificate.
    net_.setCACert(MQTT_CA_CERT);
    mqtt_.setServer(MQTT_BROKER_HOST, MQTT_BROKER_PORT);
    // PubSubClient tuning (see Waterbutt reference project):
    //  - larger TX/RX buffer: the JSON payload + topic + header exceeds the
    //    256-byte default headroom comfortably.
    //  - keepalive (60 s): PubSubClient sends a PINGREQ once the keepalive
    //    elapses and then drops the link itself (state -4) if the PINGRESP is
    //    not *read* before the next check. Blocking TLS HTTP fetches (river
    //    poll, WOW upload) can starve mqtt_.loop() for 15-30 s, so a 30 s
    //    keepalive left no margin and the client kept timing out its own ping.
    //    60 s (matching the stable sibling projects) tolerates a full blocking
    //    fetch without dropping.
    //  - socket timeout: a stalled TLS connect must not approach the 30 s
    //    hardware watchdog.
    mqtt_.setBufferSize(1024);
    mqtt_.setKeepAlive(60);
    mqtt_.setSocketTimeout(15);
    // Route incoming command-topic messages to onMqttMessage(). PubSubClient on
    // ESP32 takes a std::function, so we bind the member directly.
    mqtt_.setCallback(
        [this](char *topic, uint8_t *payload, unsigned int length) {
            this->onMqttMessage(topic, payload, length);
        });
    // Unique client ID = base + lower 32 bits of the chip MAC (stable across
    // reboots, but distinct from the legacy Go station's "weather-mqtt-client").
    // A shared client ID makes the broker kick one connection when the other
    // (re)connects, which silently drops published messages.
    char idbuf[48];
    snprintf(idbuf, sizeof(idbuf), "%s-%08X", MqttClientId,
             (uint32_t)ESP.getEfuseMac());
    clientId_ = idbuf;
    lastMinuteMs_ = millis();
    lastPublishMs_ = millis();
    Serial.printf("Reporting ready (rainMM=%.2f rainDayIn=%.3f) mqttId=%s\n",
                  v_.rainMM, v_.rainDayIn, clientId_.c_str());
}

void Reporting::loadPersisted() {
    prefs_.begin(kPrefsNamespace, /*readOnly=*/true);
    v_.rainMM = prefs_.getDouble(kKeyRainMM, 0.0);
    v_.rainIn = prefs_.getDouble(kKeyRainIn, 0.0);
    v_.rainDayIn = prefs_.getDouble(kKeyRainDayIn, 0.0);
    prefs_.end();
}

void Reporting::savePersisted() {
    prefs_.begin(kPrefsNamespace, /*readOnly=*/false);
    prefs_.putDouble(kKeyRainMM, v_.rainMM);
    prefs_.putDouble(kKeyRainIn, v_.rainIn);
    prefs_.putDouble(kKeyRainDayIn, v_.rainDayIn);
    prefs_.end();
}

// Refresh the computed values from the sensors (mirrors Go prepData).
void Reporting::prepData() {
    if (atm_ && atm_->isOnline()) {
        float pressure = 0.0f, humidity = 0.0f;
        atm_->getHumidityAndPressure(pressure, humidity);
        double tempC = atm_->getTemperature();

        v_.tempC = tempC;
        v_.tempF = celsiusToF(tempC);
        v_.pressureHpa = pressure;
        v_.humidity = humidity;
        v_.pressureIn = mslpInHg(pressure, tempC);
        v_.dewPointF = dewPointF(tempC, humidity);
    }

    if (rain_) {
        // GetAccumulation reads and resets the since-last counter (mm).
        double acc = rain_->getAccumulation();
        v_.rainMM += acc;
        double inch = mmToInch(acc);
        v_.rainIn += inch;
        v_.rainDayIn += inch;
    }

    if (wind_) {
        v_.windDir = wind_->getDirection();
        v_.windSpeedMph = wind_->getSpeed();
        v_.windGustMph = wind_->getGust();
    }
}

bool Reporting::mqttReconnect() {
    if (!net::isUp()) {
        return false;
    }
    if (mqtt_.connected()) {
        return true;
    }
    Serial.print("MQTT connecting (TLS)... ");
    // Authenticate with broker credentials when supplied (EMQX/TLS brokers
    // typically require this); fall back to anonymous connect otherwise.
    bool ok;
    if (strlen(MQTT_USERNAME) > 0) {
        ok = mqtt_.connect(clientId_.c_str(), MQTT_USERNAME, MQTT_PASSWORD);
    } else {
        ok = mqtt_.connect(clientId_.c_str());
    }
    if (ok) {
        Serial.println("connected");
        // (Re)subscribe to the command topic on every (re)connect.
        if (mqtt_.subscribe(MqttCommandTopic)) {
            Serial.printf("Subscribed to command topic %s\n", MqttCommandTopic);
        } else {
            Serial.printf("Failed to subscribe to %s\n", MqttCommandTopic);
        }
        return true;
    }
    Serial.printf("failed, rc=%d\n", mqtt_.state());
    return false;
}

// Build and publish the MQTT JSON payload. Field names + 2-dp string formatting
// are preserved exactly from the Go dataMap (weather/reporting.go).
void Reporting::publishMqtt() {
    String ip = net::ip().toString();

    // Go time format "15:04:05 02/01/2006" => HH:MM:SS DD/MM/YYYY (local time).
    char timeStr[24];
    time_t tnow = time(nullptr);
    struct tm lt;
    localtime_r(&tnow, &lt);
    snprintf(timeStr, sizeof(timeStr), "%02d:%02d:%02d %02d/%02d/%04d", lt.tm_hour,
             lt.tm_min, lt.tm_sec, lt.tm_mday, lt.tm_mon + 1, lt.tm_year + 1900);

    char payload[320];
    snprintf(payload, sizeof(payload),
             "{\"name\":\"%s\",\"ip_address\":\"%s\",\"time\":\"%s\","
             "\"rain\":\"%.2f\",\"temp\":\"%.2f\",\"windspeed\":\"%.2f\","
             "\"windgust\":\"%.2f\",\"winddir\":\"%.2f\",\"humidity\":\"%.2f\","
             "\"rain_mm_hr\":\"%.2f\",\"river_level\":\"%.3f\"}",
             MqttClientName, ip.c_str(), timeStr, v_.rainMM, v_.tempC,
             v_.windSpeedMph, v_.windGustMph, v_.windDir, v_.humidity,
             rain_ ? rain_->getRate() : 0.0f, river_ ? river_->level() : 0.0f);

    if (mqtt_.publish(MqttTopic, payload, /*retained=*/true)) {
        Serial.printf("MQTT published to %s\n", MqttTopic);
    } else {
        Serial.println("MQTT publish failed");
    }
}

// Upload to WOW MetOffice (ported from weather/reporting.go). On a successful
// (HTTP 200) send, the since-last-report rain totals are cleared — matching the
// Go reference, which only resets RainIn/RainMM after a confirmed upload. The
// daily total (rainDayIn) is NOT reset here; that happens at 09:00.
void Reporting::sendToWow() {
    if (strlen(WOW_SITE_ID) == 0) {
        return; // WOW disabled (no site id configured)
    }
    if (!net::isUp()) {
        Serial.println("WOW: network down, skipping upload");
        return;
    }

    // dateutc — UTC, Go format "2006-01-02+15:04:05" then URL-encoded
    // (so '+' -> %2B, ':' -> %3A), reproducing the reference exactly.
    char dateRaw[24];
    time_t tnow = time(nullptr);
    struct tm gmt;
    gmtime_r(&tnow, &gmt);
    strftime(dateRaw, sizeof(dateRaw), "%Y-%m-%d+%H:%M:%S", &gmt);

    String url = kWowBaseUrl;
    url += "siteid=" + urlEncode(WOW_SITE_ID);
    url += "&siteAuthenticationKey=" + urlEncode(WOW_PIN);
    url += "&dateutc=" + urlEncode(dateRaw);
    url += "&softwaretype=" + urlEncode(kSoftwareType);
    url += "&baromin=" + floatParam(v_.pressureIn, 4);
    url += "&dailyrainin=" + floatParam(v_.rainDayIn, 4);
    url += "&rainin=" + floatParam(v_.rainIn, 4);
    url += "&humidity=" + floatParam(v_.humidity, 2);
    url += "&tempf=" + floatParam(v_.tempF, 2);
    url += "&dewptf=" + floatParam(v_.dewPointF, 2);
    url += "&winddir=" + floatParam(v_.windDir, 2);
    url += "&windspeedmph=" + floatParam(v_.windSpeedMph, 2);
    url += "&windgustmph=" + floatParam(v_.windGustMph, 2);

#ifdef WOW_DRY_RUN
    // Dev/bench build: the readings are not real weather, so never POST to the
    // Met Office. Log the URL that would have been sent and leave the rain
    // totals untouched (a real upload only clears them on HTTP 200).
    Serial.print("WOW dry-run (not sending): ");
    Serial.println(url);
    return;
#endif

    HTTPClient http;
    http.setConnectTimeout(15000);
    http.setTimeout(30000);
    if (!http.begin(url)) {
        Serial.println("WOW: failed to begin HTTP request");
        return;
    }
    int code = http.GET();
    if (code == 200) {
        Serial.println("WOW upload OK; resetting rainIn/rainMM");
        v_.rainMM = 0.0;
        v_.rainIn = 0.0;
    } else {
        Serial.printf("WOW upload failed: HTTP %d\n", code);
    }
    http.end();
}

void Reporting::service() {
    // Keep the MQTT connection serviced on every loop iteration so inbound
    // command-topic messages are processed promptly.
    unsigned long now = millis();
    bool connected = mqtt_.connected();
    if (connected) {
        mqtt_.loop();
        // loop() can drop the link itself on a keepalive ping timeout
        // (state -4, MQTT_CONNECTION_TIMEOUT) — the broker never sends a close.
        // Detect that in-pass transition here, otherwise the reason is lost
        // (wasConnected_ would already be false by the next iteration).
        if (!mqtt_.connected()) {
            Serial.printf("MQTT connection lost in loop (state=%d)\n",
                          mqtt_.state());
        }
    } else {
        // Log the reason exactly once per drop so we can tell a broker-initiated
        // close (state -3, MQTT_CONNECTION_LOST) from a client ping timeout
        // (state -4, MQTT_CONNECTION_TIMEOUT) rather than guessing.
        if (wasConnected_) {
            Serial.printf("MQTT connection lost (state=%d)\n", mqtt_.state());
        }
        if (now - lastReconnectMs_ >= MqttReconnectIntervalMs) {
            // Throttled, non-blocking reconnect. Runs independently of the
            // per-minute tick so command/control survives even in recovery mode.
            lastReconnectMs_ = now;
            mqttReconnect();
        }
    }
    wasConnected_ = mqtt_.connected();

    // Periodic presence beacon (IP + command topic), like the Waterbutt node.
    if (mqtt_.connected() && now - lastBeaconMs_ >= MqttBeaconIntervalMs) {
        lastBeaconMs_ = now;
        publishBeacon();
    }

    // Recovery mode: MQTT + OTA only — suspend all sensor/reporting work.
    if (recoveryMode_) {
        return;
    }

    // --- data publish (retained, every MqttPublishIntervalMs) ---
    if (now - lastPublishMs_ >= MqttPublishIntervalMs) {
        lastPublishMs_ += MqttPublishIntervalMs; // catches up if delayed
        prepData();
        if (mqtt_.connected()) {
            publishMqtt();
        } else {
            Serial.println("MQTT client is not connected");
        }
    }

    // The WOW upload + rain reset stay on the once-per-minute wall clock; the
    // data publish above runs on its own faster cadence.
    if (now - lastMinuteMs_ < 60000UL) {
        return;
    }
    lastMinuteMs_ += 60000UL; // advance one minute (catches up if delayed)

    // --- wall-clock-gated work (needs a valid NTP time) ---
    time_t tnow = time(nullptr);
    if (tnow < kClockValidEpoch) {
        return;
    }
    struct tm lt;
    localtime_r(&tnow, &lt);

    // Reset the daily rain total once per day at/after 09:00 local time (WOW
    // MetOffice 9am-9am convention). Edge-triggered on day-of-year rather than
    // matching the exact 09:00 minute: the per-minute gate above rides on
    // millis() (not the wall clock), so it drifts and can skip the single
    // tm_min==0 window entirely — which would silently miss the reset for the
    // whole day.
    if (lt.tm_hour >= 9) {
        if (!rainResetInit_) {
            // First valid-clock pass after boot and it is already past 09:00:
            // today's reset happened before we powered up. Adopt the persisted
            // day total instead of wiping it.
            rainResetInit_ = true;
            lastRainResetYday_ = lt.tm_yday;
        } else if (lastRainResetYday_ != lt.tm_yday) {
            Serial.println("Resetting daily rain accumulation");
            if (rain_) {
                rain_->resetDayAccumulation();
            }
            v_.rainDayIn = 0.0;
            lastRainResetYday_ = lt.tm_yday;
            savePersisted();
        }
    } else {
        // Before 09:00: mark init done so the first >=09:00 pass on the new day
        // triggers a real reset (last reset belongs to the previous day).
        rainResetInit_ = true;
    }

    // Every ReportFreqMin minutes: WOW upload + persist.
    if (lt.tm_min % ReportFreqMin == 0) {
        sendToWow(); // resets v_.rainMM / v_.rainIn on a successful upload
        savePersisted();
    }
}

// Force an immediate sensor refresh + MQTT publish, plus a WOW upload when the
// clock is valid. Triggered by the "report" command.
void Reporting::forceReport() {
    prepData();
    if (mqtt_.connected()) {
        publishMqtt();
    }
    if (time(nullptr) >= kClockValidEpoch) {
        sendToWow();
        savePersisted();
    }
}

// PubSubClient delivers inbound messages here (registered in begin()).
void Reporting::onMqttMessage(char *topic, uint8_t *payload,
                              unsigned int length) {
    if (strcmp(topic, MqttCommandTopic) != 0) {
        return; // only the command topic carries control messages
    }
    String message;
    message.reserve(length);
    for (unsigned int i = 0; i < length; i++) {
        message += (char)payload[i];
    }
    message.trim();
    Serial.printf("MQTT command on %s: %s\n", topic, message.c_str());
    handleCommand(message);
}

// Handle a single command string. Commands are case-insensitive. Replies are
// published to the status topic. Anything relevant to a weather station lives
// here (mirrors the Waterbutt command set, minus the pump-specific verbs).
void Reporting::handleCommand(String cmd) {
    cmd.toLowerCase();

    if (cmd == "status") {
        prepData();
        if (mqtt_.connected()) {
            publishMqtt();
        }
    } else if (cmd == "report" || cmd == "report-now") {
        mqtt_.publish(MqttStatusTopic, "reporting now");
        forceReport();
    } else if (cmd == "ip" || cmd == "address") {
        mqtt_.publish(MqttStatusTopic, net::ip().toString().c_str());
    } else if (cmd == "version") {
        mqtt_.publish(MqttStatusTopic, _SEMVER_CORE);
    } else if (cmd == "reset-rain" || cmd == "resetrain") {
        if (rain_) {
            rain_->resetDayAccumulation();
        }
        v_.rainDayIn = 0.0;
        savePersisted();
        mqtt_.publish(MqttStatusTopic, "daily rain total reset");
        Serial.println("Command: daily rain total reset");
    } else if (cmd == "recover" || cmd == "recovery") {
        recoveryMode_ = true;
        mqtt_.publish(MqttStatusTopic,
                      "recovery mode: MQTT+OTA only, send 'resume' or 'reset'");
        Serial.println("Command: recovery mode activated");
    } else if (cmd == "resume" || cmd == "normal") {
        recoveryMode_ = false;
        mqtt_.publish(MqttStatusTopic, "resumed normal operation");
        Serial.println("Command: resumed normal operation");
    } else if (cmd == "reset" || cmd == "reboot" || cmd == "restart") {
        mqtt_.publish(MqttStatusTopic, "rebooting");
        Serial.println("Command: rebooting");
        delay(200); // let the publish flush
        ESP.restart();
    } else {
        Serial.printf("Command: unknown [%s]\n", cmd.c_str());
        String reply = "unknown command: " + cmd;
        mqtt_.publish(MqttStatusTopic, reply.c_str());
    }
}

// Publish a health + readings snapshot to the status topic.
void Reporting::publishStatus() {
    String ip = net::ip().toString();

    char payload[640];
    snprintf(
        payload, sizeof(payload),
        "{\"id\":\"%s\",\"ip\":\"%s\",\"rssi\":%d,\"uptime_s\":%lu,"
        "\"heap\":%u,\"version\":\"%s\",\"recovery\":%s,"
        "\"atmosphere\":%s,\"rain\":%s,\"wind\":%s,\"river\":%s,"
        "\"temp\":%.2f,\"humidity\":%.2f,\"pressure\":%.2f,"
        "\"windspeed\":%.2f,\"windgust\":%.2f,\"winddir\":%.2f,"
        "\"rain_mm_hr\":%.2f,\"river_level\":%.3f,\"rain_day\":%.3f}",
        MqttStationId, ip.c_str(), net::rssi(), millis() / 1000UL,
        (unsigned)ESP.getFreeHeap(), _SEMVER_CORE,
        recoveryMode_ ? "true" : "false",
        (atm_ && atm_->isOnline()) ? "true" : "false",
        (rain_ && rain_->isOnline()) ? "true" : "false",
        (wind_ && wind_->isOnline()) ? "true" : "false",
        (river_ && river_->lastScrapeSuccess()) ? "true" : "false", v_.tempC,
        v_.humidity, v_.pressureHpa, v_.windSpeedMph, v_.windGustMph, v_.windDir,
        rain_ ? rain_->getRate() : 0.0f,
        river_ ? river_->level() : 0.0f,
        rain_ ? rain_->getDayAccumulation() : 0.0f);

    if (mqtt_.publish(MqttStatusTopic, payload)) {
        Serial.printf("MQTT status published to %s\n", MqttStatusTopic);
    } else {
        Serial.println("MQTT status publish failed");
    }
}

// Broadcast presence so apps can discover the station and its command topic.
void Reporting::publishBeacon() {
    String ip = net::ip().toString();

    char payload[160];
    snprintf(payload, sizeof(payload),
             "{\"id\":\"%s\",\"ip_address\":\"%s\",\"command_topic\":\"%s\"}",
             MqttStationId, ip.c_str(), MqttCommandTopic);
    mqtt_.publish(MqttBeaconTopic, payload);
}
