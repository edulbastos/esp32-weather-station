# Functional Specification: ESP32 Weather Station

**Version:** 1.8.9

## Document Control

### Revision History

| Version | Date       | Changes                                   |
|---------|------------|-------------------------------------------|
| 1.0.0   | 2026-10-15 | Initial specification and implementation. |
| 1.1.0   | 2026-10-15 | Time of day via Wi-Fi + NTP (FR-5.1 to FR-5.6). |
| 1.2.0   | 2026-10-15 | Wi-Fi signal icon in the OLED header (FR-5.7). |
| 1.3.0   | 2026-10-15 | Measurement icon in the OLED header (FR-2.6). |
| 1.4.0   | 2026-10-15 | Measurement interval set via menuconfig (FR-3.2, FR-1.4 changed). |
| 1.5.0   | 2026-10-16 | Weather data sent to an MQTT broker (FR-6.1 to FR-6.6). |
| 1.6.0   | 2026-10-16 | Measurement history in the internal flash, read on the serial console (FR-7.1 to FR-7.6). |
| 1.7.0   | 2026-10-16 | Web page on the local network with current values, history charts and CSV download (FR-8.1 to FR-8.6). |
| 1.7.1   | 2026-10-16 | MQTT reconnection follows the Wi-Fi state: immediate after Wi-Fi returns, no attempts while it is down (FR-6.5 changed). |
| 1.8.0   | 2026-10-16 | Energy saving: display on/off with the BOOT button and the console, automatic switch-off after inactivity (FR-9.1, FR-9.2). |
| 1.8.1   | 2026-10-16 | Web page tab icon, `/favicon.ico` no longer answers 404 (FR-8.2 changed); one request per HTTP connection, so idle browser connections no longer cause "error in recv" warnings (FR-8.6 changed). |
| 1.8.2   | 2026-10-16 | Reliability: network watchdog for Wi-Fi associated without traffic, health log and `sys` console command, start reason in the log (NFR-2.3, NFR-2.4). |
| 1.8.3   | 2026-10-16 | Web charts: 1 h / 24 h / 7 d periods, about 150 averaged points, incremental update between full reloads (FR-8.4 changed). |
| 1.8.4   | 2026-10-16 | Network watchdog: MQTT and web traffic count as signs of a working network, gateway ping only as last resort (NFR-2.3 changed); Wi-Fi power save off by default; fixed, readable MQTT client ID shown in the log and in `sys`. |
| 1.8.5   | 2026-10-17 | Web charts: the history is reloaded when the page becomes visible again or after missed measurements, so no gap appears after the page was idle (FR-8.4 changed). |
| 1.8.6   | 2026-10-17 | Faster web page on weak Wi-Fi: page sent gzip-compressed, history answer without scanning the whole partition, iOS home-screen icon requests answered without warnings, longer send timeout (FR-8.2, FR-8.6 changed). |
| 1.8.7   | 2026-10-17 | Weak or unstable Wi-Fi: strongest access point chosen, connection details logged, A-MPDU reception off, MQTT keepalive 30 s (FR-6.5 changed); web charts show "Loading…" and retry a history request after 20 s (FR-8.4 changed). |
| 1.8.8   | 2026-10-17 | Correction: A-MPDU reception aggregation back to the ESP-IDF default (turned off in v1.8.7 after a misread log); routine Wi-Fi driver messages no longer logged (section 6.4 changed); MQTT with QoS 1 so readings in transit survive a reconnection (FR-6.4 changed), keepalive 60 s (FR-6.5 changed). |
| 1.8.9   | 2026-10-18 | Web charts served from tables kept in RAM and fed by every measurement (constant answer time at any measurement or log interval); the page loads all three periods at once, so switching is instant (FR-8.4 changed); each Wi-Fi disconnection logged with network, access point, signal and the meaning of the reason (section 6.4). |

