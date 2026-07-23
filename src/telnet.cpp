#include "telnet.h"

#include <time.h>

#include "constants.h"
#include "net.h"
#include "version.h"
#include "weblog.h"
#define Serial Log // capture Serial output for the /logs web view

// =============================================================================
// TelnetServer — non-blocking, single-client TCP console.
//
// The server is polled from loop() via service(). Each call does a bounded
// amount of work: check for a new connection, drain available bytes into a
// line buffer, and dispatch a completed line. This never blocks loop() for
// more than a few microseconds.
// =============================================================================

TelnetServer::TelnetServer(Atmosphere *atm, Rainmeter *rain, Anemometer *wind,
                           RiverMonitor *river, Reporting *reporting)
    : server_(TelnetPort),
      atm_(atm),
      rain_(rain),
      wind_(wind),
      river_(river),
      reporting_(reporting) {}

void TelnetServer::begin() {
    server_.begin();
    server_.setNoDelay(true);
    Serial.printf("Telnet console on port %u\n", TelnetPort);
}

void TelnetServer::service() {
    if (!net::isUp()) {
        return;
    }
    acceptClient();
    if (client_ && client_.connected()) {
        readClient();
    }
}

// ---------------------------------------------------------------------------
// Private helpers
// ---------------------------------------------------------------------------

void TelnetServer::acceptClient() {
    if (server_.hasClient()) {
        WiFiClient incoming = server_.accept();
        if (!incoming) {
            return;
        }
        // Kick an existing connected client before replacing it.
        if (client_ && client_.connected()) {
            client_.println("\r\nNew connection — disconnecting you.\r\n");
            client_.stop();
            Serial.println("Telnet: kicked previous client for new connection");
        }
        client_ = incoming;
        inputBuf_ = "";
        Serial.printf("Telnet: client connected from %s\r\n",
                      client_.remoteIP().toString().c_str());

        // Banner — suppress IAC negotiation by sending plain ASCII only.
        client_.printf("\r\nWeather Station [%s]\r\n", _VERSION);
        client_.print("Type 'help' for commands.\r\n\r\n");
        prompt();
    }
}

void TelnetServer::readClient() {
    while (client_.available()) {
        char c = static_cast<char>(client_.read());

        // Swallow telnet IAC sequences (0xFF + 2 bytes) so terminal
        // option negotiation does not pollute the command buffer.
        if (static_cast<uint8_t>(c) == 0xFF) {
            // Consume the next two bytes of the IAC sequence if present.
            for (int i = 0; i < 2 && client_.available(); ++i) {
                client_.read();
            }
            continue;
        }

        // Discard bare CR (telnet line endings are CR LF; we act on LF).
        if (c == '\r') {
            continue;
        }

        if (c == '\n') {
            inputBuf_.trim();
            processLine(inputBuf_);
            inputBuf_ = "";
        } else if (c == 0x7F || c == 0x08) {
            // Backspace / DEL: remove last character.
            if (inputBuf_.length() > 0) {
                inputBuf_.remove(inputBuf_.length() - 1);
            }
        } else if (inputBuf_.length() < 128) {
            // Guard against runaway input.
            inputBuf_ += c;
        }
    }
}

void TelnetServer::processLine(const String &line) {
    if (line.length() == 0) {
        prompt();
        return;
    }

    String cmd = line;
    cmd.toLowerCase();

    if (cmd == "w") {
        cmdWeather();
    } else if (cmd == "status") {
        cmdStatus();
    } else if (cmd == "set") {
        cmdSet("");
    } else if (cmd.startsWith("set ")) {
        cmdSet(cmd.substring(4));
    } else if (cmd == "help" || cmd == "?") {
        cmdHelp();
    } else if (cmd == "quit" || cmd == "q" || cmd == "exit") {
        client_.print("Goodbye.\r\n");
        client_.stop();
        Serial.println("Telnet: client disconnected");
        return;
    } else {
        client_.printf("Unknown command: '%s'  (type 'help')\r\n", line.c_str());
    }

    prompt();
}

