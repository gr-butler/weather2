#include "net.h"

#include <WiFi.h>
#ifdef USE_ETHERNET
#include <ETH.h>
#endif

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <lwip/ip_addr.h>
#include <ping/ping_sock.h>

#include "reboot_log.h"
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

// Active reachability watchdog. isUp() only proves the interface still HOLDS an
// IP; the lwIP stack / PHY path can wedge so the device is unreachable while
// loop() keeps running and neither the task watchdog nor the both-links-down
// check above ever fires. We ICMP-ping the default gateway and reboot if it
// stays unreachable for kReachMaxFailReboots consecutive checks.
constexpr unsigned long kReachCheckIntervalMs = 30000; // ping gateway every 30 s
constexpr uint8_t kReachPingCount = 2;                 // echo requests per check
constexpr uint32_t kReachPingTimeoutMs = 1000;         // per-request timeout
constexpr int kReachMaxFailReboots = 10;               // ~5 min unreachable -> reboot

constexpr const char *kHostname = "weather-station";

// Record the reason (surfaced in the reboot history), then restart. Used by both
// the both-links-down watchdog and the gateway-reachability watchdog.
void triggerSelfHealReboot(const char *reason) {
    rebootlog::setPending(rebootlog::Reason::SelfHeal, reason);
    Serial.printf("SELF-HEAL: %s -- rebooting\n", reason);
    delay(100);
    ESP.restart();
}

// Blocking ICMP echo (ping) of `target`. Returns true if at least one reply
// arrives within the per-request timeout. With the defaults used here it runs in
// well under 3 s, so it stays comfortably inside the 30 s task watchdog window.
bool pingHost(const IPAddress &target, uint8_t count, uint32_t timeoutMs) {
    if (static_cast<uint32_t>(target) == 0) {
        return false;
    }

    ip_addr_t addr;
    memset(&addr, 0, sizeof(addr));
    addr.type = IPADDR_TYPE_V4;
    addr.u_addr.ip4.addr = static_cast<uint32_t>(target);

    SemaphoreHandle_t done = xSemaphoreCreateBinary();
    if (done == nullptr) {
        return false;
    }
    volatile uint32_t received = 0;

    struct Ctx {
        SemaphoreHandle_t done;
        volatile uint32_t *received;
    } ctx{done, &received};

    esp_ping_config_t cfg = ESP_PING_DEFAULT_CONFIG();
    cfg.target_addr = addr;
    cfg.count = count;
    cfg.timeout_ms = timeoutMs;
    cfg.interval_ms = 100;

    esp_ping_callbacks_t cbs = {};
    cbs.cb_args = &ctx;
    cbs.on_ping_success = [](esp_ping_handle_t, void *args) {
        auto *c = static_cast<Ctx *>(args);
        (*c->received)++;
    };
    cbs.on_ping_timeout = nullptr;
    cbs.on_ping_end = [](esp_ping_handle_t, void *args) {
        auto *c = static_cast<Ctx *>(args);
        xSemaphoreGive(c->done);
    };

    esp_ping_handle_t hdl = nullptr;
    if (esp_ping_new_session(&cfg, &cbs, &hdl) != ESP_OK) {
        vSemaphoreDelete(done);
        return false;
    }
    esp_ping_start(hdl);

    const uint32_t waitMs =
        static_cast<uint32_t>(count) * (timeoutMs + cfg.interval_ms) + 500;
    const bool finished = xSemaphoreTake(done, pdMS_TO_TICKS(waitMs)) == pdTRUE;

    esp_ping_stop(hdl);
    esp_ping_delete_session(hdl);
    vSemaphoreDelete(done);

    return finished && received > 0;
}

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
#ifdef ETH_STATIC_IP
    // Static addressing (production): pin the station to a fixed IP so the
    // Prometheus scrape target and MQTT clients always find it. Applied right
    // after ETH.begin(); if any address is malformed or config fails we fall
    // back to DHCP rather than ending up unreachable.
    {
        IPAddress ip, gw, mask, dns1, dns2;
        if (ip.fromString(ETH_STATIC_IP) && gw.fromString(ETH_GATEWAY) &&
            mask.fromString(ETH_SUBNET) && dns1.fromString(ETH_DNS1) &&
            dns2.fromString(ETH_DNS2) &&
            ETH.config(ip, gw, mask, dns1, dns2)) {
            Serial.printf("ETH static IP: %s mask %s gw %s dns %s/%s\n",
                          ETH_STATIC_IP, ETH_SUBNET, ETH_GATEWAY, ETH_DNS1,
                          ETH_DNS2);
        } else {
            Serial.println("ETH static config FAILED — falling back to DHCP");
        }
    }
#endif
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
    static unsigned long lastReachCheck = 0;
    static int downCount = 0;
    static int reachFailCount = 0;
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
            triggerSelfHealReboot("no ETH or WiFi link");
        } else {
            Serial.printf("Network fully down (%d/%d)\n", downCount,
                          kNetMaxDownReboots);
        }
    }

    // Reachability watchdog: even with an IP, ping the gateway to prove the
    // stack can actually pass traffic. Pinging the GATEWAY (local, always-on)
    // means an internet/ISP outage — which a reboot cannot fix — never triggers
    // a needless restart; only a genuinely wedged local stack does.
    if (now - lastReachCheck >= kReachCheckIntervalMs) {
        lastReachCheck = now;
        if (!isUp()) {
            reachFailCount = 0; // the both-links-down watchdog owns this case
        } else {
            IPAddress gw = gatewayIP();
            if (static_cast<uint32_t>(gw) == 0) {
                reachFailCount = 0; // no gateway learned yet — nothing to test
            } else if (pingHost(gw, kReachPingCount, kReachPingTimeoutMs)) {
                reachFailCount = 0;
            } else if (++reachFailCount >= kReachMaxFailReboots) {
                triggerSelfHealReboot("gateway unreachable (network stack wedged)");
            } else {
                Serial.printf("Gateway %s unreachable (%d/%d)\n",
                              gw.toString().c_str(), reachFailCount,
                              kReachMaxFailReboots);
            }
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

IPAddress gatewayIP() {
#ifdef USE_ETHERNET
    if (ethUp()) {
        return ETH.gatewayIP();
    }
#endif
    if (wifiUp()) {
        return WiFi.gatewayIP();
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
