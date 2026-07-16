#pragma once

#include <esp_system.h>

// Human-readable ESP32 boot / reset cause. Shared by the boot log (main.cpp)
// and the MQTT status payload (reporting.cpp) so the wording stays consistent.
// esp_reset_reason() is stable for the whole boot, so these may be called at any
// time, not just during setup().

inline const char *bootReasonString() {
    switch (esp_reset_reason()) {
    case ESP_RST_POWERON:   return "power-on";
    case ESP_RST_EXT:       return "external reset pin";
    case ESP_RST_SW:        return "software reboot";
    case ESP_RST_DEEPSLEEP: return "deep-sleep wake";
    case ESP_RST_PANIC:     return "crash (panic/exception)";
    case ESP_RST_INT_WDT:   return "interrupt watchdog";
    case ESP_RST_TASK_WDT:  return "task watchdog";
    case ESP_RST_WDT:       return "watchdog";
    case ESP_RST_BROWNOUT:  return "brownout";
    case ESP_RST_SDIO:      return "SDIO reset";
    default:                return "unknown";
    }
}

// True when the last boot was an abnormal restart (crash / brownout / watchdog)
// rather than a normal power-on or deliberate reboot.
inline bool bootReasonAbnormal() {
    switch (esp_reset_reason()) {
    case ESP_RST_PANIC:
    case ESP_RST_INT_WDT:
    case ESP_RST_TASK_WDT:
    case ESP_RST_WDT:
    case ESP_RST_BROWNOUT:
        return true;
    default:
        return false;
    }
}
