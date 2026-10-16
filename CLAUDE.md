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
*   `sdkconfig.defaults`: Default configuration (target `esp32`, console at 115200 baud, custom partition table).
*   `partitions.csv`: Flash layout (8 MB): `nvs`, `phy_init`, app `factory` (2 MB) and the `history` data partition (5 MB).
*   `main/config.h`: Central configuration (I2C pins and addresses, display size, sea-level pressure, task periods, stack sizes and priorities).
*   `main/bme280.h`, `main/bme280.cpp`: BME280 driver with the Bosch datasheet compensation formulas.
*   `main/ssd1306.h`, `main/ssd1306.cpp`: SSD1306 driver with a local frame buffer.
*   `main/font5x7.h`: 5x7 font (ASCII plus a degree sign at `0x7F`).
*   `main/net_time.h`, `main/net_time.cpp`: Wi-Fi station with automatic reconnection, signal level for the header icon, and NTP time sync (time of day).
*   `main/telemetry.h`, `main/telemetry.cpp`: MQTT client (connects once Wi-Fi has an IP, reconnects on its own) and the JSON payload.
*   `main/history.h`, `main/history.cpp`: Measurement history as a ring buffer of 16-byte records with CRC16 on the `history` partition, and the `log` console command (`info`, `dump [N]`, `clear`).
*   `main/web_server.h`, `main/web_server.cpp`: HTTP server (`esp_http_server`) started on the first IP address: `/`, `/api/current`, `/api/history?hours=N`, `/history.csv`. Large answers are sent in chunks.
*   `main/web_page.html`: The web page (HTML, CSS and JavaScript in one file, no external resources), embedded in the firmware with `EMBED_FILES`.
*   `main/idf_component.yml`: external component `espressif/mqtt` from the ESP Component Registry (downloaded into `managed_components/`, not committed).
*   `main/Kconfig.projbuild`: "Weather Station" menuconfig menu for the measurement interval, the Wi-Fi credentials, the MQTT broker and publish interval, and the history log interval.
*   `main/main.cpp`: `app_main`, initialization, and the FreeRTOS tasks.

## Build and Run Process

**Dependencies:**

The only external component is `espressif/mqtt` (ESP-MQTT), which is no longer part of ESP-IDF since v6.0; the build downloads it automatically from the ESP Component Registry. Everything else uses ESP-IDF components:

*   `esp_driver_i2c` (`driver/i2c_master.h`): I2C master driver
*   `esp_driver_gpio`
*   `esp_wifi`, `esp_netif` (including SNTP), `esp_event`, `lwip`, `nvs_flash`
*   `esp_partition` (history partition), `console` (serial console), `esp_http_server` (web page)
*   FreeRTOS (part of ESP-IDF)

In VS Code, keep the ESP-IDF extension setting `idf.enableUpdateSrcsToCMakeListsFile` off: when on, it inserts every new source file into the nearest `CMakeLists.txt`, including those of `managed_components/`, and breaks the build.

The Arduino libraries (`Wire.h`, `Adafruit_GFX.h`, `Adafruit_SSD1306.h`, `Adafruit_BME280.h`) are not used.

**Steps:**

1.  Activate the ESP-IDF environment:
    `source ~/.espressif/tools/activate_idf_v6.1.sh`
2.  Set the target (only needed once): `idf.py set-target esp32`
3.  Set the Wi-Fi credentials and the MQTT broker (only needed once): `idf.py menuconfig` → "Weather Station". They are stored in the local `sdkconfig`, which is not committed.
4.  Build: `idf.py build`
5.  Flash and open the serial monitor (adjust the port):
    `idf.py -p /dev/ttyUSB0 flash monitor`

## Development Conventions

*   The code is written in C++; the entry point is `extern "C" void app_main(void)`.
*   All configurable values belong in `main/config.h`; do not hard-code them elsewhere. Exceptions come from Kconfig (`idf.py menuconfig`) and are read into `config.h`: the measurement, MQTT publish and history log intervals, and per-installation settings and secrets (Wi-Fi credentials, MQTT broker URI and credentials), which are never committed.
*   Use `ESP_LOGI`/`ESP_LOGW`/`ESP_LOGE` for serial output and return `esp_err_t` from driver functions.

The code is divided into four tasks:

*   **`taskMeasure`:** Measures the weather data from the BME280 sensor at the measurement interval (`CONFIG_WS_MEASURE_PERIOD_S`, default 2 seconds).
*   **`taskDisplay`:** Updates the OLED display with the current weather data every 1 second. `taskMeasure` also wakes it with a task notification on each new measurement, so the measurement icon appears immediately and is cleared after `MEASURE_ICON_MS`.
*   **`taskPublish`:** Sends the latest valid measurement to the MQTT broker at the publish interval (`CONFIG_WS_MQTT_PUBLISH_PERIOD_S`, default 60 seconds). Created only when Wi-Fi and a broker URI are configured.
*   **`taskLog`:** Stores the latest valid measurement in the flash history at the log interval (`CONFIG_WS_LOG_PERIOD_S`, default 60 seconds).

The HTTP server runs in its own ESP-IDF task and reads the current data through `current_for_web()` in `main.cpp` (under `dataMutex`) and the history through `history::visit()`.

The ESP-IDF console REPL runs on the serial port (prompt `weather> `) with the `help` and `log` commands; `log dump` silences other log output while it prints.

The tasks are synchronized via a mutex (`i2cMutex`) to control access to the I2C bus. A second mutex (`dataMutex`) protects the shared measurement data.

Wi-Fi and SNTP (`net_time`) and the MQTT client (`telemetry`) run in the background, driven by ESP-IDF events; network failures are never critical errors.

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
