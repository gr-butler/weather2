# ESP32 Port — Conversion Plan

Reference implementation: `../weather/` (Go, Raspberry Pi Zero)  
Target: `weather2/` (C++17, PlatformIO, ESP32)

---

## Phase 1 — Project Foundation
- [x] **`platformio.ini`** — board, framework (`arduino`), C++17, build flags for all pin assignments (`TX_GPIO_NUM`, `RX_GPIO_NUM`, `RAIN_PIN`, SDA, SCL)
- [x] **`include/constants.h`** — port all values from `weather/env/constants.go`: `MphPerTick`, `WindSamplesPerSecond`, `WindBufferLengthSeconds`, `MmPerTip`, `HPaToInHg`, `MmToInch`, `ReportFreqMin`
- [x] **`include/secrets.h`** (gitignored) — WiFi SSID/password, MQTT broker host/port, WOW site ID + PIN; added to `.gitignore`
- [x] **`include/can_protocol.h`** — `WIND_CAN_ID`, CAN frame struct (`pulse_count` uint8, `adc_raw` uint16)

## Phase 2 — Circular Buffer
Port `weather/buffer/buffer.go` → `include/buffer.h` (header-only)

- [x] Fixed-size circular buffer with position tracking
- [x] `addItem()` — pre-fill all slots on first insert (matches Go `first` flag behaviour)
- [x] `getAverageMinMaxSum()`, `getRawData()`, `getLast()`, `sumMinMaxLast()`, `averageLast()`
- [x] Thread-safe (`std::mutex` — works on ESP32 and host)
- [x] **Unit tests** written (`test/test_buffer/`) — ported from `buffer_test.go`. NOTE: host
      execution blocked by antivirus quarantining linked test exe; buffer compiles clean.

## Phase 3 — Atmospheric Sensors
Port `weather/sensors/Atmosphere.go` → `src/atmosphere.cpp` / `include/atmosphere.h`

- [x] **BME280** I2C `0x76` — pressure + humidity (Adafruit BME280 library)
- [x] **MCP9808** I2C `0x18` — high-resolution temperature
- [x] `getHumidityAndPressure()` → hPa, % RH (Go rounding: humidity whole %, pressure 2 dp)
- [x] `getTemperature()` → °C (MCP9808 primary, BME280 fallback)
- [ ] Sea-level pressure correction (MSLP) + dew point — lives in reporting (Phase 9), using `Rd`, `g`, `z0`, `kelvin` from `weather/reporting.go`
- [x] Graceful init failure — log error, mark sensor offline, do not crash

## Phase 4 — Rain Sensor
Port `weather/sensors/Rainmeter.go` → `src/rainmeter.cpp` / `include/rainmeter.h`
(plus `include/led.h` ported from `weather/led/led.go`)

- [x] GPIO interrupt on `FALLING` edge; software debounce: repeated edges within 500 ms ignored
- [x] 60-slot circular buffer (one slot per minute)
- [x] `getRate()` → `MmPerTip * sum_of_tips_in_last_hour`
- [x] `getDayAccumulation()` / `resetDayAccumulation()` — reset driven by reporting at 09:01 (Phase 9/10)
- [x] `getAccumulation()` — mm since last call (read + reset), for WOW deltas
- [x] Non-blocking `Led` helper (tip LED + heartbeat)

## Phase 5 — CAN Bus Wind Reception
Port data-ingestion path of `weather/sensors/Anemometer.go` → `src/anemometer.cpp`  
*(Replaces I2C polling to masthead `0x55` with CAN push from remote node via Waveshare SN65HVD230 board)*

- [x] Initialise ESP32 TWAI controller with `TX_GPIO_NUM` / `RX_GPIO_NUM` from build flags
- [x] CAN receive — non-blocking `twai_receive` drain in `update()`, parse frame per `include/can_protocol.h`
- [x] Document **Option A vs B** at top of file — **Option B** implemented (accumulate pulses, 250 ms local timer inserts one sample/slot)
- [x] Stale frame handling: insert 0 + last direction after a missed slot; mark wind offline after **5 consecutive missed frames**

