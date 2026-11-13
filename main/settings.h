// Per-installation settings (FR-12.1): Wi-Fi network, MQTT broker, station
// name, altitude and METAR airport. They are stored as one set in NVS by the
// setup page (provisioning); while no set is stored, the values of
// `idf.py menuconfig` apply. The set is read once at start and does not
// change while the station runs: a new set takes effect after a restart.
#pragma once

#include <cstddef>
#include <cstdint>
#include "config.h"
#include "esp_err.h"

namespace settings {

struct Settings {
    char wifi_ssid[config::SETTINGS_SSID_MAX + 1];
    char wifi_password[config::SETTINGS_WIFI_PASS_MAX + 1];
    char mqtt_uri[config::SETTINGS_MQTT_URI_MAX + 1];
    char mqtt_username[config::SETTINGS_MQTT_USER_MAX + 1];
    char mqtt_password[config::SETTINGS_MQTT_PASS_MAX + 1];
    char name[config::DEVICE_NAME_MAX + 1];
    int32_t altitude_m;
    char metar[5];  // ICAO code in capitals, "" = check off
};

// Loads the stored set, or the menuconfig values when none is stored. Call
// once in app_main, after NVS is initialized (identity::init()).
void init();

// The active set; valid after init() and unchanged until the next start.
const Settings &get();

// "stored" or "menuconfig".
const char *source_name();
bool stored();

// The values of `idf.py menuconfig` (factory defaults).
Settings defaults();

// Checks the limits of FR-12.1 (config.h). On failure writes the reason,
// e.g. "Wi-Fi password: 8 to 63 characters", and returns false. Turns the
// METAR code into capitals.
bool validate(Settings &s, char *why, size_t len);

// Stores a validated set in NVS; it applies after the next restart.
esp_err_t save(const Settings &s);

// Erases the stored set; the menuconfig values apply after the next restart.
esp_err_t clear();

// Prints the active set and its source with the passwords masked (`setup show`).
void print();

}  // namespace settings
