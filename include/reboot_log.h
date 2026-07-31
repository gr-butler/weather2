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

// Comma-separated 2-digit codes, most-recent first, for the MQTT status payload.
String historyCodes();

} // namespace rebootlog
