// Identifiers of the station and its sensors (FR-10.1 .. FR-10.3, FR-10.7).
// The device identifier comes from the factory MAC address, the sensor
// identifier from the factory trimming parameters of the chip; the last
// sensor seen at each position is kept in NVS to detect a replacement.
#pragma once

#include <cstddef>
#include <cstdint>
#include "esp_err.h"

namespace identity {

// Sensor types, as stored in the history (FR-10.6).
enum SensorType : uint8_t { SENSOR_BME280 = 1 };

// Sensors of this station: one BME280 at position 0.
constexpr size_t SENSOR_COUNT = 1;

struct Sensor {
    uint8_t type;          // SensorType
    uint8_t index;         // position in the station configuration
    uint8_t address;       // I2C address
    uint32_t fingerprint;  // CRC-32 of the factory trimming parameters
};

// Reads the MAC address and the name, and opens NVS. Call first in app_main.
esp_err_t init();

const uint8_t *device_mac();     // 6 bytes
const char *device_id();         // e.g. "ws-a0b76551a842"
const char *device_name();       // CONFIG_WS_DEVICE_NAME, may be empty
const char *firmware_version();  // e.g. "v2.0.0"

// Writes "ws-" + 12 hex digits.
void format_device_id(const uint8_t mac[6], char *buf, size_t len);
// Writes e.g. "bme280-3fa91c07".
void format_sensor_id(uint8_t type, uint32_t fingerprint, char *buf, size_t len);
const char *sensor_type_name(uint8_t type);

// FR-10.7: records the sensor found at its position, after every successful
// initialization. Returns true if it replaced another chip (logged as a
// warning). The first sensor ever seen at a position is not a replacement.
bool set_sensor(const Sensor &s);

// Current sensor at `index`; false while it is not known yet.
bool sensor(size_t index, Sensor &out);

// Identifier of the current sensor at `index`, "" while it is not known yet.
void sensor_id(size_t index, char *buf, size_t len);

// FR-10.4: the info message (also /api/info, FR-10.5) as JSON. Returns the
// length, like snprintf.
int info_json(char *buf, size_t len);

// The device name escaped for a JSON string (without the quotes).
const char *device_name_json();

}  // namespace identity
