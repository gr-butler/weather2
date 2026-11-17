#pragma once

#include <stddef.h>

// Single-page web UI (Live / Logs / Settings tabs) served at GET /.
//
// Self-contained: inline CSS + JS, no external resources, so it works on an
// isolated LAN. The page lives in flash and is streamed from there by
// WeatherWebServer (never copied to the heap). It talks to the device only
// through /data.json, /logs.json and /system.json.
extern const char kWebUiHtml[];
extern const size_t kWebUiHtmlLen;
