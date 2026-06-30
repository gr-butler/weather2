#pragma once

#include <math.h>

#include "constants.h"

// Pure, Arduino-independent unit conversions ported from weather/reporting.go.
// Kept in a header so they can be unit-tested on the host (see test/ native env)
// and reused by both MQTT reporting (Phase 7) and WOW upload (Phase 9).

// Constants from reporting.go (mean-sea-level pressure correction).
constexpr double WowRd = 287.1;     // Rd, gas constant J/(kg K)
constexpr double WowG = 9.807;      // gravity m/s^2
constexpr double WowZ0 = 24.71;     // altitude above sea level (m)
constexpr double WowKelvin = 273.1; // Celsius -> Kelvin offset (as used in Go)

// Celsius -> Fahrenheit (Go ctof).
inline double celsiusToF(double c) { return (c * 9.0 / 5.0) + 32.0; }

// Millimetres -> inches (Go mmToIn).
inline double mmToInch(double mm) { return mm / static_cast<double>(MmToInch); }

// Mean-sea-level pressure in inches of mercury (Go prepData MSLP calculation).
//   pressureInHg = pressureHpa * HPaToInHg
//   tempK        = tempC + kelvin
//   H            = (Rd * tempK) / g
//   mslp         = pressureInHg * exp(z0 / H)
inline double mslpInHg(double pressureHpa, double tempC) {
    double pressureInHg = pressureHpa * static_cast<double>(HPaToInHg);
    double tempK = tempC + WowKelvin;
    double H = (WowRd * tempK) / WowG;
    return pressureInHg * exp(WowZ0 / H);
}

// Outdoor dew point in Fahrenheit (Go prepData formula, preserved verbatim):
//   Td = T - ((100 - RH)/5)  then converted C->F via +273/-273 dance.
inline double dewPointF(double tempC, double humidity) {
    return ((((tempC + 273) - ((100 - humidity) / 5.0)) - 273) * 9 / 5.0) + 32;
}
