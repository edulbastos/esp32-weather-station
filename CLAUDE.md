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

## Specification and Release Workflow

The functional specification is `esp32-weather-station.md`. It is the only place where requirements are added. Specification and firmware share one version number (SemVer), and each release is an annotated git tag `vX.Y.Z`.

**Version bump rules:**
*   New functional requirement → MINOR (1.0.0 → 1.1.0)
*   Fix, clarification, or refactoring without a new requirement → PATCH (1.1.0 → 1.1.1)
*   Incompatible change (hardware, pinout, removed functionality) → MAJOR

**When asked to process the proposed requirements:**
1.  Find every `[PROPOSED]` marker in the specification. If there is more than one, ask whether to release them together or one version each.
2.  Compute the new version from the latest tag (`git describe --tags --abbrev=0`).
3.  Implement the code, following the conventions above.
4.  Update the specification:
    *   Replace `[PROPOSED]` with `[vX.Y.Z]`.
    *   Update the **Version** field.
    *   Add a row to the Revision History.
    *   Update the affected sections (architecture, interfaces).
    *   Remove items from section 7 that are now implemented.
5.  Update `CLAUDE.md` and `README.md` if the structure or usage changed.
6.  Run `idf.py build` and fix any errors.
7.  Stop and report what changed. Do not commit yet; the user tests on the board first.
8.  After the user approves:
    *   Set the release date in the Revision History.
    *   Commit with the message `Release vX.Y.Z: <summary>`.
    *   Create an annotated tag: `git tag -a vX.Y.Z -m "<summary>"`.
    *   Do not push unless asked.

Older requirements and texts are never deleted, only marked; history lives in the git tags.
