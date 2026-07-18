# weather2 — ESP32 Weather Station

ESP32 (C++/PlatformIO/Arduino) port of the Go `weather/` Raspberry Pi Zero
weather station. It reads atmospheric, rain and wind sensors, publishes readings
over MQTT (TLS), uploads to the WOW MetOffice API, exposes an HTTP dashboard +
Prometheus metrics, and supports OTA firmware updates.

The Go project (`../weather/`) is the **reference implementation** — sensor
behaviour, calculations and data output match it exactly. See [AGENTS.md](AGENTS.md)
for the governing rules.

## Build

```bash
# Use the explicit PlatformIO path (the bash `pio` on PATH may be an older core)
C:/Users/butlerg/AppData/Roaming/Python/Python313/Scripts/platformio.exe run            # build (default: esp32 / Olimex POE)
C:/Users/butlerg/AppData/Roaming/Python/Python313/Scripts/platformio.exe run -t upload  # flash
```

Two firmware environments are defined in `platformio.ini`:

| Env        | Board                  | Networking            | Use                                         |
|------------|------------------------|-----------------------|---------------------------------------------|
| `esp32`    | Olimex ESP32-POE-ISO   | Ethernet + WiFi fallback | Production (default)                      |
| `esp32dev` | Any standard ESP32 dev | WiFi only             | Bench testing without the PoE/Ethernet board |

Both share all sensor/CAN/LED pins, so wiring carries over. Build/flash the
bench target with `platformio.exe run -e esp32dev -t upload`. The `esp32dev`
build omits the `USE_ETHERNET` flag, so [src/net.cpp](src/net.cpp) compiles
WiFi-only — everything except the wired link and PoE is exercised.

Secrets (WiFi, MQTT, WOW) live in the gitignored `private.ini` `[secrets]`
section and are injected as build flags by `platformio.ini`. Leave `wow_site_id`
blank to disable WOW uploads.

## Hardware

Target board: **Olimex ESP32-POE-ISO** (ESP32 + LAN8720 RMII Ethernet + isolated
802.3af PoE). The same firmware also runs on the non-isolated **ESP32-POE** —
change only `board` in `platformio.ini`.

| Function            | Interface | GPIO        | Notes                                              |
|---------------------|-----------|-------------|----------------------------------------------------|
| Temperature         | I²C 0x18  | SDA 13, SCL 16 | MCP9808 (primary)                              |
| Pressure + humidity | I²C 0x76  | SDA 13, SCL 16 | BME280 (also temperature fallback)             |
| Rain bucket         | GPIO IRQ  | 14          | Falling edge, 500 ms debounce                      |
| Wind (speed/dir)    | CAN/TWAI  | TX 5, RX 4  | Remote masthead node — see [docs/MASTHEAD_CAN_SPEC.md](docs/MASTHEAD_CAN_SPEC.md) |
| Heartbeat LED       | GPIO      | 33          | Flashes every 60 s                                 |
| Rain-tip LED        | GPIO      | 32          | Flashes on each bucket tip                         |
| Ethernet (LAN8720)  | RMII      | power 12, MDC 23, MDIO 18, clock-out 17 (+ 0,19,21,22,25,26,27) | Pins fixed by the PoE board |

> RMII consumes GPIO 0, 17, 18, 19, 21, 22, 23, 25, 26, 27 (+ GPIO12 = PHY
> power), so the sensor/LED pins above are placed clear of them. Pin assignments
> are build flags in `platformio.ini`.

## Networking

Wired **Ethernet is the primary link**; **WiFi is an automatic fallback** (and
the convenient path for initial setup / bench testing). Both are lwIP netifs, so
MQTT/TLS, the HTTP server, OTA and the WOW upload run unchanged over whichever
link is active. The abstraction lives in [include/net.h](include/net.h) /
[src/net.cpp](src/net.cpp):

- At boot both interfaces start; the device waits briefly for either to get an IP.
- Ethernet is preferred for the active IP whenever its link is up.
- WiFi auto-reconnects in the background and is nudged if the supplicant stalls.
- A last-resort watchdog reboots the device only if **both** links stay down for
  ~2 minutes — a healthy wired link never triggers a reboot when WiFi is absent.

## MQTT

