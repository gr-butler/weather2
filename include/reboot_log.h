#pragma once

#include <Arduino.h>

// Persistent reboot history. Each boot is classified into a short numeric code
// and appended to a last-5 ring buffer in NVS (so it survives power loss); the
// codes are surfaced in the MQTT status payload as "reboots" (most-recent
// first, e.g. "04,01,02,01,03"). Deliberate restarts (OTA, self-heal, the MQTT
// reset command) record their intent via setPending() just before rebooting so
// the otherwise-ambiguous ESP_RST_SW cause can be resolved on the next boot.
namespace rebootlog {

// 2-digit reboot reason codes (stable — do not renumber; they are published).
enum class Reason : uint8_t {
    Unknown         = 0, // 00 — could not be determined
    PowerCycle      = 1, // 01 — power-on (mains/USB power applied)
    FirmwareUpgrade = 2, // 02 — OTA firmware update
    Crash           = 3, // 03 — panic / exception
    SelfHeal        = 4, // 04 — network self-heal watchdog
    UserCommand     = 5, // 05 — MQTT reset/reboot command
    WatchdogHang    = 6, // 06 — task / interrupt watchdog timeout
    Brownout        = 7, // 07 — supply brownout
    ExternalReset   = 8, // 08 — external reset pin / other
    SoftwareReboot  = 9, // 09 — software reboot with no recorded cause
};

// Call once, early in setup(): classify THIS boot, append it to the persisted
// last-5 history, log it, and clear the pending marker.
void begin();

// Record the intended reason immediately before a deliberate ESP.restart().
// `detail` is an optional short human string kept only for the next boot log.
void setPending(Reason reason, const char *detail = "");

// Wipe the persisted history: the last-5 ring buffer, the stored crash summary,
// and any core dump a pre-update crash left in flash. Call this just before an
// OTA reboot so the post-update boot starts from a clean slate ("02" then any
// genuinely new crashes), instead of carrying old crashes forward.
void clearHistory();

// Comma-separated 2-digit codes, most-recent first, for the MQTT status payload.
String historyCodes();

// Decoded summary of the most recent crash (panic), extracted from the ESP32
// core dump written to flash: crashing task, program counter, exception
// cause/faulting address and a short backtrace of PCs. Empty until a crash has
// been captured. Decode the PCs with:
//   xtensa-esp32-elf-addr2line -pfiaC -e .pio/build/<env>/firmware.elf <pc...>
// Surfaced in the boot log and the MQTT status payload so a headless remote
// crash can be diagnosed without a serial console.
String crashDetail();

// Walks every heap region for corruption right now (cheap — no allocation).
// Logs and returns false if any is found. The recurring panic_abort() crash
// (see repo notes) unwinds with no useful backtrace, so this is called
// periodically and after heap-heavy handlers to narrow down WHERE the
// corruption happens, without needing a full heap-poisoning sdkconfig rebuild.
bool checkHeapIntegrity(const char *context);

} // namespace rebootlog