void TelnetServer::cmdWeather() {
    // Gather readings — same fields as the HTTP / handler.
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
    const char *windStr = wind_ ? wind_->getDirectionString() : "--";

    // Wall-clock timestamp (local time).
    char timeStr[32] = "--";
    time_t tnow = time(nullptr);
    if (tnow > 1700000000UL) {
        struct tm lt;
        localtime_r(&tnow, &lt);
        strftime(timeStr, sizeof(timeStr), "%d %b %y %H:%M:%S %Z", &lt);
    }

    client_.printf(
        "  Time        : %s\r\n"
        "  Temperature : %.2f °C\r\n"
        "  Humidity    : %.1f %%RH\r\n"
        "  Pressure    : %.2f hPa\r\n"
        "  Wind speed  : %.1f mph\r\n"
        "  Wind gust   : %.1f mph\r\n"
        "  Wind dir    : %.0f° (%s)\r\n"
        "  Rain/hr     : %.2f mm\r\n"
        "  Rain today  : %.2f mm\r\n",
        timeStr, tempC, humidity, pressure,
        windSpeed, windGust,
        windDir, windStr,
        rainHr, rainDay);

    if (river_ && river_->hasData()) {
        client_.printf("  River level : %.3f m\r\n", river_->level());
    }
}

void TelnetServer::cmdStatus() {
    unsigned long upSec = millis() / 1000UL;
    unsigned long upMin = upSec / 60;
    unsigned long upHr  = upMin / 60;
    upMin %= 60;
    upSec %= 60;

    client_.printf(
        "  Uptime      : %luh %02lum %02lus\r\n"
        "  Free heap   : %u bytes\r\n"
        "  Network     : %s  %s\r\n"
        "  IP          : %s\r\n"
        "  WiFi RSSI   : %d dBm\r\n"
        "  MQTT        : %s\r\n"
        "  Recovery    : %s\r\n"
        "  Wind online : %s\r\n"
        "  dir-avg     : %ds (default %ds)\r\n"
        "  app-window  : %ds (default %ds)\r\n",
        upHr, upMin, upSec,
        ESP.getFreeHeap(),
        net::isUp() ? "UP" : "DOWN", net::iface(),
        net::ip().toString().c_str(),
        net::rssi(),
        (reporting_ && reporting_->mqttConnected()) ? "connected" : "disconnected",
        (reporting_ && reporting_->isRecoveryMode()) ? "YES" : "no",
        (wind_ && wind_->isOnline()) ? "yes" : "NO",
        wind_ ? wind_->getDirAvgSeconds()     : -1, WindDirectionAverageSeconds,
        wind_ ? wind_->getAppSummarySeconds() : -1, AppWindSummarySeconds);
}

void TelnetServer::cmdSet(const String &args) {
    // No arguments: report current values.
    if (args.length() == 0) {
        client_.printf(
            "  dir-avg    : %ds (default %ds)\r\n"
            "  app-window : %ds (default %ds)\r\n",
            wind_ ? wind_->getDirAvgSeconds()     : -1,
            WindDirectionAverageSeconds,
            wind_ ? wind_->getAppSummarySeconds() : -1,
            AppWindSummarySeconds);
        return;
    }

    // Parse: <key> <value>
    int sp = args.indexOf(' ');
    if (sp < 0) {
        client_.printf("Usage: set <key> <value>\r\n"
                       "  Keys: dir-avg  app-window\r\n"
                       "  (no args: show current values)\r\n");
        return;
    }
    String key = args.substring(0, sp);
    String val = args.substring(sp + 1);
    val.trim();
    if (val.length() == 0) {
        client_.print("Missing value.\r\n");
        return;
    }

    int n = val.toInt();
    if (n <= 0 && val != "0") {
        client_.printf("Invalid value '%s' — must be a positive integer.\r\n",
                       val.c_str());
        return;
    }

    if (key == "dir-avg") {
        if (!wind_) {
            client_.print("Wind sensor not available.\r\n");
            return;
        }
        int prev = wind_->getDirAvgSeconds();
        wind_->setDirAvgSeconds(n);
        client_.printf("dir-avg: %ds -> %ds (saved to NVS).\r\n",
                       prev, wind_->getDirAvgSeconds());
    } else if (key == "app-window") {
        if (!wind_) {
            client_.print("Wind sensor not available.\r\n");
            return;
        }
        int prev = wind_->getAppSummarySeconds();
        wind_->setAppSummarySeconds(n);
        client_.printf("app-window: %ds -> %ds (saved to NVS).\r\n",
                       prev, wind_->getAppSummarySeconds());
    } else {
        client_.printf("Unknown key '%s'. Valid keys: dir-avg, app-window\r\n",
                       key.c_str());
    }
}

void TelnetServer::cmdHelp() {
    client_.print(
        "  w                       — current weather readings\r\n"
        "  status                  — system status (uptime, heap, network, MQTT)\r\n"
        "  set                     — show wind window config\r\n"
        "  set dir-avg <seconds>   — wind direction smoothing window [1-60]\r\n"
        "  set app-window <seconds>— app MQTT summary window [1-60]\r\n"
        "  help/?                  — this message\r\n"
        "  quit/q                  — disconnect\r\n");
}

void TelnetServer::prompt() {
    client_.print("> ");
}
