#pragma once

// =============================================================================
//  SECRETS — committed fallback shim (placeholders only, no real credentials).
//
//  REAL credentials live in the gitignored `private.ini` ([secrets] section),
//  which platformio.ini injects as -D build flags. Those flags OVERRIDE the
//  #ifndef placeholders below. These defaults only keep a build working if a
//  flag is missing — never put real secrets in this file.
// =============================================================================

// --- WiFi ---
#ifndef WIFI_SSID
#define WIFI_SSID "your-wifi-ssid"
#endif
#ifndef WIFI_PASSWORD
#define WIFI_PASSWORD "your-wifi-password"
#endif

// --- MQTT broker (TLS). EMQX Cloud uses port 8883 + username/password. ---
#ifndef MQTT_BROKER_HOST
#define MQTT_BROKER_HOST "server.internal"
#endif
#ifndef MQTT_BROKER_PORT
#define MQTT_BROKER_PORT 8883
#endif
#ifndef MQTT_USERNAME
#define MQTT_USERNAME ""
#endif
#ifndef MQTT_PASSWORD
#define MQTT_PASSWORD ""
#endif

// --- WOW MetOffice. Leave WOW_SITE_ID blank to disable uploads. ---
#ifndef WOW_SITE_ID
#define WOW_SITE_ID ""
#endif
#ifndef WOW_PIN
#define WOW_PIN ""
#endif
