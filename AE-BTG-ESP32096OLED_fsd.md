# Functional Specification: ESP32 Weather Station

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
*   **FR-1.4:** The sensor data MUST be updated at least every 2 seconds.
*   **FR-1.5:** The system MUST provide a means to safely synchronize access to the I2C bus between different tasks (FreeRTOS mutex `i2cMutex`).

### 3.2 Data Display on OLED
*   **FR-2.1:** The system MUST display the current temperature on the SSD1306 OLED display.
*   **FR-2.2:** The system MUST display the current humidity on the SSD1306 OLED display.
*   **FR-2.3:** The system MUST display the current air pressure on the SSD1306 OLED display.
*   **FR-2.4:** The OLED display MUST be refreshed at least once every second.
*   **FR-2.5:** The displayed values MUST be clearly legible and include the corresponding units (e.g. °C, %RH, hPa).

### 3.3 Configuration
*   **FR-3.1:** Configuration parameters such as display dimensions, I2C pins and addresses of the sensors and displays, and sea-level pressure MUST be defined centrally in a dedicated header file (`main/config.h`).

### 3.4 Error Handling
*   **FR-4.1:** The system MUST output an error message on the serial console (via `ESP_LOGE`) if the OLED display cannot be initialized.
*   **FR-4.2:** The system MUST output an error message on the serial console (via `ESP_LOGE`) if the BME280 sensor cannot be initialized.
*   **FR-4.3:** In the event of a critical initialization error (display or sensor), the system MUST remain in an error mode or stop operating.

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
*   **Microcontroller:** ESP32 (ESP32 Dev Module)
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
*   **Project modules:**
    *   `bme280.h` / `bme280.cpp` (driver for the BME280 sensor, using the Bosch datasheet compensation formulas)
    *   `ssd1306.h` / `ssd1306.cpp` (driver for the SSD1306 OLED, with a local frame buffer)
    *   `font5x7.h` (5x7 font for text output, including a degree sign)
    *   `config.h` (central configuration)
    *   `main.cpp` (initialization and the FreeRTOS tasks `taskMeasure` and `taskDisplay`)

## 6. Interfaces

### 6.1 User Interface (OLED)
*   Display of temperature, humidity, and air pressure.

### 6.2 Hardware Interfaces
*   **I2C bus:** Uses GPIO 21 (SDA) and GPIO 22 (SCL) of the ESP32.
*   **Power supply:** 3.3V for the ESP32, BME280, and SSD1306.

### 6.3 Serial Interface
*   The serial interface is used for debug output at 115200 baud (ESP-IDF console UART, viewed with `idf.py monitor`).

## 7. Open Issues / Future Extensions

*   **Wi-Fi connectivity:** Add a feature to transmit weather data to a server or cloud service.
*   **Data logging:** Store historical weather data on an SD card or in the ESP32's internal flash memory.
*   **Web server:** Implement a simple web server on the ESP32 to display the data on the local network.
*   **Calibration:** Provide a way to calibrate the sensor data.
*   **Time of day:** Integrate a real-time clock (RTC) to timestamp the data.
