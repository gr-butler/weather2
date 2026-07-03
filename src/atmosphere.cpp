#include "atmosphere.h"

#include <Arduino.h>
#include <Wire.h>
#include <math.h>

#include "weblog.h"
#define Serial Log // capture Serial output for the /logs web view

// Ported from weather/sensors/Atmosphere.go.

bool Atmosphere::begin() {
    // MCP9808 high-resolution temperature sensor.
    Serial.printf("Starting MCP9808 Temperature Sensor [0x%02X]\n", MCP9808_I2C_ADDR);
    if (mcp_.begin(MCP9808_I2C_ADDR)) {
        // Resolution mode 3 = 0.0625 C (highest), matching mcp9808.High in Go.
        mcp_.setResolution(3);
        mcp_.wake();
        tempOnline_ = true;
    } else {
        Serial.println("Failed to open MCP9808 sensor");
        tempOnline_ = false;
    }

    // BME280 pressure & humidity sensor.
    Serial.printf("Starting BME280 reader [0x%02X]\n", BME280_I2C_ADDR);
    if (bme_.begin(BME280_I2C_ADDR)) {
        phOnline_ = true;
    } else {
        Serial.println("Failed to initialize BME280");
        phOnline_ = false;
    }

    if (phOnline_ && tempOnline_) {
        Serial.println("Atmospheric sensors online");
    }
    return isOnline();
}

bool Atmosphere::getHumidityAndPressure(float &pressureHpa, float &humidityRH) {
    pressureHpa = 0;
    humidityRH = 0;
    if (!phOnline_) {
        return false;
    }

    float rawHumidity = bme_.readHumidity();            // % RH
    float rawPressurePa = bme_.readPressure();          // Pascals
    if (isnan(rawHumidity) || isnan(rawPressurePa)) {
        Serial.println("BME280 read failed");
        return false;
    }

    // Match Go rounding: humidity to nearest whole %, pressure to 2 dp (hPa).
    humidityRH = roundf(rawHumidity);
    float hPa = rawPressurePa / 100.0f;
    pressureHpa = roundf(hPa * 100.0f) / 100.0f;
    return true;
}

float Atmosphere::getTemperature() {
    if (tempOnline_) {
        float c = mcp_.readTempC();
        if (!isnan(c)) {
            return c;
        }
        Serial.println("MCP9808 read failed");
    }
    if (phOnline_) {
        // Fallback to BME280, mirroring the Go reference.
        Serial.println("MCP9808 offline - falling back to BME280");
        float c = bme_.readTemperature();
        if (!isnan(c)) {
            return c;
        }
        Serial.println("BME280 fallback read failed");
    }
    return 0;
}
