// Check of the sea-level pressure against the QNH of a nearby airport
// (FR-11.4). Once an hour taskMetar fetches the METAR of the configured
// airport over HTTPS, compares it with the station QNH at the time of the
// observation, and keeps the median difference of the last days with a
// suggested correction. It only reports; nothing is changed automatically.
#pragma once

#include <cstddef>
#include "esp_err.h"

namespace metar {

// Starts taskMetar when the settings name an airport (FR-12.1).
// Call after net_time::start() succeeded. Returns ESP_ERR_INVALID_STATE if
// no airport is configured.
esp_err_t start();

bool enabled();

// The state as a JSON object (for /api/current and MQTT), or "null" while
// disabled or without a sample. Returns the length, like snprintf.
constexpr size_t JSON_BYTES = 320;
int json(char *buf, size_t len);

// Prints the state for the `sys` console command.
void print_status();

}  // namespace metar
