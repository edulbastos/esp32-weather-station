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

// Wi-Fi status for the header icon (FR-5.7): -1 = Wi-Fi not configured or not
// started, 0 = disconnected, 1..3 = connected with weak/fair/good signal.
int wifi_level();

// Link state for the network watchdog (NFR-2.3): associated with the access
// point, and holding an IP address.
bool associated();
bool has_ip();

// Drops the Wi-Fi association; the usual retry logic connects again.
esp_err_t force_reconnect();

}  // namespace net_time
