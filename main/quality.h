// Measurement quality (FR-11.1 .. FR-11.3): calibration offsets per sensor
// channel, kept in NVS for the chip they were set for; the pressure reduced
// to mean sea level (QNH) from the configured altitude; and the dew point.
#pragma once

#include <cstddef>
#include <cstdint>
#include "bme280.h"
#include "esp_err.h"
#include "identity.h"

namespace quality {

// FR-11.1: offsets in the resolution of the history (FR-10.6).
struct Offsets {
    int16_t temp_centi;   // 0.01 degC
    int16_t hum_centi;    // 0.01 %RH
    int16_t press_deci;   // 0.1 hPa
    bool any() const { return temp_centi != 0 || hum_centi != 0 || press_deci != 0; }
    bool operator==(const Offsets &o) const
    {
        return temp_centi == o.temp_centi && hum_centi == o.hum_centi && press_deci == o.press_deci;
    }
    bool operator!=(const Offsets &o) const { return !(*this == o); }
};

// FR-11.1: binds the calibration to the sensor found at its position, after
// every successful initialization. On the first call the offsets are loaded
// from NVS; offsets stored for another chip, and those in RAM when the chip
// changes, are reset to 0 with a warning.
void bind_sensor(const identity::Sensor &s);

// Current offsets of the sensor at `index` (all 0 if none).
Offsets offsets(size_t index);

// Adds the offsets to a reading; the humidity is clamped to 0..100 %RH.
void apply(size_t index, Bme280Data &d);

// FR-1.6: counts a sample of the sensor at `index` that the driver rejected
// (out of range or skipped), and the count since start for `sys`.
void count_rejected(size_t index);
uint32_t rejected(size_t index);

// Writes e.g. "temperature -1.25 °C, humidity +0.00 %RH, pressure +0.0 hPa".
void format_offsets(const Offsets &o, char *buf, size_t len);

// Registers the console command `cal`.
esp_err_t register_console_command();

// FR-11.2: altitude of the station (CONFIG_WS_ALTITUDE_M) and the factor
// that reduces the station pressure to sea level (ICAO standard atmosphere).
int altitude_m();
float msl_factor();
float msl_factor_at(float altitude_m);
inline float to_msl(float station_hpa) { return station_hpa * msl_factor(); }

// FR-11.3: dew point (Magnus formula, Sonntag 1990). False outside
// -45..60 degC or at a humidity of 0.
bool dew_point(float temperature_c, float humidity_rh, float &out_c);

}  // namespace quality
