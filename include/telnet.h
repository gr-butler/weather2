#pragma once

#include <Arduino.h>
#include <WiFiClient.h>
#include <WiFiServer.h>

#include "anemometer.h"
#include "atmosphere.h"
#include "rainmeter.h"
#include "reporting.h"
#include "river.h"

// TelnetServer — a lightweight TCP console on port 23.
//
// Accepts a single client at a time. On connect the client gets a banner and
// a prompt. Lines typed are dispatched to a small command handler. The server
// is non-blocking: service() is polled from loop() and does only a small amount
// of work per call so it never stalls the main loop.
//
// Supported commands:
//   w          — print current weather readings
//   status     — system status (uptime, free heap, network, MQTT)
//   help / ?   — list commands
//   quit / q   — disconnect
class TelnetServer {
public:
    TelnetServer(Atmosphere *atm, Rainmeter *rain, Anemometer *wind,
                 RiverMonitor *river, Reporting *reporting);

    // Start listening on TelnetPort.
    void begin();

    // Call frequently from loop(): accepts new clients, reads input, dispatches
    // commands. Non-blocking.
    void service();

private:
    void acceptClient();
    void readClient();
    void processLine(const String &line);
    void cmdWeather();
    void cmdStatus();
    void cmdSet(const String &args);
    void cmdHelp();
    void prompt();

    WiFiServer server_;
    WiFiClient client_;
    String inputBuf_;

    Atmosphere *atm_;
    Rainmeter *rain_;
    Anemometer *wind_;
    RiverMonitor *river_;
    Reporting *reporting_;
};
