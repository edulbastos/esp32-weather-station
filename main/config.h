// Central configuration of the weather station (FR-3.1, NFR-3.2).
// All tunable values live here; do not hard-code them elsewhere.
#pragma once

#include <cstddef>
#include <cstdint>
#include "driver/gpio.h"
#include "driver/i2c_types.h"
#include "esp_log.h"
#include "esp_wifi_types.h"
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
// Identification (FR-10.x)
// ---------------------------------------------------------------------------
// FR-10.2: free-text name, set via `idf.py menuconfig` -> "Weather Station"
constexpr const char *DEVICE_NAME     = CONFIG_WS_DEVICE_NAME;
constexpr size_t      DEVICE_NAME_MAX = 32;  // longer names are cut

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
// Energy saving (FR-9.x)
// ---------------------------------------------------------------------------
// BOOT button of the board, active low; toggles the display after start-up.
// (Held during reset it still selects the download mode, as always.)
constexpr gpio_num_t BUTTON_PIN          = GPIO_NUM_0;
constexpr uint32_t   BUTTON_DEBOUNCE_MS  = 250;
// FR-9.2: set via `idf.py menuconfig` -> "Weather Station" (default 5 min, 0 = never)
constexpr uint32_t   DISPLAY_TIMEOUT_MS  = CONFIG_WS_DISPLAY_TIMEOUT_MIN * 60 * 1000;

// ---------------------------------------------------------------------------
// Measurement quality (FR-11.x)
// ---------------------------------------------------------------------------
// FR-11.1: largest calibration offsets accepted by the `cal` command
constexpr float CAL_MAX_TEMP_C    = 5.0f;   // degC
constexpr float CAL_MAX_HUM_RH    = 10.0f;  // %RH
constexpr float CAL_MAX_PRESS_HPA = 10.0f;  // hPa
// FR-11.2: altitude of the station, set via `idf.py menuconfig` -> "Weather
// Station" (metres, default 0); reduces the pressure to sea level (QNH)
constexpr int   ALTITUDE_M        = CONFIG_WS_ALTITUDE_M;

// ---------------------------------------------------------------------------
// Check of the sea-level pressure against a nearby airport (FR-11.4)
// ---------------------------------------------------------------------------
// ICAO code of the airport, set via `idf.py menuconfig` -> "Weather Station";
// empty = check disabled, no request leaves the station
constexpr const char *METAR_STATION     = CONFIG_WS_METAR_STATION;
// API of the Aviation Weather Center (aviationweather.gov/data/api): latest
// METAR of one airport as JSON; at most 100 requests per minute (the station
// makes 2 per hour at most); 204 = no recent observation
constexpr const char *METAR_URL_FORMAT  = "https://aviationweather.gov/api/data/metar?ids=%s&format=json";
// The API asks automated clients for a User-Agent of their own; the firmware
// version is appended. No identifier of the station is sent.
constexpr const char *METAR_USER_AGENT  = "esp32-weather-station";
constexpr uint32_t    METAR_FETCH_MINUTE = 10;   // minute of the hour of the request
constexpr uint32_t    METAR_RETRY_MINUTE = 25;   // second try when the observation was not new
constexpr int         METAR_TIMEOUT_MS   = 10000;
constexpr size_t      METAR_MAX_ANSWER   = 2048;          // bytes; longer answers are refused
constexpr size_t      METAR_MIN_HEAP_BLOCK = 48 * 1024;   // the TLS handshake needs about 40 KB
constexpr uint32_t    METAR_MATCH_S      = 120;   // station data within +/- this of the observation
constexpr float       METAR_QNH_MIN_HPA  = 850.0f;  // plausible reference values
constexpr float       METAR_QNH_MAX_HPA  = 1090.0f;
constexpr size_t      METAR_WINDOW       = 168;   // samples kept (7 days of hourly samples)
constexpr size_t      METAR_MIN_SAMPLES  = 24;    // before the median is reported
constexpr float       METAR_WARN_HPA     = 2.0f;  // |median| above this is logged as a warning