### Requirement Markers
*   **`[PROPOSED]`**: The requirement is new and not implemented yet.
*   **`[vX.Y.Z]`**: The requirement was added in this version.
*   **`[CHANGED vX.Y.Z]`**: The requirement was modified in this version.
*   **`[REMOVED vX.Y.Z]`**: The requirement was dropped in this version. Its text is kept and struck through (`~~...~~`).
*   Requirements without a marker exist since v1.0.0.
*   Requirement IDs are never reused or renumbered.

Older versions of this document are kept as git tags: `git show vX.Y.Z:esp32-weather-station.md`. In v1.0.0 the file was still named `AE-BTG-ESP32096OLED_fsd.md`.

## 1. Introduction

### 1.1 Purpose of the Document
This document describes the functional and non-functional requirements for the ESP32 Weather Station. It serves as a reference for the development, validation, and future extensions of the system.

### 1.2 Scope
The specification covers the acquisition of temperature, humidity, and air pressure data using a BME280 sensor, the display of this data on an SSD1306 OLED display, and the underlying software architecture based on FreeRTOS.

### 1.3 Target Audience
This document is intended for developers, testers, and anyone interested in gaining an in-depth understanding of the weather station.

## 2. Product Overview

### 2.1 Brief Description
The ESP32 Weather Station is a standalone device that continuously collects environmental data (temperature, humidity, air pressure) and displays it in real time on a small OLED screen. The system is robust, energy-efficient, and takes advantage of a real-time operating system (FreeRTOS) for reliable and responsive performance.

### 2.2 Goals and Vision
The main goal is to provide a weather station that is easy to build and use, delivers accurate measurements, and can serve as a foundation for further IoT projects.

## 3. Functional Requirements

### 3.1 Sensor Data Acquisition
*   **FR-1.1:** The system MUST acquire temperature readings from the BME280 sensor.
*   **FR-1.2:** The system MUST acquire humidity readings from the BME280 sensor.
*   **FR-1.3:** The system MUST acquire air pressure readings from the BME280 sensor.
*   **FR-1.4:** [CHANGED v1.4.0] The sensor data MUST be updated at the measurement interval set in sdkconfig (FR-3.2), 2 seconds by default. Before v1.4.0: "at least every 2 seconds".
*   **FR-1.5:** The system MUST provide a means to safely synchronize access to the I2C bus between different tasks (FreeRTOS mutex `i2cMutex`).

### 3.2 Data Display on OLED
*   **FR-2.1:** The system MUST display the current temperature on the SSD1306 OLED display.
*   **FR-2.2:** The system MUST display the current humidity on the SSD1306 OLED display.
*   **FR-2.3:** The system MUST display the current air pressure on the SSD1306 OLED display.
*   **FR-2.4:** The OLED display MUST be refreshed at least once every second.
*   **FR-2.5:** The displayed values MUST be clearly legible and include the corresponding units (e.g. °C, %RH, hPa).
*   **FR-2.6:** [v1.3.0] The system MUST display an icon to indicate when a measurement arrives and disappear after 0.5s. The icon is a filled dot at the left edge of the OLED header; the display is redrawn as soon as a new measurement arrives. The duration is set in `main/config.h`.

### 3.3 Configuration
*   **FR-3.1:** Configuration parameters such as display dimensions, I2C pins and addresses of the sensors and displays, and sea-level pressure MUST be defined centrally in a dedicated header file (`main/config.h`).
*   **FR-3.2:** [v1.4.0] Measurement interval time MUST be done in sdkconfig. It is set via `idf.py menuconfig` ("Weather Station" menu, `CONFIG_WS_MEASURE_PERIOD_S`), in seconds, from 1 to 3600, default 2; `main/config.h` takes the value from there.

### 3.4 Error Handling
*   **FR-4.1:** The system MUST output an error message on the serial console (via `ESP_LOGE`) if the OLED display cannot be initialized.
*   **FR-4.2:** The system MUST output an error message on the serial console (via `ESP_LOGE`) if the BME280 sensor cannot be initialized.
*   **FR-4.3:** In the event of a critical initialization error (display or sensor), the system MUST remain in an error mode or stop operating.

