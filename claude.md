# Project: Weather Station

## Project Overview

This project is a weather station built with an ESP32 and a BME280 sensor. The weather data (temperature, humidity, air pressure) is shown on an SSD1306 OLED display.

The project is built with ESP-IDF (v6.1) and uses FreeRTOS to run sensor measurement and display updates in separate tasks. This allows for a modular and extensible software architecture.

**Main components:**

*   **ideaspark ESP32 0.96" OLED board (ESP-WROOM-32):** Microcontroller board with the SSD1306 display built in
*   **BME280:** Sensor for temperature, humidity, and air pressure
*   **SSD1306:** OLED display (128x64, I2C)

## Project Structure

*   `CMakeLists.txt`: ESP-IDF project file.
*   `sdkconfig.defaults`: Default configuration (target `esp32`, console at 115200 baud).
*   `main/config.h`: Central configuration (I2C pins and addresses, display size, sea-level pressure, task periods, stack sizes and priorities).
*   `main/bme280.h`, `main/bme280.cpp`: BME280 driver with the Bosch datasheet compensation formulas.
*   `main/ssd1306.h`, `main/ssd1306.cpp`: SSD1306 driver with a local frame buffer.
*   `main/font5x7.h`: 5x7 font (ASCII plus a degree sign at `0x7F`).
*   `main/main.cpp`: `app_main`, initialization, and the FreeRTOS tasks.

## Build and Run Process

**Dependencies:**

No external libraries are required. The drivers use only ESP-IDF components:

*   `esp_driver_i2c` (`driver/i2c_master.h`): I2C master driver
*   `esp_driver_gpio`
*   FreeRTOS (part of ESP-IDF)

The Arduino libraries (`Wire.h`, `Adafruit_GFX.h`, `Adafruit_SSD1306.h`, `Adafruit_BME280.h`) are not used.

**Steps:**

1.  Activate the ESP-IDF environment:
    `source ~/.espressif/tools/activate_idf_v6.1.sh`
2.  Set the target (only needed once): `idf.py set-target esp32`
3.  Build: `idf.py build`
4.  Flash and open the serial monitor (adjust the port):
    `idf.py -p /dev/ttyUSB0 flash monitor`

## Development Conventions

*   The code is written in C++; the entry point is `extern "C" void app_main(void)`.
*   All configurable values belong in `main/config.h`; do not hard-code them elsewhere.
*   Use `ESP_LOGI`/`ESP_LOGW`/`ESP_LOGE` for serial output and return `esp_err_t` from driver functions.

The code is divided into two main tasks:

*   **`taskMeasure`:** Measures the weather data from the BME280 sensor every 2 seconds.
*   **`taskDisplay`:** Updates the OLED display with the current weather data every 1 second.

The two tasks are synchronized via a mutex (`i2cMutex`) to control access to the I2C bus. A second mutex (`dataMutex`) protects the shared measurement data.