The broker connection uses **TLS** (port 8883) with username/password auth and a
CA certificate ([include/mqtt_ca_cert.h](include/mqtt_ca_cert.h)).

### Data topic (locked — do not rename)

| Topic               | Direction | Cadence       | Payload |
|---------------------|-----------|---------------|---------|
| `culverhay/weather` | publish   | every 30 s    | Core JSON (below) |
| `culverhay/weather/app` | publish | every 30 s | App JSON (same shape; wind is 30 s summary) |

```json
{
  "name": "weather_station",
  "ip_address": "0.0.0.0",
  "time": "HH:MM:SS DD/MM/YYYY",
  "rain": "0.00",
  "temp": "0.00",
  "windspeed": "0.00",
  "windgust": "0.00",
  "winddir": "0.00",
  "humidity": "0.00",
  "pressure": "0.00",
  "rain_mm_hr": "0.00",
  "rain_day": "0.00",
  "river_level": "0.000"
}
```

> All data-payload field names are consumed by downstream dashboards and **must
> not change** (AGENTS.md Rule 1). Values are 2-dp strings, matching the Go
> reference `dataMap`.
>
> Rain fields: `rain` is the rainfall (mm) accumulated **since the last WOW
> report** (cleared on each successful upload, ~every 15 min — mirrors the Go
> `RainMM` / WOW `rainin`). `rain_mm_hr` is the last-hour rate (mm/hr).
> `rain_day` is the running **9am–9am day total** (mm).

`culverhay/weather/app` keeps the same payload shape for app compatibility but
uses app-facing wind values:
- `windspeed`: mean over the last 30 s
- `winddir`: circular mean over the last 30 s
- `windgust`: peak 3-second average found within the last 30 s

This app-only topic does not change Prometheus metrics or WOW Met Office
reporting logic.

### Control plane

| Topic                       | Direction | Cadence    | Purpose |
|-----------------------------|-----------|------------|---------|
| `culverhay/weather/command` | subscribe | on demand  | Inbound commands (see below) |
| `culverhay/weather/status`  | publish   | on demand  | Replies to commands (`status`, acks) |
| `beacon/weatherstation`     | publish   | every 30 s | Presence broadcast `{id, ip_address, command_topic}` |

### Commands

Publish one of these strings to `culverhay/weather/command` (case-insensitive):

| Command                         | Action |
|---------------------------------|--------|
| `status`                        | Publish a health + readings snapshot to the status topic |
| `report` / `report-now`         | Force an immediate sensor refresh, MQTT publish and WOW upload |
| `ip` / `address`                | Publish the current IP address |
| `version`                       | Publish the firmware version |
| `reset-rain`                    | Reset the daily rain accumulation total |
| `recover` / `recovery`          | Enter recovery mode (MQTT + OTA only; sensor processing suspended) |
| `resume` / `normal`             | Leave recovery mode and resume normal operation |
| `reset` / `reboot` / `restart`  | Reboot the device |

**Recovery mode** keeps the device remotely reachable (MQTT control + OTA) while
suspending all sensor processing and reporting — use it to recover a misbehaving
unit without a physical visit, then `resume` or `reset`.

`status` payload:

```json
{
  "id": "weatherstation",
  "ip": "0.0.0.0",
  "rssi": -60,
  "uptime_s": 1234,
  "heap": 123456,
  "version": "1.2.3",
  "boot_reason": "power-on",
  "recovery": false,
  "atmosphere": true,
  "rain": true,
  "wind": true,
  "river": true,
  "temp": 0.00,
  "humidity": 0.00,
  "pressure": 0.00,
  "windspeed": 0.00,
  "windgust": 0.00,
  "winddir": 0.00,
  "rain_mm_hr": 0.00,
  "river_level": 0.000,
  "rain_day": 0.000
}
```

## HTTP endpoints (port 80)

| Method | Path       | Response | Description |
|--------|------------|----------|-------------|
| GET    | `/`        | JSON     | Current readings (web dashboard payload) |
| GET    | `/metrics` | text     | Prometheus exposition |
| —      | `/update`  | —        | ElegantOTA firmware update UI |

### `GET /` payload