## Phase 6 — Wind Speed & Gust Calculations
Complete `src/anemometer.cpp` — exact port of `GetSpeed()`, `GetGust()`, `voltToDegrees()` from `weather/sensors/Anemometer.go`

- [x] `getSpeed()` — mean over 240-sample buffer × `WindSamplesPerSecond` × `MphPerTick`; clamp > 100 mph → return 0, log error
- [x] `getGust()` — sliding 12-sample (3 s) window over full buffer; max sum × (`MphPerTick / 3.0`); clamp > 120 mph → reuse last valid value
- [x] `voltToDegrees()` — identical voltage threshold lookup table from Go reference (in `include/wind_math.h`)
- [x] `getDirection()` — average of `dirBuf`; hold last valid value when speed == 0
- [x] **Maths separated into pure `include/wind_math.h`**; native tests written (`test/test_wind_math/`, ported from `Anemometer_test.go`)

## Phase 7 — MQTT Reporting
Port `weather/main.go` (MQTT setup) + `weather/reporting.go` → `src/reporting.cpp`

- [x] WiFi-connected MQTT client (PubSubClient); auto-reconnect on connection loss
- [x] Publish JSON to topic **`culverhay/weather`** — **every minute** (matches Go loop; AGENTS.md "15 min" note is inaccurate — Go only gates WOW/DB on ReportFreqMin)
- [x] MQTT JSON field names preserved exactly from Go `dataMap`:
  `name`, `ip_address`, `time`, `rain`, `temp`, `windspeed`, `windgust`, `winddir`, `humidity` (all 2-dp strings)
- [x] Persist rain totals across reboots (ESP32 NVS / `Preferences`)
- [x] `secrets.h` created (gitignored): WiFi, MQTT broker, WOW credentials
- [x] Pure conversions extracted to `include/conversions.h` (ctof, mmToIn, MSLP, dewpoint) for reuse in Phase 9

## Phase 8 — HTTP Endpoints
Port `weather/main.go` handler + Prometheus metrics → `src/webserver.cpp`

- [x] `GET /` — JSON `webdata` (live sensor read); field names match Go struct tags exactly:
  `time` (RFC822), `hiResTemp_C`, `humidity_RH`, `pressure_hPa`, `rain_mm_hr`, `rain_rate` (0, as in Go), `rain_day`, `wind_dir`, `wind_speed`, `wind_gust`
- [x] `GET /metrics` — Prometheus text format; metric names match exactly:
  `atmospheric_pressure`, `rain_min_rate`, `rain_day` (counter), `relative_humidity`, `temperature`, `windspeed`, `windgust`, `winddirection`
- [x] Implemented with `ESPAsyncWebServer` (shared instance exposed via `server()` for ElegantOTA in Phase 10); ElegantOTA set to async backend via `ELEGANTOTA_USE_ASYNC_WEBSERVER` build flag

## Phase 9 — WOW MetOffice Upload
Port `weather/reporting.go` WOW section → `src/reporting.cpp`

- [x] HTTP GET to `http://wow.metoffice.gov.uk/automaticreading?` every `ReportFreqMin` (15) min
- [x] Unit conversions (via `include/conversions.h`):
  - Pressure: MSLP-corrected `mslpInHg()` → `baromin`
  - Rain: `mmToInch()` → `dailyrainin` / `rainin`
  - Temperature: `celsiusToF()` → `tempf`
  - `dewPointF()` from temperature and humidity → `dewptf`
- [x] `dateutc` UTC, encoded to match Go (`+`→`%2B`, `:`→`%3A`); all params URL-encoded
- [x] Reset `rainMM`/`rainIn` only after a successful (HTTP 200) upload (matches Go)
- [x] Credentials from gitignored `private.ini` `[secrets]` → injected as `-DWOW_SITE_ID` / `-DWOW_PIN` build flags (blank `wow_site_id` disables upload)

## Phase 10 — Main Loop & System Integration
Complete `src/main.cpp`

