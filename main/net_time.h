// Time of day from NTP over Wi-Fi (FR-5.1 .. FR-5.6).
// Wi-Fi runs in station mode and reconnects on its own; SNTP keeps the system
// clock in sync. Network failures are never critical for the station.
#pragma once

#include <ctime>
#include "esp_err.h"

namespace net_time {

// Starts Wi-Fi and SNTP and returns immediately (does not wait for the
// connection). Returns ESP_ERR_INVALID_STATE if no SSID is configured.
esp_err_t start();

// Fills `out` with the local time. Returns false while the clock has not been
// synchronized yet.
bool local_time(struct tm &out);

}  // namespace net_time
