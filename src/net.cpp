#include "net.h"

#include <WiFi.h>
#ifdef USE_ETHERNET
#include <ETH.h>
#endif

#include "secrets.h"
#include "weblog.h"
#define Serial Log // capture Serial output for the /logs web view

// =============================================================================
//  Ethernet-primary / WiFi-fallback networking for the Olimex ESP32-POE.
//
//  The board uses a LAN8720 PHY over RMII. The PHY power pin, MDC and MDIO are
//  supplied as build flags (ETH_POWER_PIN / ETH_MDC_PIN / ETH_MDIO_PIN) so the
//  pin map lives in platformio.ini. The 50 MHz PHY reference clock is driven
//  OUT of GPIO17 — this is the documented wiring for both the ESP32-POE and the
//  ESP32-POE-ISO, so the same firmware runs on either variant.
//
//  When USE_ETHERNET is NOT defined (env:esp32dev, a bog-standard ESP32 dev
//  board with no PHY) this file compiles to a WiFi-only build: ethUp() is
//  always false and WiFi carries everything.
// =============================================================================

namespace {
#ifdef USE_ETHERNET
constexpr eth_phy_type_t kEthType = ETH_PHY_LAN8720;
constexpr eth_clock_mode_t kEthClk = ETH_CLOCK_GPIO17_OUT;

// Updated from the link event handler (ISR-context-safe primitives only).
volatile bool g_ethHasIp = false;
volatile bool g_ethLinkUp = false;
#endif

// WiFi fallback + full-network watchdog timing.
constexpr unsigned long kWifiRetryIntervalMs = 10000; // nudge WiFi every 10 s
constexpr unsigned long kNetCheckIntervalMs = 10000;  // re-check "all down" 10 s
constexpr int kNetMaxDownReboots = 12;                // ~2 min down -> reboot

constexpr const char *kHostname = "weather-station";

// Unified handler for both ETH and WiFi events (the Arduino core funnels both
// through WiFi.onEvent). Registered before either interface is started so we
// never miss a GOT_IP.
void onNetEvent(arduino_event_id_t event) {
    switch (event) {
#ifdef USE_ETHERNET
    case ARDUINO_EVENT_ETH_START:
        ETH.setHostname(kHostname);
        Serial.println("ETH started");
        break;
    case ARDUINO_EVENT_ETH_CONNECTED:
        g_ethLinkUp = true;
        Serial.println("ETH link up");
        break;
    case ARDUINO_EVENT_ETH_GOT_IP:
        g_ethHasIp = true;
        Serial.printf("ETH got IP %s (%d Mbps, %s duplex)\n",
                      ETH.localIP().toString().c_str(), ETH.linkSpeed(),
                      ETH.fullDuplex() ? "full" : "half");
        break;
    case ARDUINO_EVENT_ETH_DISCONNECTED:
        g_ethLinkUp = false;
        g_ethHasIp = false;
        Serial.println("ETH link down");
        break;
    case ARDUINO_EVENT_ETH_STOP:
        g_ethLinkUp = false;
        g_ethHasIp = false;
        Serial.println("ETH stopped");
        break;
#endif
    case ARDUINO_EVENT_WIFI_STA_GOT_IP:
        Serial.printf("WiFi got IP %s\n", WiFi.localIP().toString().c_str());
        break;
    default:
        break;
    }
}
} // namespace

namespace net {

void begin() {
    // Register the event handler before starting either interface.
    WiFi.onEvent(onNetEvent);

    // WiFi standby/fallback (also the convenient link for setup + bench test).
    WiFi.persistent(true);
    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(true);
    WiFi.setHostname(kHostname);
    if (strlen(WIFI_SSID) > 0) {
        WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    }

#ifdef USE_ETHERNET
    // Wired Ethernet — the primary link (Olimex ESP32-POE LAN8720).
    Serial.println("Starting Ethernet (Olimex ESP32-POE LAN8720)...");
    ETH.begin(ETH_PHY_ADDR, ETH_POWER_PIN, ETH_MDC_PIN, ETH_MDIO_PIN, kEthType,
              kEthClk);
#else
    Serial.println("Ethernet disabled (WiFi-only build)");
#endif

    // Wait up to ~15 s for either link to obtain an IP so NTP + the first MQTT
    // connect have connectivity. Ethernet is preferred but WiFi is accepted.
    Serial.print("Waiting for network");
    for (int i = 0; i < 30 && !isUp(); i++) {
        delay(500);
        Serial.print('.');
    }
    if (isUp()) {
        Serial.printf("\nNetwork up via %s, IP: %s\n", iface(),
                      ip().toString().c_str());
    } else {
        Serial.println("\nNetwork NOT up — continuing offline");
    }
}

void service() {
    static unsigned long lastWifiRetry = 0;
    static unsigned long lastDownCheck = 0;
    static int downCount = 0;
    unsigned long now = millis();

    // Nudge WiFi back up if it has dropped. setAutoReconnect(true) handles
    // ordinary drops on its own; we only intervene when the supplicant looks
    // stalled. Reset the timer while the link is healthy so a freshly-
    // associating connection is never kicked, and use WiFi.reconnect() rather
    // than disconnect()+begin() — the latter tears down a connection that is
    // mid-recovery and races auto-reconnect, producing "begin(): connect
    // failed!" churn that drops the TLS/MQTT socket every interval.
    if (strlen(WIFI_SSID) > 0) {
        if (WiFi.status() == WL_CONNECTED) {
            lastWifiRetry = now;
        } else if (now - lastWifiRetry >= kWifiRetryIntervalMs) {
            lastWifiRetry = now;
            WiFi.reconnect();
        }
    }

    // Full-network watchdog: reboot ONLY if both Ethernet and WiFi are down for
    // ~2 min. A healthy wired link must never trigger a reboot just because
    // WiFi is absent (the normal steady state for this station).
    if (now - lastDownCheck >= kNetCheckIntervalMs) {
        lastDownCheck = now;
        if (isUp()) {
            downCount = 0;
        } else if (++downCount >= kNetMaxDownReboots) {
            Serial.println("Network down too long (no ETH or WiFi) — rebooting");
            delay(100);
            ESP.restart();
        } else {
            Serial.printf("Network fully down (%d/%d)\n", downCount,
                          kNetMaxDownReboots);
        }
    }
}

bool ethUp() {
#ifdef USE_ETHERNET
    return g_ethHasIp;
#else
    return false;
#endif
}
bool wifiUp() { return WiFi.status() == WL_CONNECTED; }
bool isUp() { return ethUp() || wifiUp(); }

IPAddress ip() {
#ifdef USE_ETHERNET
    if (ethUp()) {
        return ETH.localIP();
    }
#endif
    if (wifiUp()) {
        return WiFi.localIP();
    }
    return IPAddress((uint32_t)0);
}

int rssi() { return wifiUp() ? WiFi.RSSI() : 0; }

const char *iface() {
    if (ethUp()) {
        return "eth";
    }
    if (wifiUp()) {
        return "wifi";
    }
    return "none";
}

} // namespace net