// ---------------------------------------------------------------------------
// Time of day via Wi-Fi + NTP (FR-5.x)
// ---------------------------------------------------------------------------
// Credentials come from `idf.py menuconfig` -> "Weather Station" and live only
// in the local sdkconfig, so they are never committed (FR-5.6).
constexpr const char *WIFI_SSID           = CONFIG_WS_WIFI_SSID;
constexpr const char *WIFI_PASSWORD       = CONFIG_WS_WIFI_PASSWORD;
constexpr uint32_t    WIFI_RETRY_DELAY_MS = 10000;
constexpr size_t      WIFI_DROP_REASONS   = 6;  // counters: 5 reasons apart, the last sums the others (NFR-2.4)
constexpr const char *NTP_SERVER          = "pool.ntp.org";
// POSIX TZ string: UTC-3, Brasilia time, no daylight saving time
constexpr const char *TIMEZONE            = "<-03>3";
// Signal strength thresholds for the Wi-Fi header icon (FR-5.7):
// >= GOOD -> 3 arcs, >= FAIR -> 2 arcs, below -> 1 arc
constexpr int8_t      WIFI_RSSI_GOOD_DBM  = -60;
constexpr int8_t      WIFI_RSSI_FAIR_DBM  = -70;
// Wi-Fi modem power save (radio sleeps between beacons). With some routers
// the station then stops receiving while it can still send; off by default
// on mains power. Turn on to save energy (e.g. on battery).
constexpr bool        WIFI_POWER_SAVE     = false;
// Low, stable Wi-Fi rates (section 6.4). The station sends little data, but
// on a link that passes small packets and loses large ones (over 85 % of the
// pings of 1400 bytes at -61 dBm) the web page stalls while MQTT works. No
// 802.11n, and everything is sent at a fixed rate: 6 Mbps OFDM, a basic rate
// every 802.11g access point receives. Needs A-MPDU off when sending
// (sdkconfig.defaults).
constexpr uint8_t     WIFI_PROTOCOLS      = WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G;
constexpr wifi_phy_rate_t WIFI_TX_RATE    = WIFI_PHY_RATE_6M;
// Log level of the Wi-Fi driver (tag "wifi"). Its INFO lines (Block Ack
// sessions "ba-add"/"ba-del"/"DELBA", power save, state changes) are routine;
// the association is logged by net_time instead.
constexpr esp_log_level_t WIFI_DRIVER_LOG_LEVEL = ESP_LOG_WARN;

// ---------------------------------------------------------------------------
// Data transmission via MQTT (FR-6.x)
// ---------------------------------------------------------------------------
// Broker and credentials come from `idf.py menuconfig` -> "Weather Station"
// and live only in the local sdkconfig (FR-6.6). Empty URI = disabled.
constexpr const char *MQTT_BROKER_URI     = CONFIG_WS_MQTT_BROKER_URI;
constexpr const char *MQTT_USERNAME       = CONFIG_WS_MQTT_USERNAME;
constexpr const char *MQTT_PASSWORD       = CONFIG_WS_MQTT_PASSWORD;
// FR-10.4: topics <prefix>/<device_id>/data and <prefix>/<device_id>/info
constexpr const char *MQTT_TOPIC_PREFIX   = "weather-station";
// FR-6.7: retained payloads of <prefix>/<device_id>/status
constexpr const char *MQTT_STATUS_ONLINE  = "online";
constexpr const char *MQTT_STATUS_OFFLINE = "offline";  // last will, sent by the broker
// Topic used before v2.0.0; its retained message is cleared on every connection
constexpr const char *MQTT_LEGACY_TOPIC   = "weather-station/data";
constexpr int         MQTT_QOS            = 1;     // 1 = acknowledged by the broker, resent if not
constexpr bool        MQTT_RETAIN         = true;  // new subscribers get the last reading
constexpr uint32_t    MQTT_RECONNECT_MS   = 10000;
constexpr int         MQTT_KEEPALIVE_S    = 60;  // ESP-MQTT default 120
// A send that makes no progress for this long drops the connection
// (ESP-MQTT default 10 s); covers link stalls of up to about 30 s.
constexpr int         MQTT_NETWORK_TIMEOUT_MS = 30000;
constexpr uint32_t    MQTT_OUTBOX_LIMIT   = 4096;  // bytes of unacknowledged messages kept

// ---------------------------------------------------------------------------
// Measurement history in the internal flash (FR-7.x)
// ---------------------------------------------------------------------------
// Data partition defined in partitions.csv (project root)
constexpr const char *HISTORY_PARTITION_LABEL = "history";
// Serial console used to read the history (`log` command)
constexpr const char *CONSOLE_PROMPT      = "weather> ";

// ---------------------------------------------------------------------------
// Web page on the local network (FR-8.x)
// ---------------------------------------------------------------------------
constexpr uint16_t    WEB_SERVER_PORT        = 80;
// Simultaneous connections; lwIP has 10 sockets, shared with MQTT and SNTP
constexpr uint16_t    WEB_MAX_CLIENTS        = 4;
constexpr uint16_t    WEB_MAX_ROUTES         = 10;    // registered URIs (9 in use)
constexpr uint16_t    WEB_SEND_TIMEOUT_S     = 15;    // give up sending after this
constexpr size_t      WEB_CHUNK_BYTES        = 4096;  // answer bytes per write (FR-8.6)
constexpr uint32_t    WEB_SLOW_ANSWER_MS     = 1000;  // log answers slower than this (FR-8.6)
// The TCP send buffer (CONFIG_LWIP_TCP_SND_BUF_DEFAULT, 11520 bytes = 8 MSS
// in sdkconfig.defaults) bounds how much can be in flight per connection.
constexpr uint32_t    WEB_HISTORY_HOURS      = 24;    // default window of /api/history
constexpr uint32_t    WEB_HISTORY_MAX_HOURS  = 8760;  // limit of /api/history?hours= (1 year)
constexpr uint32_t    WEB_HISTORY_POINTS     = 150;   // default points of /api/history, other periods
// Chart periods kept ready in RAM (charts.cpp) and their numbers of points
// (FR-8.4): 1 h -> 1 min, 24 h -> 15 min, 7 d -> 2 h per point. Few points
// keep /api/charts small: fewer TCP segments to lose on a weak link.
constexpr uint32_t    WEB_CHART_PERIODS_H[]  = {1, 24, 168};
constexpr uint32_t    WEB_CHART_POINTS[]     = {60, 96, 84};
constexpr uint32_t    WEB_CHART_MAX_POINTS   = 96;    // largest of WEB_CHART_POINTS
constexpr uint32_t    WEB_HISTORY_MAX_POINTS = 1440;  // limit of /api/history?points=

