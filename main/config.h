// Central configuration of the weather station (FR-3.1, NFR-3.2).
// All tunable values live here; do not hard-code them elsewhere.
#pragma once

#include <cstdint>
#include "driver/gpio.h"
#include "driver/i2c_types.h"
#include "freertos/FreeRTOS.h"
#include "sdkconfig.h"

namespace config {

// ---------------------------------------------------------------------------
// I2C bus (shared by the BME280 and the on-board SSD1306)
// ---------------------------------------------------------------------------
constexpr i2c_port_num_t I2C_PORT        = 0;            // I2C controller 0
constexpr gpio_num_t     I2C_SDA_PIN     = GPIO_NUM_21;  // OLED-SDA on the board
constexpr gpio_num_t     I2C_SCL_PIN     = GPIO_NUM_22;  // OLED-SCL on the board
constexpr uint32_t       I2C_FREQ_HZ     = 400000;       // 400 kHz fast mode
constexpr bool           I2C_INTERNAL_PULLUP = true;
constexpr int            I2C_TIMEOUT_MS  = 100;          // per transaction

// ---------------------------------------------------------------------------
// Devices
// ---------------------------------------------------------------------------
constexpr uint8_t BME280_ADDR  = 0x76;  // 0x76 (SDO=GND) or 0x77 (SDO=VCC)
constexpr uint8_t SSD1306_ADDR = 0x3C;  // usually 0x3C, sometimes 0x3D

// ---------------------------------------------------------------------------
// Display
// ---------------------------------------------------------------------------
constexpr int     DISPLAY_WIDTH    = 128;
constexpr int     DISPLAY_HEIGHT   = 64;   // 64 or 32
constexpr uint8_t DISPLAY_CONTRAST = 0xCF;
// Two-color panels: the top rows are yellow, the rest blue. The header is
// drawn inside the yellow band and the values below it. 0 = single color.
constexpr int     DISPLAY_YELLOW_ROWS = 16;

// ---------------------------------------------------------------------------
// Measurement
// ---------------------------------------------------------------------------
// Reference sea-level pressure used for the altitude estimate (hPa)
constexpr float SEA_LEVEL_PRESSURE_HPA = 1013.25f;

// ---------------------------------------------------------------------------
// Time of day via Wi-Fi + NTP (FR-5.x)
// ---------------------------------------------------------------------------
// Credentials come from `idf.py menuconfig` -> "Weather Station" and live only
// in the local sdkconfig, so they are never committed (FR-5.6).
constexpr const char *WIFI_SSID           = CONFIG_WS_WIFI_SSID;
constexpr const char *WIFI_PASSWORD       = CONFIG_WS_WIFI_PASSWORD;
constexpr uint32_t    WIFI_RETRY_DELAY_MS = 10000;
constexpr const char *NTP_SERVER          = "pool.ntp.org";
// POSIX TZ string: UTC-3, Brasilia time, no daylight saving time
constexpr const char *TIMEZONE            = "<-03>3";

// ---------------------------------------------------------------------------
// Status LED (blue LED on GPIO2 of the board), used to signal error mode
// ---------------------------------------------------------------------------
constexpr gpio_num_t STATUS_LED_PIN          = GPIO_NUM_2;
constexpr uint32_t   ERROR_BLINK_PERIOD_MS   = 250;

// ---------------------------------------------------------------------------
// Tasks
// ---------------------------------------------------------------------------
constexpr uint32_t    MEASURE_PERIOD_MS   = 2000;  // FR-1.4: at least every 2 s
constexpr uint32_t    DISPLAY_PERIOD_MS   = 1000;  // FR-2.4: at least every 1 s

constexpr uint32_t    MEASURE_TASK_STACK  = 4096;
constexpr uint32_t    DISPLAY_TASK_STACK  = 4096;
constexpr UBaseType_t MEASURE_TASK_PRIO   = 5;
constexpr UBaseType_t DISPLAY_TASK_PRIO   = 4;

// Max time a task waits for a mutex before giving up on the current cycle
constexpr uint32_t    MUTEX_TIMEOUT_MS    = 500;

// Consecutive failures before a device is re-initialized (NFR-2.2)
constexpr int         MAX_CONSECUTIVE_ERRORS = 3;

}  // namespace config
