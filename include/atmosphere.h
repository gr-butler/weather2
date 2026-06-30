#pragma once

#include <Adafruit_BME280.h>
#include <Adafruit_MCP9808.h>

// Atmospheric sensors — ported from weather/sensors/Atmosphere.go.
//   * BME280  (I2C 0x76) — pressure + humidity
//   * MCP9808 (I2C 0x18) — high-resolution temperature
//
// Matches the Go reference behaviour:
//   * humidity rounded to nearest whole % RH
//   * pressure rounded to 2 decimal places (hPa)
//   * temperature: MCP9808 primary, BME280 fallback
//   * on init failure, the sensor is marked offline (we do not crash)

constexpr uint8_t MCP9808_I2C_ADDR = 0x18;
constexpr uint8_t BME280_I2C_ADDR = 0x76;

class Atmosphere {
public:
    Atmosphere() = default;

    // Initialise both sensors. Returns true if at least pressure/humidity is
    // available. Individual sensor availability is tracked separately.
    bool begin();

    bool isOnline() const { return phOnline_ || tempOnline_; }
    bool hasPressureHumidity() const { return phOnline_; }
    bool hasTemperature() const { return tempOnline_ || phOnline_; }

    // Reads pressure (hPa) and relative humidity (% RH) from the BME280.
    // Returns false and leaves outputs at 0 if the sensor is offline/failed.
    bool getHumidityAndPressure(float &pressureHpa, float &humidityRH);

    // Temperature in degrees Celsius. MCP9808 primary, BME280 fallback.
    // Returns 0 if no sensor is available.
    float getTemperature();

private:
    Adafruit_BME280 bme_;   // pressure & humidity
    Adafruit_MCP9808 mcp_;  // temperature
    bool phOnline_ = false;
    bool tempOnline_ = false;
};