// ---------------------------------------------------------------------------
// System health and network watchdog (NFR-2.3, NFR-2.4)
// ---------------------------------------------------------------------------
constexpr uint32_t    HEALTH_LOG_PERIOD_MS = 10 * 60 * 1000;  // health line in the log
constexpr uint32_t    NET_CHECK_PERIOD_MS  = 60 * 1000;       // gateway ping
constexpr uint32_t    NET_PING_COUNT       = 5;     // pings per check; one reply is enough
constexpr uint32_t    NET_PING_TIMEOUT_MS  = 2000;
constexpr int         NET_CHECK_FAILS      = 3;  // failed checks in a row -> Wi-Fi reconnect
constexpr int         NET_MAX_RECONNECTS   = 2;  // reconnects without recovery -> restart

// ---------------------------------------------------------------------------
// Status LED (blue LED on GPIO2 of the board), used to signal error mode
// ---------------------------------------------------------------------------
constexpr gpio_num_t STATUS_LED_PIN          = GPIO_NUM_2;
constexpr uint32_t   ERROR_BLINK_PERIOD_MS   = 250;

// ---------------------------------------------------------------------------
// Tasks
// ---------------------------------------------------------------------------
// FR-1.4, FR-3.2: set via `idf.py menuconfig` -> "Weather Station" (default 2 s)
constexpr uint32_t    MEASURE_PERIOD_MS   = CONFIG_WS_MEASURE_PERIOD_S * 1000;
constexpr uint32_t    DISPLAY_PERIOD_MS   = 1000;  // FR-2.4: at least every 1 s
constexpr uint32_t    MEASURE_ICON_MS     = 500;   // FR-2.6: measurement icon duration
// FR-6.2: set via `idf.py menuconfig` -> "Weather Station" (default 60 s)
constexpr uint32_t    MQTT_PUBLISH_PERIOD_MS = CONFIG_WS_MQTT_PUBLISH_PERIOD_S * 1000;
// FR-7.2: set via `idf.py menuconfig` -> "Weather Station" (default 60 s)
constexpr uint32_t    LOG_PERIOD_MS       = CONFIG_WS_LOG_PERIOD_S * 1000;

constexpr uint32_t    MEASURE_TASK_STACK  = 5120;  // MQTT info message on a sensor change
constexpr uint32_t    DISPLAY_TASK_STACK  = 4096;
constexpr uint32_t    PUBLISH_TASK_STACK  = 4096;
constexpr uint32_t    LOG_TASK_STACK      = 4096;
constexpr uint32_t    CONSOLE_TASK_STACK  = 6144;  // `log dump` formats floats
constexpr uint32_t    HEALTH_TASK_STACK   = 4096;
constexpr uint32_t    WEB_TASK_STACK      = 6144;  // JSON/CSV formatting + 1 KB send buffer
constexpr uint32_t    METAR_TASK_STACK    = 8192;  // HTTPS client and TLS handshake (FR-11.4)
constexpr UBaseType_t MEASURE_TASK_PRIO   = 5;
constexpr UBaseType_t DISPLAY_TASK_PRIO   = 4;
constexpr UBaseType_t PUBLISH_TASK_PRIO   = 3;
constexpr UBaseType_t LOG_TASK_PRIO       = 3;
constexpr UBaseType_t CONSOLE_TASK_PRIO   = 2;
constexpr UBaseType_t WEB_TASK_PRIO       = 2;
constexpr UBaseType_t HEALTH_TASK_PRIO    = 2;
constexpr UBaseType_t METAR_TASK_PRIO     = 1;

// Max time a task waits for a mutex before giving up on the current cycle
constexpr uint32_t    MUTEX_TIMEOUT_MS    = 500;

// Consecutive failures before a device is re-initialized (NFR-2.2)
constexpr int         MAX_CONSECUTIVE_ERRORS = 3;

}  // namespace config
