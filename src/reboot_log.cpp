#include "reboot_log.h"

#include <Preferences.h>
#include <esp_system.h>

#include "weblog.h"
#define Serial Log // capture Serial output for the /logs web view

// See reboot_log.h for the design. Boot classification combines the hardware
// reset cause (esp_reset_reason) with an RTC-backed "pending" marker that
// deliberate restarts set beforehand.

namespace {
// Pending-reason marker. RTC_NOINIT survives a clean software reboot (the only
// path that sets it) but holds garbage after power loss/brownout — where
// esp_reset_reason() is authoritative — so it is trusted only on ESP_RST_SW.
RTC_NOINIT_ATTR uint32_t g_pendingMagic;
RTC_NOINIT_ATTR uint8_t g_pendingReason;
RTC_NOINIT_ATTR char g_pendingDetail[48];
constexpr uint32_t kPendingMagic = 0x52424C31; // 'RBL1'

// last-5 history persisted in NVS (survives power loss), stored oldest-first.
constexpr const char *kNs = "rebootlog";
constexpr const char *kKeyHist = "hist";
constexpr size_t kMaxHistory = 5;

const char *reasonName(uint8_t code) {
    switch (code) {
    case 1:  return "power cycle";
    case 2:  return "firmware upgrade";
    case 3:  return "crash/exception";
    case 4:  return "self-heal";
    case 5:  return "user command";
    case 6:  return "watchdog hang";
    case 7:  return "brownout";
    case 8:  return "external reset";
    case 9:  return "software reboot";
    default: return "unknown";
    }
}

// Classify the current boot from the hardware reset cause plus the pending
// marker (only consulted on a clean software reset, where RTC memory is intact).
uint8_t classifyBoot() {
    switch (esp_reset_reason()) {
    case ESP_RST_POWERON:
        return 1;
    case ESP_RST_SW:
        if (g_pendingMagic == kPendingMagic && g_pendingReason != 0) {
            return g_pendingReason;
        }
        return 9;
    case ESP_RST_PANIC:
        return 3;
    case ESP_RST_INT_WDT:
    case ESP_RST_TASK_WDT:
    case ESP_RST_WDT:
        return 6;
    case ESP_RST_BROWNOUT:
        return 7;
    case ESP_RST_EXT:
    case ESP_RST_DEEPSLEEP:
    case ESP_RST_SDIO:
        return 8;
    default:
        return 0;
    }
}
} // namespace

namespace rebootlog {

void begin() {
    uint8_t code = classifyBoot();

    // Append this boot to the ring buffer, dropping the oldest once full.
    uint8_t hist[kMaxHistory] = {0};
    Preferences prefs;
    prefs.begin(kNs, /*readOnly=*/false);
    size_t n = prefs.getBytes(kKeyHist, hist, sizeof(hist));
    if (n > kMaxHistory) {
        n = 0; // corrupt / first run
    }
    uint8_t out[kMaxHistory];
    size_t m = 0;
    size_t start = (n == kMaxHistory) ? 1 : 0;
    for (size_t i = start; i < n; i++) {
        out[m++] = hist[i];
    }
    out[m++] = code;
    prefs.putBytes(kKeyHist, out, m);
    prefs.end();

    bool haveDetail =
        (g_pendingMagic == kPendingMagic) && g_pendingDetail[0] != '\0';
    if (haveDetail) {
        Serial.printf("Boot reason: code %02u (%s) — %s\n", code,
                      reasonName(code), g_pendingDetail);
    } else {
        Serial.printf("Boot reason: code %02u (%s)\n", code, reasonName(code));
    }

    // Consume the pending marker so the next boot classifies fresh.
    g_pendingMagic = 0;
    g_pendingReason = 0;
    g_pendingDetail[0] = '\0';
}

void setPending(Reason reason, const char *detail) {
    g_pendingReason = static_cast<uint8_t>(reason);
    strncpy(g_pendingDetail, detail ? detail : "", sizeof(g_pendingDetail) - 1);
    g_pendingDetail[sizeof(g_pendingDetail) - 1] = '\0';
    g_pendingMagic = kPendingMagic;
}

String historyCodes() {
    uint8_t hist[kMaxHistory] = {0};
    Preferences prefs;
    prefs.begin(kNs, /*readOnly=*/true);
    size_t n = prefs.getBytes(kKeyHist, hist, sizeof(hist));
    prefs.end();
    if (n > kMaxHistory) {
        n = 0;
    }

    String out;
    for (size_t i = 0; i < n; i++) {
        char buf[4];
        snprintf(buf, sizeof(buf), "%02u", hist[n - 1 - i]); // newest first
        if (out.length()) {
            out += ",";
        }
        out += buf;
    }
    return out;
}

} // namespace rebootlog
