# Functional Specification: ESP32 Weather Station

**Version:** 1.4.0

## Document Control

### Revision History

| Version | Date       | Changes                                   |
|---------|------------|-------------------------------------------|
| 1.0.0   | 2026-10-15 | Initial specification and implementation. |
| 1.1.0   | 2026-10-15 | Time of day via Wi-Fi + NTP (FR-5.1 to FR-5.6). |
| 1.2.0   | 2026-10-15 | Wi-Fi signal icon in the OLED header (FR-5.7). |
| 1.3.0   | 2026-10-15 | Measurement icon in the OLED header (FR-2.6). |
| 1.4.0   | 2026-10-15 | Measurement interval set via menuconfig (FR-3.2, FR-1.4 changed). |

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

## 4. Non-Functional Requirements

### 4.1 Performance
*   **NFR-1.1:** Sensor data acquisition and display updates MUST run in parallel and without noticeable delay.
*   **NFR-1.2:** The use of FreeRTOS MUST ensure efficient resource management.

### 4.2 Reliability
*   **NFR-2.1:** The system MUST operate without interruption over extended periods (weeks/months).
*   **NFR-2.2:** I2C communication errors SHOULD be handled robustly to prevent system crashes.

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

### 5.2 Software Components
*   **Development framework:** ESP-IDF v6.1 (CMake / `idf.py` build system)
*   **Real-time operating system:** FreeRTOS (part of ESP-IDF)
*   **Programming language:** C++ (entry point `app_main`)
*   **ESP-IDF components:**
    *   `esp_driver_i2c` (`driver/i2c_master.h`, I2C master communication)
    *   `esp_driver_gpio` (GPIO configuration)
    *   `esp_log` (serial debug output)
    *   `esp_wifi`, `esp_netif`, `esp_event`, `lwip` (Wi-Fi station and TCP/IP stack)
    *   `esp_netif_sntp` (NTP time synchronization, part of `esp_netif`)
    *   `nvs_flash` (non-volatile storage required by the Wi-Fi driver)
*   **Project modules:**
    *   `bme280.h` / `bme280.cpp` (driver for the BME280 sensor, using the Bosch datasheet compensation formulas)
    *   `ssd1306.h` / `ssd1306.cpp` (driver for the SSD1306 OLED, with a local frame buffer)
    *   `font5x7.h` (5x7 font for text output, including a degree sign)
    *   `net_time.h` / `net_time.cpp` (Wi-Fi connection with automatic reconnection and signal level, and NTP time synchronization)
    *   `config.h` (central configuration)
    *   `Kconfig.projbuild` ("Weather Station" menu in `idf.py menuconfig` for the measurement interval and the Wi-Fi credentials)
    *   `main.cpp` (initialization and the FreeRTOS tasks `taskMeasure` and `taskDisplay`)

## 6. Interfaces

### 6.1 User Interface (OLED)
*   Display of temperature, humidity, and air pressure.
*   Date and time in the header once the clock is synchronized.
*   Wi-Fi signal icon at the right edge of the header.
*   Measurement icon (filled dot) at the left edge of the header, shown for 0.5 s after each measurement.

### 6.2 Hardware Interfaces
*   **I2C bus:** Uses GPIO 21 (SDA) and GPIO 22 (SCL) of the ESP32.
*   **Power supply:** 3.3V for the ESP32, BME280, and SSD1306.

### 6.3 Serial Interface
*   The serial interface is used for debug output at 115200 baud (ESP-IDF console UART, viewed with `idf.py monitor`).

### 6.4 Network Interface
*   **Wi-Fi:** 2.4 GHz station mode (WPA2-PSK, or an open network if no password is set).
*   **Wi-Fi:** [v1.2.0] Add a symbol to indicate WiFi connectivity in the yellow area (see FR-5.7).
*   **NTP:** SNTP client (UDP port 123), default server `pool.ntp.org`.

## 7. Open Issues / Future Extensions

*   **Wi-Fi connectivity:** Add a feature to transmit weather data to a server or cloud service.
*   **Data logging:** Store historical weather data on an SD card or in the ESP32's internal flash memory.
*   **Web server:** Implement a simple web server on the ESP32 to display the data on the local network.
*   **Calibration:** Provide a way to calibrate the sensor data.
*   **Hardware RTC:** Add an RTC module (e.g. DS3231) so the time of day survives power loss without Wi-Fi (the time of day via NTP is implemented in v1.1.0).