### 3.5 Time of Day
*   **FR-5.1** [v1.1.0]: The system MUST obtain the current date and time from an NTP server (default `pool.ntp.org`) over Wi-Fi (station mode) and keep the clock synchronized periodically.
*   **FR-5.2** [v1.1.0]: The local time MUST use the time zone defined in `main/config.h` (default: UTC-3, Brasília, without daylight saving time).
*   **FR-5.3** [v1.1.0]: Once the clock is synchronized, the OLED header MUST show the date and time as `DD/MM/YYYY HH:MM`. Before synchronization, the header shows the title "WEATHER STATION". A sensor error ("SENSOR ERROR") takes precedence over the date and time.
*   **FR-5.4** [v1.1.0]: Once the clock is synchronized, every measurement written to the serial console MUST be prefixed with a timestamp in the format `YYYY-MM-DD HH:MM:SS`.
*   **FR-5.5** [v1.1.0]: Wi-Fi or NTP failures MUST NOT be critical errors: the weather station MUST keep measuring and displaying, log a warning (`ESP_LOGW`), and retry the Wi-Fi connection periodically (every 10 s by default).
*   **FR-5.6** [v1.1.0]: The Wi-Fi credentials (SSID and password) MUST be set via `idf.py menuconfig` ("Weather Station" menu) and stored only in the local `sdkconfig`, which is not version-controlled. With an empty SSID, Wi-Fi and the time of day are disabled.
*   **FR-5.7** [v1.2.0]: The OLED header MUST show a Wi-Fi icon at its right edge: 1 to 3 arcs according to the signal strength (3 arcs at RSSI ≥ -60 dBm, 2 arcs at ≥ -70 dBm, 1 arc below; thresholds in `main/config.h`) while connected, and an X while disconnected. With no SSID configured, no icon is shown. The header text is centered in the space left of the icon.

### 3.6 Data Transmission (MQTT)
*   **FR-6.1** [v1.5.0]: The system MUST transmit the weather data over Wi-Fi to an MQTT broker (MQTT 3.1.1; `mqtt://` or `mqtts://` URI). This implements the "Wi-Fi connectivity" extension proposed in section 7 up to v1.4.0.
*   **FR-6.2** [v1.5.0]: The latest valid measurement MUST be published at the publish interval set via `idf.py menuconfig` ("Weather Station" menu, `CONFIG_WS_MQTT_PUBLISH_PERIOD_S`), in seconds, from 1 to 86400, default 60. The interval is independent of the measurement interval (FR-3.2). A measurement that was already published is not sent again, and nothing is sent while the sensor has no valid reading.
*   **FR-6.3** [v1.5.0]: Each message MUST be a JSON object with the fields `temperature_c`, `humidity_rh` and `pressure_hpa` (two decimal places) and, once the clock is synchronized (FR-5.1), `time` with the Unix time of the measurement in seconds. Example: `{"time":1792161125,"temperature_c":23.41,"humidity_rh":56.12,"pressure_hpa":1012.84}`.
*   **FR-6.4** [v1.5.0, CHANGED v1.8.8]: Messages MUST be published to the topic defined in `main/config.h` (default `weather-station/data`) with QoS 1 and the retain flag set, so a new subscriber receives the latest reading at once. With QoS 1 the broker confirms each message, and a message not yet confirmed when the connection drops is sent again after the reconnection (messages older than 30 s are dropped; the flash history keeps every reading). Before v1.8.8: QoS 0, so a reading in transit when the connection dropped was lost for MQTT.
*   **FR-6.5** [CHANGED v1.7.1, CHANGED v1.8.7, CHANGED v1.8.8]: MQTT failures MUST NOT be critical errors: the weather station MUST keep measuring and displaying, log a warning (`ESP_LOGW`), skip the publish while disconnected, and reconnect to the broker automatically. While Wi-Fi has an IP address, a failed connection is retried every 10 s by default; while Wi-Fi is down, no connection is attempted; when Wi-Fi gets an IP address again, the client reconnects at once. Since v1.8.7 the MQTT keepalive is 30 s (`main/config.h`), so a connection that stops delivering is noticed after about 45 s; before, the ESP-MQTT default of 120 s took up to 3 minutes. Since v1.8.8 the keepalive is 60 s: a dead connection is noticed after about 90 s, and stalls of up to about 30 s (seen on the path to the broker) no longer force a reconnection, as they did with 30 s. Before v1.7.1: "reconnect to the broker automatically (every 10 s by default)", which also tried, and logged errors, while Wi-Fi was down, and waited up to 10 s after Wi-Fi returned.
*   **FR-6.6** [v1.5.0]: The broker URI, user name and password MUST be set via `idf.py menuconfig` ("Weather Station" menu) and stored only in the local `sdkconfig`, which is not version-controlled. With an empty broker URI, or without Wi-Fi (FR-5.6), data transmission is disabled.

