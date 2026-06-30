#pragma once

#include <Arduino.h>
#include <IPAddress.h>

// Network abstraction. Wired Ethernet (Olimex ESP32-POE, LAN8720 RMII) is the
// PRIMARY link; WiFi is an automatic fallback and the convenient path for
// initial setup / bench testing. Both interfaces are lwIP netifs, so every
// higher layer (MQTT/TLS, HTTP, the WOW upload) is link-agnostic and talks to
// these helpers instead of WiFi/ETH directly.
namespace net {

// Start Ethernet + WiFi and register the link event handler. Blocks briefly
// (a few seconds) waiting for either interface to obtain an IP.
void begin();

// Call frequently from loop(): nudges WiFi reconnection and, as a last resort,
// reboots the device only if BOTH links stay down for too long.
void service();

bool isUp();         // true if either Ethernet or WiFi has an IP
bool ethUp();        // true if Ethernet has an IP
bool wifiUp();       // true if WiFi STA is connected
IPAddress ip();      // active IP — Ethernet preferred, then WiFi, else 0.0.0.0
int rssi();          // WiFi signal strength (dBm); 0 when WiFi is not connected
const char *iface(); // "eth", "wifi" or "none"

} // namespace net
