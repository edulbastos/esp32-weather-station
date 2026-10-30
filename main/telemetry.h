// Transmission of the weather data to an MQTT broker (FR-6.1 .. FR-6.6,
// FR-10.4). Each station publishes under its own topics,
// weather-station/<device_id>/data and .../info. The client connects once
// Wi-Fi has an IP address and reconnects on its own.
// Broker failures are never critical for the station.
#pragma once

#include <ctime>
#include "bme280.h"
#include "esp_err.h"

namespace telemetry {

// Creates the MQTT client; it connects as soon as Wi-Fi gets an IP address.
// Must be called after net_time::start() succeeded (needs the network stack).
// Returns ESP_ERR_INVALID_STATE if no broker URI is configured.
esp_err_t start();

// Publishes one measurement of the sensor at position 0 as JSON.
// `sample_time` is the Unix time of the reading, or 0 if the clock is not
// synchronized yet (field left out). `rssi_dbm` is the lowest Wi-Fi RSSI
// since the previous message (FR-5.8), or net_time::NO_RSSI (field left
// out). Returns ESP_ERR_INVALID_STATE while not
// connected to the broker.
esp_err_t publish(const Bme280Data &data, time_t sample_time, int rssi_dbm);

// FR-10.4: publishes the info message (retained). Sent on every connection;
// call it again after a sensor change. Returns ESP_ERR_INVALID_STATE while
// not connected to the broker.
esp_err_t publish_info();

// For the `sys` console command: client created, and connected to the broker.
bool enabled();
bool connected();
const char *client_id();

}  // namespace telemetry
