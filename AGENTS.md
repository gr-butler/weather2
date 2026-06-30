# Weather Station — ESP32 Port Agent Instructions

## Project Overview

This project is a port of the **`weather/`** Go application (running on a Raspberry Pi Zero) to an **ESP32** using C/C++ and PlatformIO. The Go project is the **reference implementation** — behaviour, calculations, and data output must match it exactly unless there is a documented hardware reason to differ.

- Reference implementation: `../weather/` (Go, Pi Zero)
- Target implementation: this project — `weather2/` (C/C++, PlatformIO/ESP32)

---

## Hard Rules

### 1. Do Not Change Grafana Endpoint Names or Units

The Prometheus metrics exposed by the device are consumed by Grafana dashboards that must not be broken. **Never rename, remove, or change the unit of any metric.**

| Metric name             | Unit         | Description                            |
|-------------------------|--------------|----------------------------------------|
| `atmospheric_pressure`  | hPa          | Barometric pressure                    |
| `rain_min_rate`         | mm/min       | Rain rate (last 1-minute window)       |
| `rain_day`              | mm           | Daily rain accumulation (9am–9am)      |
| `relative_humidity`     | % RH (0–100) | Relative humidity                      |
| `temperature`           | °C           | Outdoor temperature                    |
| `windspeed`             | mph          | 60-second rolling-average wind speed   |
| `windgust`              | mph          | Max 3-second rolling average (10-min)  |
| `winddirection`         | degrees      | Wind direction (0–360°)                |

If the ESP32 exposes metrics via HTTP/MQTT, the **JSON payload field names** must also be preserved:

| JSON field     | Unit  |
|----------------|-------|
| `time`         | ISO / UTC string |
| `hiResTemp_C`  | °C    |
| `humidity_RH`  | % RH  |
| `pressure_hPa` | hPa   |
| `rain_mm_hr`   | mm/hr |
| `rain_rate`    | mm/min|
| `rain_day`     | mm    |
| `wind_dir`     | degrees |
| `wind_speed`   | mph   |
| `wind_gust`    | mph   |

The MQTT topic **`culverhay/weather`** must not be changed.

---

### 2. Preserve and Verify Wind Speed and Gust Calculations

These calculations must be reproduced exactly from the Go reference. Before committing any wind-related code, verify the maths against the reference.

#### Key constants (from `weather/env/constants.go`)
```
MphPerTick              = 1.429       // mph per anemometer pulse
WindSamplesPerSecond    = 4           // one sample every 250 ms
WindBufferLengthSeconds = 60          // 1-minute rolling window → 240 samples
```

#### Wind speed (`GetSpeed`)
1. Collect pulse counts at `WindSamplesPerSecond` (4 Hz) into a 240-sample circular buffer.
2. Take the **mean pulse count per sample** across the entire 60-second buffer.
3. Convert to ticks/sec: `ticksPerSec = avg * WindSamplesPerSecond`
4. Convert to mph: `speed = MphPerTick * ticksPerSec`
5. Sanity-clamp: if `speed > 100 mph`, log an error and return 0.

#### Gust speed (`GetGust`)
Definition (Met Office): *"the maximum 3-second average wind speed occurring in any period (10 min)".*

1. Use the same 60-second circular buffer (can be extended to 10 min if hardware allows).
2. Slide a **3-second window** (= `WindSamplesPerSecond * 3 = 12 samples`) across the full buffer.
3. `threeSecMax` = maximum sum of any 12 consecutive samples.
4. Convert to mph: `gust = (threeSecMax / 3.0) * MphPerTick`
5. Sanity-clamp: if `gust > 120 mph`, reuse the previous valid reading.

#### Direction
- Convert the bearing value received over CAN to compass degrees using the **same voltage thresholds** as `voltToDegrees()` in `weather/sensors/Anemometer.go` — the remote node is expected to send a pre-converted ADC voltage or raw ADC counts; either way the same lookup table must be applied.
- If wind speed is 0, **preserve the last valid direction** (do not report garbage values).

---

### 3. CAN Bus Wind Sensor Interface (Design Change from Reference)

The Go reference polls an I2C masthead device (address `0x55`) at 4 Hz to retrieve pulse counts. **This design is replaced in the ESP32 build by a CAN bus push model.** The remote masthead node sends frames to the ESP32; there is no polling.

**The circular-buffer logic and all calculations in Rule 2 are unchanged.** Only the data-ingestion path differs.