### 3.7 Measurement History
*   **FR-7.1** [v1.6.0]: The system MUST store a history of the measurements in the internal flash, in the 5 MB data partition `history` defined in `partitions.csv`. This implements the "Data logging" extension proposed in section 7 up to v1.5.0 (internal flash option; no SD card). The partition table keeps the offsets of `nvs` and `phy_init` and enlarges the app partition from 1 MB to 2 MB.
*   **FR-7.2** [v1.6.0]: The latest valid measurement MUST be stored at the log interval set via `idf.py menuconfig` ("Weather Station" menu, `CONFIG_WS_LOG_PERIOD_S`), in seconds, from 1 to 86400, default 60. The interval is independent of the measurement and publish intervals. A measurement is stored only once, and nothing is stored while the sensor has no valid reading.
*   **FR-7.3** [v1.6.0]: Each record MUST hold a sequence number, the Unix time of the measurement (0 before the clock is synchronized), the temperature (0.01 °C), the humidity (0.01 %RH), the pressure (0.1 hPa) and a CRC16. The history works as a ring buffer: when it is full, the oldest 4 KB sector (256 records) is erased. Capacity: 327,680 records, about 7.5 months at 60 s. The history survives restarts and power loss; a record cut short by a power loss is discarded.
*   **FR-7.4** [v1.6.0]: The history MUST be readable on the serial console (prompt `weather> `, 115200 baud): `log info` shows the number of records and the space used; `log dump [N]` prints the last N records (all if N is omitted), oldest first, as CSV with the columns `seq,time,temperature_c,humidity_rh,pressure_hpa`, where `time` is local time `YYYY-MM-DD HH:MM:SS` (empty if unknown). Other log output is suppressed during the dump. `help` lists the commands.
*   **FR-7.5** [v1.6.0]: The command `log clear` MUST erase the whole history.
*   **FR-7.6** [v1.6.0]: History failures MUST NOT be critical errors: if the partition is missing or a write fails, the system logs a warning (`ESP_LOGW`) and keeps measuring, displaying and publishing.

