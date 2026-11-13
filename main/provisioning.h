// Setup mode (FR-12.2 .. FR-12.7): the station opens a Wi-Fi access point of
// its own and serves a setup page, so the Wi-Fi network, the MQTT broker and
// the other per-installation settings (FR-12.1) are entered from a phone.
// Entering and leaving setup mode restart the station: a flag in RTC memory
// tells the next start which mode to run.
#pragma once

#include <cstddef>
#include "esp_err.h"

namespace provisioning {

// True when this start must run setup mode: requested before the restart
// (button, console) or no Wi-Fi network in the settings. Call once in
// app_main, after settings::init().
bool wanted_at_start(const char **cause);

// Starts setup mode: access point, captive DNS, setup page. Replaces
// net_time::start() and everything that needs the station's network.
esp_err_t start(const char *cause);

bool active();

// Restarts into setup mode, or back into normal operation (FR-12.2). `cause`
// is logged. Leaving is refused while no Wi-Fi network is configured.
void enter(const char *cause);
void leave(const char *cause);

// FR-12.2: the BOOT button was held for SETUP_HOLD_MS: enters or leaves.
void on_long_press();

// FR-12.3: the lines of the setup screen of the display.
constexpr size_t SCREEN_LINES = 4;
constexpr size_t SCREEN_CHARS = 22;  // 21 characters of the 5x7 font + NUL
void screen(char lines[SCREEN_LINES][SCREEN_CHARS]);

// For `sys`: access point, password, clients and the state of a test.
void print_status();

// FR-12.6: console command `setup`.
esp_err_t register_console_command();

}  // namespace provisioning
