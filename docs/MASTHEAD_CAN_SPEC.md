# Masthead Wind Node — CAN Bus Interface Specification

**Audience:** firmware author for the *remote masthead node* (a separate project).
**Purpose:** define exactly what the masthead must transmit so the **weather2** ESP32
station can ingest wind speed, gust and direction data.

This is the contract. The receiver side is defined in
[`include/can_protocol.h`](../include/can_protocol.h) and consumed by
[`src/anemometer.cpp`](../src/anemometer.cpp). If you change anything here, both
projects must be updated together.

---

## 1. Physical / Electrical Layer

| Property            | Value                                                        |
|---------------------|-------------------------------------------------------------|
| Bus standard        | CAN 2.0A (ISO 11898-2, high-speed)                          |
| Transceiver (recv)  | Waveshare **SN65HVD230** (TI SN65HVD230), 3.3 V             |
| **Bit rate**        | **500 kbit/s** (must match exactly — receiver is fixed)     |
| Frame format        | **Standard 11-bit identifier** (not extended/29-bit)        |
| Termination         | 120 Ω at each physical end of the bus (one at masthead end) |
| Wiring              | Twisted pair CANH / CANL, common ground reference           |

> The receiver uses the ESP32 built-in **TWAI** controller with
> `TWAI_TIMING_CONFIG_500KBITS()` and an *accept-all* acceptance filter, so the
> masthead may use any standard 11-bit ID, but it **must** use the agreed
> `WIND_CAN_ID` below (the receiver ignores any other ID).

The masthead may use any compatible CAN controller/transceiver (e.g. SN65HVD230,
MCP2551, TJA1050, or an integrated CAN MCU). Only the bus-level parameters above
matter to the receiver.

---

## 2. Wind Data Frame

### 2.1 Identification

| Field           | Value      | Notes                                                    |
|-----------------|------------|----------------------------------------------------------|
| CAN ID          | **`0x100`**| `WIND_CAN_ID`. Standard 11-bit. Receiver drops all others.|
| RTR             | **0**      | Data frame only. Remote-transmit-request frames are ignored.|
| DLC (length)    | **7 bytes**| `WIND_FRAME_DLC`. Frames shorter than 3 bytes (`WIND_FRAME_MIN_DLC`) are discarded.|

### 2.2 Payload layout (7 bytes)

| Byte | Field         | Type     | Endianness     | Meaning                                          |
|------|---------------|----------|----------------|--------------------------------------------------|
| 0    | `pulse_count` | `uint8`  | —              | Anemometer pulses since the **previous** frame.  |
| 1    | `adc_raw` LSB | `uint16` | little-endian  | Wind-direction vane ADC counts (low byte).       |
| 2    | `adc_raw` MSB | `uint16` | little-endian  | Wind-direction vane ADC counts (high byte).      |
| 3–6  | `seq`         | `uint32` | little-endian  | Frame counter; resets to 0 on masthead boot.     |

`adc_raw = data[1] | (data[2] << 8)` — **little-endian** (low byte first).
`seq = data[3] | (data[4]<<8) | (data[5]<<16) | (data[6]<<24)` — **little-endian**.

The `seq` counter increments by 1 per transmitted frame and **resets to 0 when
the masthead boots**. This lets the receiver tell apart:
- **lost frames** on the bus — `seq` jumps forward by more than 1; and
- a **masthead reboot** — `seq` jumps *backwards* to ~0.

It is diagnostic only: it does not affect the wind-speed/gust/direction maths. A
32-bit width means no practical wrap (≈ 34 years at 4 Hz), so a backward jump
always means a reboot. Frames with DLC 3–6 (no `seq`) are still accepted for
backward compatibility; the receiver simply skips the lost/reboot detection.

---

## 3. Field Semantics

### 3.1 `pulse_count` (byte 0)

- Number of anemometer reed-switch / hall pulses counted since the **last
  transmitted frame**. It is a *delta*, not a running total — reset your counter
  to 0 immediately after each frame is queued.
- **Valid range: 0 – 25.** The receiver treats any value `> 25` (`WIND_PULSE_MAX`)
  as a fault and substitutes **0** for that frame. At the 4 Hz transmit rate (see
  §4) even a strong gust stays well under 25 pulses per 250 ms, so a higher value
  indicates switch bounce or EMI at the masthead — debounce there if possible.
- Each pulse corresponds to `MphPerTick = 1.429` mph of instantaneous wind for
  one tick; the receiver does all averaging/gust maths, so the masthead must
  **not** pre-average — just report raw pulse counts per interval.

### 3.2 `adc_raw` (bytes 1–2) — wind direction

- Raw ADC reading of the wind-vane potentiometer voltage.
- The receiver converts counts → volts assuming:

  ```
  volts = (adc_raw / 65535.0) * 5.0     // 16-bit full scale, 5.0 V reference
  ```

  (see `windAdcToVolts()` in `can_protocol.h`).
- It then maps volts → compass degrees with the threshold table in §6.