### 3.8 Web Page
*   **FR-8.1** [v1.7.0]: The system MUST serve a web page on the local network over HTTP (port 80), reached by the IP address of the station. The server starts once Wi-Fi has an IP address, and the URL (`Web page at http://<ip>/`) is logged on the serial console on every connection. There is no name resolution (mDNS) and no password: the page is read-only and offers no action that changes the station. This implements the "Web server" extension proposed in section 7 up to v1.6.0.
*   **FR-8.2** [v1.7.0, CHANGED v1.8.1, CHANGED v1.8.6]: The page (`/`) MUST show the current temperature, humidity and pressure, the time of the measurement and the Wi-Fi signal level, refreshed at the measurement interval (at least every 1 s). It MUST work without internet access (no external files) and adapt to phone screens and to the light/dark setting of the browser. Since v1.8.1 the page carries its own tab icon (embedded SVG), and `/favicon.ico` answers `204 No Content`, so browsers no longer cause a 404 warning on the serial console; before v1.8.1 the page had no icon and `/favicon.ico` answered 404. Since v1.8.6 the home-screen icons that Safari on iOS asks for (`/apple-touch-icon.png`, `/apple-touch-icon-precomposed.png`) get the same empty answer, and the page is sent gzip-compressed (about 5 KB instead of 12.5 KB) to browsers that accept it, uncompressed to the others.
*   **FR-8.3** [v1.7.0]: `/api/current` MUST return the current values as JSON: `valid`, `time` (Unix time of the measurement, `null` before NTP sync), `age_s`, `temperature_c`, `humidity_rh`, `pressure_hpa`, `wifi_level` (FR-5.7), `measure_period_s` and `log_period_s`. With `valid` false, the measurement fields are left out.
*   **FR-8.4** [v1.7.0, CHANGED v1.8.3, CHANGED v1.8.5, CHANGED v1.8.7, CHANGED v1.8.9]: The page MUST show charts of temperature, humidity and pressure from the measurement history (FR-7.1), over a period chosen with buttons: 1 hour, 24 hours (default) or 7 days; the choice is remembered by the browser. The data come from `/api/history?hours=N&points=P` (N from 1 to 8760, default 24; P from 10 to 1440, default 150, `main/config.h`) as JSON: `hours`, `points`, `bucket_s`, `log_period_s`, `available` and `records`, a list of `[time, temperature_c, humidity_rh, pressure_hpa]`. The station divides the period into P intervals of `bucket_s` seconds and sends the average of the records in each interval (one point per record when the interval is shorter than the log interval); records without a time are left out. The page loads this data when it opens, when the period changes, every 30 minutes, and (since v1.8.5) when it becomes visible again or when the next measurement shows that measurements were missed (more than twice the measurement or log interval, plus 10 s, since the last one; at most once a minute) — browsers slow down or freeze hidden pages, and before v1.8.5 the new point was then drawn after a gap until the page was reloaded by hand; in between it adds each new measurement from `/api/current` to the charts, averaged into the last point while that point is younger than one interval, so the number of points stays about P. A pause longer than three intervals (or three log intervals) breaks the line. Since v1.8.7, when the period changes the charts are cleared at once and show "Loading…" until the data arrive, and a history request that gets no answer within 20 s is given up and retried after 5 s ("station slow to answer, retrying…"); before, the charts of the previous period stayed until the answer came, which on a weak link could take half a minute. Since v1.8.9 the station keeps, for each of the three periods, a table of 150 time buckets in RAM (24 s, 9.6 min and 67.2 min per bucket) holding the running averages of the measurements; every measurement updates one bucket per table, the tables are rebuilt from the flash history at start-up, and `/api/history` answers these periods from RAM in constant time, without reading the flash, whatever the measurement or log interval (the JSON adds `measure_period_s` and `source`: `measurements`; other values of `hours` or `points` are still averaged from the flash history, `source`: `history`). Because the tables are fed by the measurements, short periods can be more detailed than the log interval. The page loads all three periods when it opens (the selected one first) and adds each new measurement to all of them, so switching between periods is instant; before v1.8.9 each switch requested the period from the station and the answer read the flash history. Before v1.8.3: "The page MUST show charts of temperature, humidity and pressure over the last 24 hours (`main/config.h`), taken from the measurement history (FR-7.1) and refreshed at the log interval (at least every 10 s). A pause longer than three log intervals breaks the line. The data come from `/api/history?hours=N` (N from 1 to 8760, default 24) as JSON: `hours`, `log_period_s`, `available` and `records`, a list of `[time, temperature_c, humidity_rh, pressure_hpa]`; at most 1440 records, evenly thinned out; records without a time are left out." (the whole 24-hour data, thinned by skipping records, was loaded again at every log interval).
*   **FR-8.5** [v1.7.0]: `/history.csv` MUST download the whole history as a CSV file, in the format of `log dump` (FR-7.4). The server handles one request at a time, so other requests wait while the download runs.
*   **FR-8.6** [v1.7.0, CHANGED v1.8.1, CHANGED v1.8.6]: The web page MUST NOT be critical: it needs Wi-Fi (FR-5.6) and is disabled without an SSID; server failures only log a warning (`ESP_LOGW`). At most 4 connections are open at once; a new client closes the least recently used one. Since v1.8.1 the server answers one request per connection (`Connection: close`) and then closes it, so idle connections kept open by browsers are not reset later; before v1.8.1 connections stayed open and each browser reset logged `error in recv : 104`. Since v1.8.6 the server waits up to 15 s (`main/config.h`) for the network to take more data before it gives up an answer; before, it gave up after 5 s, which on a lossy Wi-Fi link cut pages off halfway. The history answer reads only the flash sectors that can hold the requested period, so it starts within a fraction of a second (before v1.8.6 it probed every sector of the partition first, about 1.5 s).

