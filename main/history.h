// Measurement history in the internal flash (FR-7.1 .. FR-7.6, FR-10.6).
// Fixed-size records in a ring buffer on the "history" data partition; when
// it is full, the oldest sector is erased. Every sector starts with a header
// (device identifier) and the definitions of the sensors, so the records name
// their device and sensor. The history is read back on the serial console
// with the `log` command and on the web page.
#pragma once

#include <cstddef>
#include <cstdint>
#include <ctime>
#include <functional>
#include "bme280.h"
#include "esp_err.h"

namespace history {

// One stored measurement as read back. The strings are valid during the
// visitor call only.
struct Entry {
    uint32_t seq;          // 24 bits, restarts at 0 after 16,777,215
    time_t time;           // 0 = clock was not synchronized
    uint8_t sensor_index;  // position of the sensor (FR-10.3)
    const char *device_id; // from the sector header
    const char *sensor_id; // from the sensor definition, "" if unknown
    float temperature_c;
    float humidity_rh;
    float pressure_hpa;
};

// Return false to stop the iteration.
using Visitor = std::function<bool(const Entry &)>;

// Header line of the CSV format (with the line break).
extern const char *const CSV_HEADER;

// Finds the history partition and locates the newest record. Sectors in the
// format before v2.0.0 are ignored and erased when the ring buffer reaches
// them. Call after identity::init().
esp_err_t init();

// Appends one measurement of the sensor at `sensor_index`. `sample_time` is
// the Unix time of the reading, or 0 if the clock is not synchronized yet.
// A new sensor definition is written first if the sensor at that position
// changed (FR-10.7).
esp_err_t append(const Bme280Data &data, time_t sample_time, uint8_t sensor_index = 0);

// Calls `fn` for the newest `last_n` measurements (0 = all), oldest first.
// Each sector is copied under the lock and `fn` runs without it, so logging
// goes on meanwhile. Returns ESP_ERR_INVALID_STATE if the history is not
// available.
esp_err_t visit(uint32_t last_n, const Visitor &fn);

// Writes one record as a CSV line (with the line break) into `buf`, in the
// columns of CSV_HEADER. Returns the length, like snprintf.
int format_csv(const Entry &entry, char *buf, size_t len);

// Registers the console command `log info | dump [N] | clear`.
// Call after init() and before the console REPL starts.
esp_err_t register_console_command();

}  // namespace history
