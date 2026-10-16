// Measurement history in the internal flash (FR-7.1 .. FR-7.6).
// Fixed-size records in a ring buffer on the "history" data partition; when
// it is full, the oldest sector is erased. The history is read back on the
// serial console with the `log` command.
#pragma once

#include <cstdint>
#include <ctime>
#include "bme280.h"
#include "esp_err.h"

namespace history {

// Finds the history partition and locates the newest record.
esp_err_t init();

// Appends one measurement. `sample_time` is the Unix time of the reading, or
// 0 if the clock is not synchronized yet.
esp_err_t append(const Bme280Data &data, time_t sample_time);

// Registers the console command `log info | dump [N] | clear`.
// Call after init() and before the console REPL starts.
esp_err_t register_console_command();

}  // namespace history