### 3.9 Energy Saving
This section gathers the requirements that reduce the power consumption, in view of a future battery supply.

*   **FR-9.1** [v1.8.0]: The user MUST be able to switch the OLED display off and on: the BOOT button of the board (GPIO0) toggles it, and the serial console offers `display on`, `display off` and `display status`. When off, the panel is in sleep mode (SSD1306 command `0xAE`) and no frame is sent over I2C; measuring, history, MQTT and the web page continue. When switched on, the panel shows the current data at once. The display is on after every start-up.
*   **FR-9.2** [v1.8.0]: The display MUST switch itself off after a period without user interaction (a button press or a `display` command), set via `idf.py menuconfig` ("Weather Station" menu, `CONFIG_WS_DISPLAY_TIMEOUT_MIN`), in minutes, from 0 to 1440, default 5; 0 disables the automatic switch-off. A button press switches it on again and restarts the period. Each switch is logged on the serial console with its cause (button, console or inactivity).

## 4. Non-Functional Requirements

### 4.1 Performance
*   **NFR-1.1:** Sensor data acquisition and display updates MUST run in parallel and without noticeable delay.
*   **NFR-1.2:** The use of FreeRTOS MUST ensure efficient resource management.

### 4.2 Reliability
*   **NFR-2.1:** The system MUST operate without interruption over extended periods (weeks/months).
*   **NFR-2.2:** I2C communication errors SHOULD be handled robustly to prevent system crashes.
*   **NFR-2.3:** [v1.8.2, CHANGED v1.8.4] The system MUST detect when Wi-Fi is associated but no traffic gets through, and recover from it (detail of NFR-2.1). Every 60 s it checks for a sign of traffic: the MQTT client is connected (its keepalive proves round trips to the broker), the web server answered a request since the last check, or the default gateway answers at least one of 5 pings (2 s timeout each); the ping is only used when the other signs are missing. A check fails when Wi-Fi is associated and either has no IP address or shows no sign of traffic; 3 failed checks in a row force a Wi-Fi reconnection. The watchdog only acts after the gateway has answered ping at least once (until then it pings at every check), since some routers do not answer ping; while not associated, the usual reconnection (FR-5.5) applies. All values are in `main/config.h`. Before v1.8.4: "The system MUST detect when Wi-Fi is associated but no traffic gets through, and recover from it (detail of NFR-2.1). Every 60 s it pings the default gateway (3 pings). A check fails when Wi-Fi is associated and either has no IP address or the gateway does not answer; 3 failed checks in a row force a Wi-Fi reconnection. The watchdog only acts after the gateway has answered at least once, since some routers do not answer ping; while not associated, the usual reconnection (FR-5.5) applies. All values are in `main/config.h`." A router that occasionally ignored ping could then cause needless Wi-Fi reconnections while MQTT and the web page were working.
*   **NFR-2.4:** [v1.8.2] If the network is still down after 2 forced Wi-Fi reconnections, the system MUST restart; the history (FR-7.3) survives. To help diagnose such failures, the system logs the start reason at boot (power-on, software restart, watchdog, brownout...), logs a health line every 10 minutes (free heap, minimum free heap, largest free block, Wi-Fi signal, MQTT state, web requests served), and offers the console command `sys` with the uptime, start reason, heap, Wi-Fi link, IP and gateway, watchdog state with the last sign of traffic and its source (since v1.8.4; before: last gateway reply), MQTT state and web requests served.

### 4.3 Maintainability
*   **NFR-3.1:** The code MUST be modular and easy to understand, divided into logical modules (header and source files) within the ESP-IDF `main` component.
*   **NFR-3.2:** It MUST be possible to configure the system simply by editing `config.h`.

