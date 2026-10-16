// Transmission of the weather data to an MQTT broker (FR-6.1 .. FR-6.6).
// The client connects once Wi-Fi has an IP address and reconnects on its own.
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

// Publishes one measurement as JSON. `sample_time` is the Unix time of the
// reading, or 0 if the clock is not synchronized yet (field left out).
// Returns ESP_ERR_INVALID_STATE while not connected to the broker.
esp_err_t publish(const Bme280Data &data, time_t sample_time);

}  // namespace telemetry
