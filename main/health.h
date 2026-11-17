// System health (NFR-2.3, NFR-2.4): periodic health log, the `sys` console
// command, and the network watchdog that recovers the station when Wi-Fi is
// associated but no traffic gets through.
#pragma once

#include "esp_err.h"

namespace health {

// Starts taskHealth. Call after net_time::start(); without Wi-Fi only the
// health log runs.
esp_err_t start(bool wifi_enabled);

// Registers the console commands `sys`, `ping` and `arp` (NFR-2.5).
esp_err_t register_console_command();

// Logs why the chip started (power-on, software restart, watchdog, ...) and,
// after a restart by the task watchdog (NFR-2.6), which task was silent.
void log_reset_reason();

// NFR-2.6: task watchdog. A task calls watch() once, from the task itself,
// with the longest regular time between two of its cycles, and then alive()
// at every cycle. When a watched task stays silent for that time plus
// WATCHDOG_GRACE_MS, the station logs the task and restarts. The checks
// start with start(). Returns the id for alive(), -1 when the table is full.
int watch(const char *name, uint32_t cycle_ms);
void alive(int id);

// Heap of 8-bit capable memory in KB, as `sys` shows it; also served by
// /api/current (FR-8.9).
struct Heap {
    unsigned free_kb, min_kb, largest_kb;  // free now, lowest since start, largest block
};
Heap heap_now();

}  // namespace health