### 4.4 Safety (Physical)
*   **NFR-4.1:** The system SHOULD be housed in an enclosure to protect the electronics from environmental influences (external, not part of this specification).

### 4.5 Usability (Display)
*   **NFR-5.1:** The displayed information MUST be comprehensible at a glance.

## 5. Technical Architecture (Summary)

### 5.1 Hardware Components
*   **Board:** ideaspark ESP32 development board with built-in 0.96" OLED
*   **Microcontroller:** ESP32 (ESP-WROOM-32 module)
*   **Sensor:** BME280 (temperature, humidity, pressure)
*   **Display:** SSD1306 OLED (I2C, 128x64 pixels)
*   **Flash:** [v1.6.0] 8 MB: `nvs` 24 KB, `phy_init` 4 KB, app `factory` 2 MB, `history` 5 MB (`partitions.csv`)

### 5.2 Software Components
*   **Development framework:** ESP-IDF v6.1 (CMake / `idf.py` build system)
*   **Real-time operating system:** FreeRTOS (part of ESP-IDF)
*   **Programming language:** C++ (entry point `app_main`)
*   **External components:** `espressif/mqtt` (ESP-MQTT client, ESP Component Registry, declared in `main/idf_component.yml`). Since ESP-IDF v6.0 it is no longer part of ESP-IDF; the build downloads it into `managed_components/`.
*   **ESP-IDF components:**
    *   `esp_driver_i2c` (`driver/i2c_master.h`, I2C master communication)
    *   `esp_driver_gpio` (GPIO configuration)
    *   `esp_log` (serial debug output)
    *   `esp_wifi`, `esp_netif`, `esp_event`, `lwip` (Wi-Fi station and TCP/IP stack)
    *   `esp_netif_sntp` (NTP time synchronization, part of `esp_netif`)
    *   `nvs_flash` (non-volatile storage required by the Wi-Fi driver)
    *   `esp_partition` (raw access to the `history` partition)
    *   `esp_http_server` (web page and JSON/CSV endpoints)
    *   `console` (serial console with the `log` command)
*   **Project modules:**
    *   `bme280.h` / `bme280.cpp` (driver for the BME280 sensor, using the Bosch datasheet compensation formulas)
    *   `ssd1306.h` / `ssd1306.cpp` (driver for the SSD1306 OLED, with a local frame buffer)
    *   `font5x7.h` (5x7 font for text output, including a degree sign)
    *   `net_time.h` / `net_time.cpp` (Wi-Fi connection with automatic reconnection and signal level, and NTP time synchronization)
    *   `telemetry.h` / `telemetry.cpp` (MQTT client and JSON payload)
    *   `history.h` / `history.cpp` (measurement history ring buffer in flash and the `log` console command)
    *   `health.h` / `health.cpp` (health log, `sys` console command and network watchdog)
    *   `charts.h` / `charts.cpp` (chart tables in RAM for the web page, fed by every measurement)
    *   `web_server.h` / `web_server.cpp`, `web_page.html` (HTTP server and the page embedded in the firmware)
    *   `config.h` (central configuration)
    *   `Kconfig.projbuild` ("Weather Station" menu in `idf.py menuconfig` for the measurement interval, the Wi-Fi credentials, the MQTT broker and the history log interval)
    *   `main.cpp` (initialization, the serial console, the BOOT button and the display power control, the data for the web page and the FreeRTOS tasks `taskMeasure`, `taskDisplay`, `taskPublish` and `taskLog`)

## 6. Interfaces

### 6.1 User Interface (OLED)
*   Display of temperature, humidity, and air pressure.
*   Date and time in the header once the clock is synchronized.
*   Wi-Fi signal icon at the right edge of the header.
*   Measurement icon (filled dot) at the left edge of the header, shown for 0.5 s after each measurement.
*   [v1.8.0] The BOOT button switches the display off and on; the display switches off after 5 minutes without a button press by default (see FR-9.1, FR-9.2).