#### Waveshare SN65HVD230 CAN Board
The transceiver board is the [Waveshare SN65HVD230 CAN Board](https://www.waveshare.com/wiki/SN65HVD230_CAN_Board), based on the TI **SN65HVD230** (ISO 11898-2, up to 1 Mbps). It operates at **3.3 V** and is natively compatible with ESP32 GPIO — **no level shifter required**.

| Board pin | Function | Connection |
|-----------|----------|------------|
| VCC       | Supply   | ESP32 3.3 V |
| GND       | Ground   | Common GND |
| CTX       | Transmit input (from controller) | ESP32 TWAI_TX GPIO |
| CRX       | Receive output (to controller)   | ESP32 TWAI_RX GPIO |
| CANH      | CAN bus high | CAN bus |
| CANL      | CAN bus low  | CAN bus |

**ESP32 CAN peripheral:** Use the built-in **TWAI** controller (`#include "driver/twai.h"`).
- Declare `TX_GPIO_NUM` and `RX_GPIO_NUM` in `platformio.ini` build flags — do not hard-code them in `.cpp`.
- The MCP2515 SPI alternative is **not** recommended when TWAI is available.

#### CAN frame format
Document the agreed frame layout in `include/can_protocol.h`. Until finalised, treat it as:

| Byte(s) | Field          | Type    | Unit / notes                        |
|---------|----------------|---------|-------------------------------------|
| 0       | pulse_count    | uint8   | pulses since last frame (≤ 25 max)  |
| 1–2     | adc_raw        | uint16  | wind-direction ADC counts           |

- CAN ID for wind frames must be defined as a named constant (`WIND_CAN_ID`) in `include/can_protocol.h`.
- If the protocol changes (e.g. pre-converted direction degrees), update the header and document the change — do **not** silently alter the buffer-insertion logic.

#### Sampling discipline — preserving the 4 Hz buffer rate
The buffer calculations assume **exactly `WindSamplesPerSecond` (4) inserts per second**. Violating this corrupts speed and gust values.

- **Option A (preferred):** the remote node transmits at exactly 4 Hz (every 250 ms). The ESP32 receives each frame and pushes `pulse_count` directly into the buffer.
- **Option B:** the remote node transmits at a different rate. The ESP32 must accumulate pulses and use a 250 ms local timer to insert one sample per slot — inserting 0 if no frame arrived in that window.

Document which option is implemented in a comment at the top of `src/anemometer.cpp`.

#### Stale / missing frame handling
- If no CAN frame is received within **500 ms** of an expected 250 ms slot, insert `0` pulses and the last known direction value into the buffer, and log a warning.
- After **5 consecutive missed frames** (1.25 s), log an error and mark the wind sensor as offline (mirror the behaviour of the I2C error path in the Go reference).
- **Do not** leave the buffer unfilled — unfilled slots cause the rolling average to be calculated over a shrinking window and will produce inflated speed readings.

#### Direction from CAN
- Apply `voltToDegrees()` (or its equivalent lookup) to the received ADC value on the ESP32 side before inserting into `dirBuf`.
- If `pulse_count == 0` in the received frame, insert the last valid direction (same zero-wind rule as the Go reference).

---

### 4. Preserve Rain Calculations

| Constant    | Value    | Meaning                         |
|-------------|----------|---------------------------------|
| `MmPerTip`  | 0.3537   | mm of rainfall per bucket tip   |

- Debounce bucket-tip input: ignore glitches < 10 ms; ignore repeated edges within 500 ms.
- **Rate** = `MmPerTip * sum_of_tips_in_last_60_seconds` (mm/hr × appropriate scaling).
- **Day total** resets at 09:01 local time (matching the WOW MetOffice convention used in the Go project).

---

### 5. Preserve Unit Conversions

These conversions feed the WOW MetOffice API and must not be altered:

| Constant     | Value   | Conversion          |
|--------------|---------|---------------------|
| `HPaToInHg`  | 0.02953 | hPa → inches of mercury |
| `MmToInch`   | 25.4    | mm → inches         |

Temperatures reported to WOW must be in **°F**; internally store and compute in **°C**.

---

## General Coding Rules

### Language & Toolchain
- Language: **C++17** (PlatformIO default for ESP32).
- Framework: **Arduino** (PlatformIO `framework = arduino`).
- Use `platformio.ini` build flags for compile-time constants — do not hard-code values in `.cpp` files that are already in `platformio.ini`.

### Structure
- Mirror the Go package structure where practical: separate files for `anemometer`, `rainmeter`, `atmosphere`, `sensors`, `data`, `reporting`.
- Put shared constants in `include/constants.h`; never duplicate them across `.cpp` files.
- Use `class` types to encapsulate sensor state (matching the Go `struct` + methods pattern).

### Hardware Differences (Pi Zero → ESP32)
- **Wind sensor interface changed**: the Go reference uses I²C polling to masthead `0x55`; the ESP32 build receives wind data over **CAN bus** via the Waveshare SN65HVD230 board (see Rule 3 above). Do not re-introduce I²C polling for wind.
- I²C (`Wire` library): still used for atmospheric sensors (BME280 / similar); ensure correct SDA/SCL pin assignments in `platformio.ini`.
- ADC for wind direction: direction is received over CAN from the remote node — the ESP32 does **not** read a direction ADC directly. Apply the `voltToDegrees()` lookup to the ADC value supplied in the CAN frame.
- GPIO interrupts: use `attachInterrupt` with `FALLING` edge for the **rain** pulse input only; apply software debounce matching the Go reference values. Wind pulses are handled by the remote CAN node.
- No `periph.io` library — use ESP-IDF/Arduino equivalents; document any behavioural differences.

### Networking & Reporting
- WiFi credentials and site keys must be stored in a separate `private.h` or `secrets.h` (gitignored) — never committed to source control.
- MQTT publish interval: every **15 minutes** (`ReportFreqMin = 15`) to match the Go implementation.
- If implementing a Prometheus-compatible `/metrics` HTTP endpoint, use the same metric names from the table above.

### Safety & Reliability
- Validate all sensor readings before use; clamp out-of-range values (see wind sanity clamps above).
- On sensor init failure, log the error clearly and disable that sensor rather than crashing.
- Use a watchdog timer to recover from hangs.

---

## Reference Files

When in doubt, consult the Go reference implementation:

| Topic                   | Reference file                              |
|-------------------------|---------------------------------------------|
| Wind speed/gust logic   | `weather/sensors/Anemometer.go`             |
| Rain logic              | `weather/sensors/Rainmeter.go`              |
| Atmospheric sensors     | `weather/sensors/Atmosphere.go`             |
| Constants               | `weather/env/constants.go`                  |
| Prometheus metric names | `weather/main.go`                           |
| WOW/JSON reporting      | `weather/reporting.go`                      |
| Circular buffer         | `weather/buffer/buffer.go`                  |
