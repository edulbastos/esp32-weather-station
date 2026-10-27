// Web page on the local network (FR-8.1 .. FR-8.7, FR-10.5).
// HTTP server on port 80, started once Wi-Fi has an IP address: the page,
// the current values and the history as JSON, and the history as CSV.
// Read-only; failures are never critical for the station.
#pragma once

#include <cstdint>
#include <ctime>
#include "bme280.h"
#include "esp_err.h"

namespace web_server {

// Latest measurement, provided by main.cpp.
struct Current {
    bool valid;          // false until the first reading or after sensor loss
    Bme280Data values;
    time_t time;         // Unix time of the reading, 0 before NTP sync
    uint32_t age_ms;     // time since the reading
};

// Fills `out`; returns false if the data could not be read right now.
using CurrentFn = bool (*)(Current &out);

// Prepares the server; it starts on the first IP_EVENT_STA_GOT_IP.
// Must be called after net_time::start() succeeded.
esp_err_t start(CurrentFn current);

// Requests answered since start-up, for the `sys` console command.
uint32_t requests_served();

}  // namespace web_server