- [x] WiFi connect with retry loop (~20 s, then continue offline); logs IP on connect; auto-reconnect enabled
- [x] NTP sync (`configTzTime`, Europe/London) — UTC for WOW `dateutc`, local for 09:00 rain reset + MQTT timestamp
- [x] Initialise all sensors; log online/offline; continue with whatever is available
- [x] Hardware watchdog timer (`esp_task_wdt`, 30 s, panic-reboot; reset each loop)
- [x] Heartbeat LED — flash every 60 s (matches Go reference)
- [x] ElegantOTA attached to the shared web server (`/update`)
- [x] Single cooperative `loop()` (non-blocking sensor `update()` + `Reporting::service()` + LED `service()`) instead of FreeRTOS tasks — simpler and matches the non-blocking class design

## Phase 11 — Validation
- [ ] ~~Run ESP32 build in parallel with Go reference on Pi Zero; compare live readings~~ — **NOT POSSIBLE**: original hardware destroyed by lightning. Validation done by static review against the Go reference + successful firmware build instead.
- [x] Verified all 8 Prometheus metric names and units in `/metrics` output against `weather/main.go` (Rule 1) — `atmospheric_pressure`, `rain_min_rate`, `rain_day`, `relative_humidity`, `temperature`, `windspeed`, `windgust`, `winddirection` all match.
- [x] Verified HTTP JSON payload field names (`time`, `hiResTemp_C`, `humidity_RH`, `pressure_hPa`, `rain_mm_hr`, `rain_rate`, `rain_day`, `wind_dir`, `wind_speed`, `wind_gust`) match `weather/main.go` webdata struct.
- [x] Verified MQTT topic is exactly `culverhay/weather` (`constants.h` `MqttTopic`).
- [x] Reviewed wind speed/gust math (`wind_math.h`) line-by-line against `Anemometer.go` `GetSpeed`/`GetGust`/`getWrappedIndex`/`voltToDegrees` — exact match incl. clamps (>100→0, >120→lastVal) and full direction threshold table.
- [x] Reviewed `buffer.h`↔`buffer.go`, `rainmeter.cpp`↔`Rainmeter.go`, `atmosphere.cpp`↔`Atmosphere.go`, `reporting.cpp`↔`reporting.go`, `constants.h`↔`constants.go` — all behaviour/constants match.

### Review findings / open items
- **Wind direction ADC scaling (`can_protocol.h`)**: `windAdcToVolts()` assumes 16-bit full scale (65535 counts) over a 5 V reference. This MUST be confirmed against the remote masthead node firmware — it is the single most important open item for direction accuracy. Update the header (not the buffer logic) if the remote uses a different resolution/reference.
- **Rain debounce fidelity**: the Go reference filters glitches <10 ms *and* repeats <500 ms; the ESP32 ISR only enforces the 500 ms repeat window (the dominant term for switch bounce). Documented in `rainmeter.cpp`. Acceptable for a tipping bucket.
- **MQTT cadence**: AGENTS.md says "every 15 min", but the Go reference publishes MQTT **every minute** (only WOW/DB/persist are gated on `ReportFreqMin`=15). Implemented per the Go reference.
- **9 am rain reset**: AGENTS.md mentions 09:01; the Go reference actually resets at **09:00** (`t.Minute()==0 && t.Hour()==9`). Implemented per the Go reference (09:00).
- **Native unit tests** (`test_buffer`, `test_wind_math`) are written but cannot be executed locally — antivirus quarantines the linked test executable. Logic is otherwise compile-clean.
- Before deployment: fill `wow_site_id`, `wow_pin`, WiFi and MQTT values in `private.ini` (gitignored). WOW upload is skipped while `WOW_SITE_ID` is empty.

---

## Suggested Order of Attack

```
Phase 1 → Phase 2 (+ tests) → Phase 3 → Phase 4
→ Phase 5 + 6 (highest risk — validate early)
→ Phase 10 skeleton (WiFi + NTP + main loop)
→ Phase 7 → Phase 8 → Phase 9
→ Phase 11
```
