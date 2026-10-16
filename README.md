# ESP32 Weather Station

An ESP32 weather station that reads **temperature**, **humidity** and **air pressure** from a BME280 sensor and shows the values in real time on the 0.96" SSD1306 OLED display built into the ideaspark ESP32 development board. The date and time are fetched over Wi-Fi from an NTP server, and the readings can be sent to an MQTT broker.

Written in C++ with **ESP-IDF v6.1** and **FreeRTOS**. The BME280 and SSD1306 drivers are implemented directly on top of the ESP-IDF I2C driver; the only external component is Espressif's MQTT client (`espressif/mqtt`), which the build downloads automatically from the ESP Component Registry.

What the OLED display shows (128x64):

```
+--------------------------------+
| *   15/10/2026 14:32      ((o  |  <- yellow band: measurement dot, date/time, Wi-Fi icon
+--------------------------------+
| TEMP              23.4  oC     |
| HUM               56.1  %RH    |
| PRES            1012.8  hPa    |
+--------------------------------+
```

The dot at the left lights up for 0.5 s each time a new measurement arrives. The Wi-Fi icon shows 1 to 3 arcs according to the signal strength and an X while disconnected. Before the clock is synchronized, the header shows "WEATHER STATION".

## Hardware

| Component | Description |
|---|---|
| Board | ideaspark ESP32 development board with built-in 0.96" SSD1306 OLED (128x64, I2C) |
| Module | ESP-WROOM-32 (ESP32, Wi-Fi + Bluetooth) |
| Sensor | BME280 (temperature, humidity, pressure), I2C |
| Power supply | 3.3 V |

### Wiring

The BME280 shares the I2C bus with the board's display:

| BME280 | ESP32 |
|---|---|
| VCC | 3V3 |
| GND | GND |
| SDA | GPIO 21 |
| SCL | GPIO 22 |

Default I2C addresses: BME280 at `0x76` (SDO to GND; use `0x77` if SDO is tied to VCC) and SSD1306 at `0x3C`. The full board pinout is in [pinout.png](pinout.png).

The board's blue LED (GPIO 2) blinks when the system enters error mode.

## Building and flashing

Prerequisite: [ESP-IDF v6.1](https://docs.espressif.com/projects/esp-idf/) installed.

```bash
# 1. Activate the ESP-IDF environment
source ~/.espressif/tools/activate_idf_v6.1.sh

# 2. Set the target (first time only)
idf.py set-target esp32
# 3. Set the Wi-Fi credentials and, optionally, the measurement interval and the
#    MQTT broker (first time only): "Weather Station" menu
idf.py menuconfig

# 4. Build
idf.py build

# 5. Flash and open the serial monitor (adjust the port)
idf.py -p /dev/ttyUSB0 flash monitor
```

The serial monitor runs at 115200 baud and prints the readings, for example:

```
I (1234) weather: 2026-10-15 14:32:05  T=23.41 C  H=56.12 %RH  P=1012.84 hPa  Alt~3 m
```

The timestamp appears once the clock has been synchronized via NTP. The Wi-Fi credentials are stored only in the local `sdkconfig`, which is not committed. With no SSID configured, the station works normally without the date and time.

> **VS Code with the ESP-IDF extension:** turn off the setting `idf.enableUpdateSrcsToCMakeListsFile`. When it is on, the extension inserts every new `.c`/`.cpp` file into the nearest `CMakeLists.txt`, including the files of the downloaded MQTT component, and the build fails.

## Sending data via MQTT

With a broker URI set in menuconfig (for example `mqtt://192.168.1.10:1883`), the station publishes the latest reading every 60 s (configurable) to the topic `weather-station/data`, as JSON with the retain flag:

```json
{"time":1792161125,"temperature_c":23.41,"humidity_rh":56.12,"pressure_hpa":1012.84}
```

`time` is the Unix time of the reading and appears once the clock has been synchronized. To watch the messages: `mosquitto_sub -h 192.168.1.10 -t weather-station/data -v`. With an empty URI, or without Wi-Fi, nothing is sent; broker failures never stop the station.

## Configuration

The measurement interval (1 to 3600 s, default 2 s), the Wi-Fi credentials, and the MQTT broker URI, user, password and publish interval (1 to 86400 s, default 60 s) are set with `idf.py menuconfig` in the "Weather Station" menu.

All other tunable parameters live in [main/config.h](main/config.h):

- I2C pins, frequency and port;
- BME280 and SSD1306 addresses;
- display size (128x64 or 128x32), contrast and yellow band;
- sea-level pressure used for the altitude estimate;
- task periods, stack sizes and priorities;
- NTP server, time zone (default UTC-3), Wi-Fi retry interval and signal icon thresholds;
- MQTT topic, QoS, retain flag and reconnect interval.

## Architecture

The firmware runs three FreeRTOS tasks:

- **`taskMeasure`**: reads the BME280 at the measurement interval (2 s by default).
- **`taskDisplay`**: refreshes the display every 1 s, and right away when a new measurement arrives, with a measurement dot, the date and time and a Wi-Fi signal icon in the header.
- **`taskPublish`**: sends the latest reading to the MQTT broker at the publish interval (60 s by default), only when the broker is configured.

Wi-Fi and NTP run in the background (`net_time`), as does the MQTT client (`telemetry`); all of them reconnect on their own and network failures never stop the station.

The `i2cMutex` mutex controls access to the I2C bus, and `dataMutex` protects the data shared between the tasks. After 3 consecutive failures, the device is re-initialized automatically. If the display or the sensor fails to start at boot, the system halts and signals the error through the LED and the serial console.

```
main/
├── main.cpp      # app_main, initialization and FreeRTOS tasks
├── config.h      # central configuration
├── bme280.*      # BME280 driver (Bosch compensation formulas)
├── ssd1306.*     # SSD1306 driver with a local frame buffer
├── font5x7.h     # 5x7 font (ASCII + degree sign)
├── net_time.*    # Wi-Fi station + NTP time of day
├── telemetry.*   # MQTT client, JSON payload
├── idf_component.yml  # external component: espressif/mqtt
└── Kconfig.projbuild  # menuconfig menu: measurement interval, Wi-Fi, MQTT broker
```

The full functional specification is in [esp32-weather-station.md](esp32-weather-station.md).

## Roadmap

- Log history to an SD card or the internal flash
- Local web server to view the readings
- Sensor calibration
- Hardware RTC module so the time survives power loss without Wi-Fi
