// System health (NFR-2.3, NFR-2.4): periodic health log, the `sys` console
// command, and the network watchdog that recovers the station when Wi-Fi is
// associated but no traffic gets through.
#pragma once

#include "esp_err.h"

namespace health {

// Starts taskHealth. Call after net_time::start(); without Wi-Fi only the
// health log runs.
esp_err_t start(bool wifi_enabled);

// Registers the console command `sys`.
esp_err_t register_console_command();

// Logs why the chip started (power-on, software restart, watchdog, ...).
void log_reset_reason();

}  // namespace health