```json
{
  "time": "30 Jun 26 12:00 BST",
  "hiResTemp_C": 0.00,
  "humidity_RH": 0.00,
  "pressure_hPa": 0.00,
  "rain_mm_hr": 0.00,
  "rain_rate": 0.00,
  "rain_day": 0.00,
  "wind_dir": 0.00,
  "wind_speed": 0.00,
  "wind_gust": 0.00,
  "status": {
    "pressure_humidity": true,
    "temperature": true,
    "wind": true,
    "rain": true,
    "river": true
  }
}
```

The `status` object reports each sensor's live health — `true` = active/working,
`false` = inactive/not working. `pressure_humidity` and `temperature` come from
the atmospheric sensors (BME280 / MCP9808), `wind` is the masthead CAN node,
`rain` is the tipping-bucket meter, and `river` reflects the last EA API scrape.

### `GET /metrics` (Prometheus — names locked, AGENTS.md Rule 1)

| Metric                 | Type    | Unit          |
|------------------------|---------|---------------|
| `atmospheric_pressure` | gauge   | hPa           |
| `rain_min_rate`        | gauge   | mm/min        |
| `rain_day`             | counter | mm (9am–9am)  |
| `relative_humidity`    | gauge   | % RH          |
| `temperature`          | gauge   | °C            |
| `windspeed`            | gauge   | mph           |
| `windgust`             | gauge   | mph           |
| `winddirection`        | gauge   | degrees       |

#### River metrics (ported from the `riverMonitor` reference — names locked)

The device also polls the UK Environment Agency flood-monitoring API (station
`RIVER_STATION_ID`, default `53125`) every 7 minutes and exposes the result on
the same `/metrics` endpoint. Names match the `riverMonitor` Go service so
existing Prometheus/Grafana dashboards keep working.

| Metric                                   | Type    | Unit / meaning              |
|------------------------------------------|---------|-----------------------------|
| `riverlevel`                             | gauge   | metres                      |
| `period`                                 | gauge   | measurement period          |
| `river_monitor_api_fetch_errors_total`   | counter | total fetch/parse errors    |
| `river_monitor_api_fetch_success_total`  | counter | total successful fetches    |
| `river_monitor_last_scrape_success`      | gauge   | 1 = last scrape OK, else 0  |

### Prometheus scrape config

Weather **and** river metrics are served from the single `/metrics` endpoint on
port 80, so one scrape job covers everything (the standalone `riverMonitor`
service and its `:50000` port are no longer needed):

```yaml
global:
  scrape_interval: 15s
  scrape_timeout: 10s
  evaluation_interval: 15s
  external_labels:
    monitor: culverhay

alerting:
  alertmanagers:
  - follow_redirects: true
    scheme: http
    timeout: 10s
    api_version: v2
    static_configs:
    - targets: []

scrape_configs:
- job_name: weather
  honor_timestamps: true
  metrics_path: /metrics
  scheme: http
  static_configs:
  - targets:
    - 192.168.3.200:80
    labels:
      instance: weather-station
```

## Reporting schedule

- **Every 30 seconds:** refresh sensor values, publish a **retained** message to
  `culverhay/weather` (so the mobile app gets the latest reading immediately on
  subscribe).
- **Every 15 minutes (`ReportFreqMin`):** upload to WOW MetOffice + persist rain
  totals to NVS.
- **09:00 local:** reset the daily rain total (WOW 9am–9am convention).

## Project layout

| Path                       | Contents |
|----------------------------|----------|
| `src/main.cpp`             | Setup + cooperative main loop |
| `src/net.cpp`              | Ethernet-primary / WiFi-fallback networking |
| `src/reporting.cpp`        | MQTT data + control plane, WOW upload, scheduling |
| `src/webserver.cpp`        | HTTP `/` and `/metrics` |
| `src/anemometer.cpp`       | CAN/TWAI wind ingestion |
| `src/atmosphere.cpp`       | MCP9808 + BME280 |
| `src/rainmeter.cpp`        | Rain bucket + tip LED |
| `src/river.cpp`            | Environment Agency river-level poller (Prometheus) |
| `include/constants.h`      | All tunable constants + topic names |
| `include/*.h`              | Pure logic (buffer, wind maths, conversions) |
| `docs/MASTHEAD_CAN_SPEC.md`| CAN frame contract for the remote wind node |
| `AGENTS.md`                | Governing rules (locked metrics, calc fidelity, doc rule) |