### 6.2 Hardware Interfaces
*   **I2C bus:** Uses GPIO 21 (SDA) and GPIO 22 (SCL) of the ESP32.
*   **BOOT button:** [v1.8.0] GPIO 0 (active low), read by interrupt after start-up to switch the display. Held during a reset, it still selects the download mode of the ESP32.
*   **Power supply:** 3.3V for the ESP32, BME280, and SSD1306.

### 6.3 Serial Interface
*   The serial interface is used for debug output at 115200 baud (ESP-IDF console UART, viewed with `idf.py monitor`).
*   [v1.6.0] Interactive console on the same UART (prompt `weather> `): `help`, `log info`, `log dump [N]`, `log clear` (see FR-7.4, FR-7.5).
*   [v1.8.0] Console commands `display on`, `display off`, `display status` (see FR-9.1).
*   [v1.8.2] Console command `sys`: system health (see NFR-2.4).

### 6.4 Network Interface
*   **Wi-Fi:** 2.4 GHz station mode (WPA2-PSK, or an open network if no password is set).
*   **Wi-Fi:** [v1.2.0] Add a symbol to indicate WiFi connectivity in the yellow area (see FR-5.7).
*   **Wi-Fi power save:** [v1.8.4] Off by default (`WIFI_POWER_SAVE` in `main/config.h`). With modem power save on (the ESP-IDF default up to v1.8.3), the station stopped receiving with some routers while it could still send; turning it on saves energy, which matters for a battery supply (section 3.9).
*   **Wi-Fi access point and link:** [v1.8.7] The station scans all channels and joins the access point with the strongest signal (a mesh offers the same network from several nodes; before v1.8.7 it joined the first one found). Each association is logged with network name, access point (BSSID), channel, channel width (HT20/HT40), security and signal. ~~A-MPDU aggregation on reception is off by default (`sdkconfig.defaults`): the station receives little data, and a mesh access point kept tearing the Block Ack sessions down.~~ [CHANGED v1.8.8] A-MPDU aggregation is on in both directions (ESP-IDF default). v1.8.7 turned it off on reception because the `DELBA reason:37, initiator:0(recipient)` messages were misread: they came from the access point as recipient, so they concerned the station's transmission, and they are harmless. Opening and closing Block Ack sessions (`ba-add`, `ba-del`, `DELBA reason:39` after inactivity) is routine; since v1.8.8 these Wi-Fi driver messages are no longer logged (driver log level WARN, `main/config.h`), while the association line above still is. [v1.8.9] Each disconnection is logged with the network, access point (BSSID), signal and the reason with its meaning (for example `reason 2, access point did not answer authentication`), so a failing access point can be identified without the driver messages. A signal of about -60 dBm or better is needed for a responsive web page; at -65 to -75 dBm the link loses packets and answers can take seconds.
*   **NTP:** SNTP client (UDP port 123), default server `pool.ntp.org`.
*   **MQTT:** [v1.5.0] Client publishing the weather data as JSON to the configured broker (TCP 1883 for `mqtt://`, 8883 for `mqtts://`), topic `weather-station/data` (see FR-6.1 to FR-6.6). [v1.8.4] Client ID `weather-station-` plus the last 3 bytes of the Wi-Fi MAC address (e.g. `weather-station-51a842`), shown when the client connects and by `sys`; before v1.8.4 the ESP-MQTT default `ESP32_` plus the same bytes.
*   **HTTP:** [v1.7.0] Server on TCP port 80: `/`, `/api/current`, `/api/history?hours=N&points=P`, `/history.csv`, and `/favicon.ico` (empty answer, since v1.8.1) (see FR-8.1 to FR-8.6).

### 6.5 Web Interface
*   [v1.7.0] Page at `http://<ip>/` with the current values and 24-hour charts, and a link to download the history as CSV (see FR-8.1 to FR-8.6).

## 7. Open Issues / Future Extensions

*   **Calibration:** Provide a way to calibrate the sensor data.
*   **Hardware RTC:** Add an RTC module (e.g. DS3231) so the time of day survives power loss without Wi-Fi (the time of day via NTP is implemented in v1.1.0).