> ⚠️ **Critical agreement point.** The receiver currently assumes a **16-bit**
> ADC (full scale 65535 counts) referenced to **5.0 V**. The masthead must scale
> its ADC reading to match, **or** we update `WIND_ADC_FULL_SCALE_COUNTS` /
> `WIND_ADC_REF_VOLTS` in `can_protocol.h` to match the masthead hardware.
> Confirm your ADC resolution and reference voltage before first integration.
>
> Examples of how to fill `adc_raw` so the existing receiver maths are correct:
> - True 16-bit ADC, 5.0 V ref → send counts directly.
> - 12-bit ADC (0–4095), 3.3 V ref → either (a) left-shift/scale to a 0–65535,
>   0–5 V equivalent on the masthead, or (b) tell us so we change the header.
> - Already converting to degrees on the masthead → **don't**; this protocol
>   carries raw ADC counts. If you want to send degrees instead, that is a
>   protocol change and the header + receiver must be updated together.

- The vane voltage must span the same range the receiver expects (≈ 0 – 4.55 V
  active, see §6). If `pulse_count == 0` (calm), the receiver **ignores** the
  direction in that frame and holds the last valid bearing, so a noisy reading
  during calm does no harm — but send your best reading anyway.

---

## 4. Timing & Transmit Rate (very important)

The receiver fills a **240-sample circular buffer at exactly 4 Hz** (one sample
every 250 ms) covering a 60-second rolling window. Speed and gust calculations
**depend on this rate**. There are two acceptable strategies:

### Option A — Preferred: transmit at exactly 4 Hz
- Send **one frame every 250 ms** (4 frames/second), each carrying the pulses
  accumulated during that 250 ms window.
- The receiver pushes each frame's `pulse_count` straight into the buffer.
- This is the cleanest design and what the receiver is tuned for.

### Option B — Transmit at a different rate
- Allowed, but the receiver then accumulates incoming pulses and inserts one
  sample per local 250 ms slot. To avoid bias, transmit **at least** every
  250 ms and ideally faster; do **not** transmit slower than 4 Hz or buckets of
  pulses will land in the wrong slot.

**Recommendation: implement Option A — fixed 4 Hz, every 250 ms.**

### Stale / missing-frame behaviour (receiver side — for your awareness)
- If **no** frame arrives within a 250 ms slot, the receiver inserts `0` pulses
  and reuses the last direction, and logs a warning.
- After **5 consecutive missed slots (~1.25 s)** the receiver marks the wind
  sensor **offline**.
- Therefore: keep transmitting at a steady cadence even in calm conditions
  (send `pulse_count = 0` frames). **Silence is interpreted as a fault**, not as
  "no wind".

---

## 5. Worked Examples

### Example 1 — light wind, vane pointing roughly East
- 3 pulses in the last 250 ms.
- Vane ADC reads `0x1A00` (6656 counts) → ≈ 0.508 V → "E" (90°) per §6.

| Byte | Value  |
|------|--------|
| 0    | `0x03` |
| 1    | `0x00` |  ← `adc_raw` LSB
| 2    | `0x1A` |  ← `adc_raw` MSB

Full frame: ID `0x100`, DLC `3`, data `03 00 1A`.

### Example 2 — calm (no wind)
- 0 pulses; still transmit so the receiver knows the node is alive.
- Direction bytes can be the last reading (receiver ignores direction when
  `pulse_count == 0`).

Full frame: ID `0x100`, DLC `3`, data `00 xx xx`.

---

## 6. Direction Voltage → Compass Lookup (reference)

The masthead only sends **raw ADC counts**; it does **not** need to implement
this table. It is provided so you can verify the vane wiring/voltages produce the
expected bearings on the receiver. Thresholds are upper bounds (volts), matching
`windVoltToDegrees()` / the Go `voltToDegrees()` reference.

| Vane voltage `< v` | Degrees | Compass |
|--------------------|---------|---------|
| 0.376              | 112.5   | ESE     |
| 0.441              | 67.5    | ENE     |
| 0.548              | 90.0    | E       |
| 0.775              | 157.5   | SSE     |
| 1.069              | 135.0   | SE      |
| 1.324              | 202.5   | SSW     |
| 1.726              | 180.0   | S       |
| 2.161              | 22.5    | NNE     |
| 2.640              | 45.0    | NE      |
| 3.055              | 247.5   | WSW     |
| 3.315              | 225.0   | SW      |
| 3.705              | 337.5   | NNW     |
| 4.013              | 0.0     | N       |
| 4.258              | 292.5   | WNW     |
| 4.550              | 315.0   | NW      |
| ≥ 4.550 (default)  | 270.0   | W       |

---

## 7. Summary Checklist for the Masthead Firmware

- [ ] CAN 2.0A, **500 kbit/s**, standard 11-bit IDs, 120 Ω termination at the masthead end.
- [ ] Transmit frame ID **`0x100`**, **DLC 3**, data frame (RTR = 0).
- [ ] Byte 0 = pulses since last frame (delta, 0–25); reset counter after each send.
- [ ] Bytes 1–2 = direction ADC counts, **little-endian uint16**.
- [ ] ADC scaled to **16-bit / 5.0 V** convention (or agree a header change).
- [ ] Transmit **every 250 ms (4 Hz)** — including `0`-pulse frames during calm.
- [ ] Never go silent: ≥ 5 missed 250 ms slots marks the sensor offline.

---

## 8. Receiver-Side Reference Files

| Topic                        | File                                          |
|------------------------------|-----------------------------------------------|
| CAN constants / frame decode | `weather2/include/can_protocol.h`             |
| TWAI driver + sampling logic | `weather2/src/anemometer.cpp`                 |
| Wind speed/gust/direction maths | `weather2/include/wind_math.h`             |
| Original (Go) reference      | `weather/sensors/Anemometer.go`               |
| Design rationale (Rule 3)    | `weather2/AGENTS.md`                           |
