#include "reboot_log.h"

#include <Preferences.h>
#include <esp_core_dump.h>
#include <esp_heap_caps.h>
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
// Decoded summary of the most recent crash (task/PC/cause/backtrace).
constexpr const char *kKeyCrash = "crash";

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

// Decode the core dump that the ESP-IDF panic handler wrote to the `coredump`
// flash partition (enabled by default in arduino-esp32, ELF format), persist a
// compact one-line summary to NVS, then erase the image so the next boot starts
// clean. Runs on every boot: a stored dump is only present after a panic. The
// PCs correspond to the firmware that crashed — decode them against that build's
// firmware.elf. (The first dump seen right after an OTA may belong to the old
// build; subsequent crashes on the running build decode exactly.)
void captureCrashSummary() {
#if CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH && CONFIG_ESP_COREDUMP_DATA_FORMAT_ELF
    if (esp_core_dump_image_check() != ESP_OK) {
        return; // no valid core dump stored
    }
    auto *sum = static_cast<esp_core_dump_summary_t *>(
        malloc(sizeof(esp_core_dump_summary_t)));
    if (sum != nullptr && esp_core_dump_get_summary(sum) == ESP_OK) {
        char bt[160];
        size_t off = 0;
        uint32_t depth = sum->exc_bt_info.depth;
        if (depth > 8) {
            depth = 8; // keep the summary compact enough for the status payload
        }
        for (uint32_t i = 0; i < depth && off < sizeof(bt) - 12; i++) {
            off += snprintf(bt + off, sizeof(bt) - off, "%s0x%08x",
                            i ? " " : "", (unsigned)sum->exc_bt_info.bt[i]);
        }
        char detail[256];
        snprintf(detail, sizeof(detail),
                 "task=%.15s pc=0x%08x cause=%u vaddr=0x%08x bt=%s",
                 sum->exc_task, (unsigned)sum->exc_pc,
                 (unsigned)sum->ex_info.exc_cause,
                 (unsigned)sum->ex_info.exc_vaddr, bt);
        Preferences prefs;
        prefs.begin(kNs, /*readOnly=*/false);
        prefs.putString(kKeyCrash, detail);
        prefs.end();
        Serial.printf("Crash core-dump: %s\n", detail);
    }
    free(sum);
    esp_core_dump_image_erase();
#endif
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

    // Decode + persist any core dump a panic left behind, then clear it.
    captureCrashSummary();
}

void setPending(Reason reason, const char *detail) {
    g_pendingReason = static_cast<uint8_t>(reason);
    strncpy(g_pendingDetail, detail ? detail : "", sizeof(g_pendingDetail) - 1);
    g_pendingDetail[sizeof(g_pendingDetail) - 1] = '\0';
    g_pendingMagic = kPendingMagic;
}

void clearHistory() {
    Preferences prefs;
    prefs.begin(kNs, /*readOnly=*/false);
    prefs.remove(kKeyHist);
    prefs.remove(kKeyCrash);
    prefs.end();
    // Drop any core dump a pre-update crash left behind so the next boot does
    // not re-capture it and report a stale crash as if it were new.
#if CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH && CONFIG_ESP_COREDUMP_DATA_FORMAT_ELF
    if (esp_core_dump_image_check() == ESP_OK) {
        esp_core_dump_image_erase();
    }
#endif
    Serial.println("Reboot history + crash summary cleared");
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

String crashDetail() {
    Preferences prefs;
    prefs.begin(kNs, /*readOnly=*/true);
    String detail = prefs.getString(kKeyCrash, "");
    prefs.end();
    return detail;
}

bool checkHeapIntegrity(const char *context) {
    bool ok = heap_caps_check_integrity_all(/*print_errors=*/true);
    if (!ok) {
        Serial.printf("*** HEAP CORRUPTION DETECTED (context=%s) ***\n", context);
    }
    return ok;
}

} // namespace rebootlog
