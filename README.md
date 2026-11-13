# ESP32 Weather Station

An ESP32 weather station that reads **temperature**, **humidity** and **air pressure** from a BME280 sensor and shows the values in real time on the 0.96" SSD1306 OLED display built into the ideaspark ESP32 development board. The date and time are fetched over Wi-Fi from an NTP server, the readings can be sent to an MQTT broker, a history of the readings is kept in the internal flash, and a web page shows everything on the local network.

Written in C++ with **ESP-IDF v6.1** and **FreeRTOS**. The BME280 and SSD1306 drivers are implemented directly on top of the ESP-IDF I2C driver; the only external component is Espressif's MQTT client (`espressif/mqtt`), which the build downloads automatically from the ESP Component Registry.

<p align="center">
  <img src="images/station.jpeg" alt="The weather station: ideaspark ESP32 board with the BME280 wired to it, the OLED showing date, time, Wi-Fi signal, temperature, humidity and pressure" width="250" height="480">
  &nbsp;&nbsp;
  <img src="images/picture2.jpeg" alt="The web page of the station in a desktop browser: current values, Wi-Fi signal in dBm, the 24-hour temperature, humidity and pressure charts with the shared cursor, and the firmware version and heap in the footer" width="485" height="480">
</p>
<p align="center"><em>The station with the BME280 wired to the board (left) and its web page in a browser, 24-hour view (right).</em></p>

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

## Display on/off (energy saving)

The **BOOT** button of the board switches the display off and on (holding it for 5 s opens [setup mode](#setup-from-a-phone) instead). The display also switches off by itself after 5 minutes without a button press; pressing the button wakes it up. The time is set in `idf.py menuconfig` ("Display switch-off after inactivity", 0 = never). While the display is off, the station keeps measuring, logging and publishing.

In the serial monitor: `display on`, `display off`, `display status`.

The red **PWR** LED is wired straight to the 3.3 V supply and cannot be switched off by the firmware.

## Setup from a phone

The Wi-Fi network, the MQTT broker, the station name, the altitude and the METAR airport can be set without a computer:

1. **Hold the BOOT button for 5 seconds.** The station restarts in setup mode; a new station without a Wi-Fi network starts in it by itself. The display shows `SETUP`, the name of the station's own Wi-Fi network (`ws-setup-` plus 4 hex digits) and its password, which is new every time.
2. **Join that network with the phone.** Most phones then open the setup page by themselves; otherwise open `http://192.168.4.1/`. If the phone warns that the network has no internet, stay connected.
3. **Pick your Wi-Fi network** from the list (or type a hidden one), enter its password and the other settings, and tap **Test and save**. Fields you leave as they are keep their values; an empty password field keeps the stored password.
4. The station first joins your network to test it. If the password is wrong or the network is not found, the page says so and **nothing is stored**. Otherwise it also tries the MQTT broker, stores the settings (even when the broker cannot be reached right now), shows the address of its web page and restarts. Reconnect the phone to your usual network.

Setup mode ends without changes after another 5-second press of BOOT, with `setup cancel`, or after 15 minutes without a phone connected (a station without a Wi-Fi network stays in it). Measuring and the display continue while it runs; MQTT and the web page are off.

On the serial console: `setup` (enter setup mode), `setup cancel`, `setup show` (the settings, passwords masked) and `setup clear` (erase the stored settings; the menuconfig values apply again). The stored settings replace all the menuconfig values of the list above; `sys` shows which are in use (`Settings: stored` or `menuconfig`). They are kept in NVS without encryption, so anyone who can read the board's flash can read them.

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
# 3. Optionally set the Wi-Fi credentials, the station name, the altitude, the
#    METAR airport and the MQTT broker as factory defaults (they can also be set
#    later from a phone, see "Setup from a phone"), and the measurement and
#    history intervals (first time only): "Weather Station" menu
idf.py menuconfig

# 4. Build
idf.py build

# 5. Flash and open the serial monitor (adjust the port)
idf.py -p /dev/ttyUSB0 flash monitor
```

The serial monitor runs at 115200 baud and prints the readings, for example:

```
I (1234) weather: 2026-10-15 14:32:05  T=23.41 C  H=56.12 %RH  Td=14.16 C  P=930.12 hPa  QNH=1016.78 hPa
```

The timestamp appears once the clock has been synchronized via NTP. Wi-Fi credentials set in menuconfig are stored only in the local `sdkconfig`, which is not committed. With no SSID configured, the station starts in setup mode (see [Setup from a phone](#setup-from-a-phone)) and keeps measuring.

> **VS Code with the ESP-IDF extension:** turn off the setting `idf.enableUpdateSrcsToCMakeListsFile`. When it is on, the extension inserts every new `.c`/`.cpp` file into the nearest `CMakeLists.txt`, including the files of the downloaded MQTT component, and the build fails.

## Identification

Every station and every sensor has an identifier, so a management system can tell them apart and keep the data of several stations together:

- **Device:** `ws-` followed by the board's MAC address, for example `ws-a0b76551a842`. It never changes, not even after reflashing.
- **Sensor:** the type followed by a checksum of the chip's factory calibration data, for example `bme280-3fa91c07`. The BME280 has no serial number, but its calibration data differ from chip to chip, so a replaced sensor gets a new identifier; the station notices the replacement and logs it (`Sensor at position 0 (i2c@0x76) replaced: ...`).
- **Name (optional):** a free-text name such as "Varanda", set on the setup page or in menuconfig ("Station name"). It is shown after the heading of the web page ("Weather Station · Varanda"), in the tab title, in `sys` and in the MQTT info message, but never used as a key.

The identifiers appear in the boot log, in `sys`, on the web page, in every MQTT message and in every line of the history CSV.

## Sending data via MQTT

With a broker URI set on the setup page or in menuconfig (for example `mqtt://192.168.1.10:1883`), the station publishes the latest reading every 60 s (configurable) to its own topic `weather-station/<device_id>/data`, as JSON with QoS 1 (the broker confirms each message; unconfirmed ones are sent again after a reconnection) and the retain flag:

```json
{"device_id":"ws-a0b76551a842","sensor_id":"bme280-3fa91c07","time":1792161125,"temperature_c":23.41,"humidity_rh":56.12,"pressure_hpa":930.12,"pressure_msl_hpa":1016.78,"dew_point_c":14.16,"rssi_dbm":-67}
```

`time` is the Unix time of the reading and appears once the clock has been synchronized. `pressure_hpa` is the pressure measured at the station, `pressure_msl_hpa` the same reduced to sea level, and `dew_point_c` the dew point (see [Measurement quality](#measurement-quality)). `rssi_dbm` is the weakest Wi-Fi signal measured since the previous message (sampled with every measurement), so a consumer can follow the quality of the link over time; it is not stored in the flash history. On every connection, and after a sensor replacement, the station also publishes a retained message to `weather-station/<device_id>/info` with its name, firmware version, intervals and sensors (with channels, units and resolution); a management system finds all stations by subscribing to `weather-station/+/info`. The topic `weather-station/<device_id>/status` says whether the station is reachable: `online` while it is connected, and `offline`, which the broker publishes by itself about 90 s after the station stops answering (MQTT last will). To watch everything: `mosquitto_sub -h 192.168.1.10 -t 'weather-station/#' -v`. With an empty URI, or without Wi-Fi, nothing is sent; broker failures never stop the station.

> **Updating from v1.x:** up to v1.9.1 the readings went to `weather-station/data`, without identifiers. Subscribers must move to `weather-station/<device_id>/data` (or `weather-station/+/data`). The station clears the old retained message on that topic.

## Web page

Once Wi-Fi is connected, the station serves a page on the local network. Its address appears in the serial monitor:

```
I (5321) web: Web page at http://192.168.1.42/
```

The page shows the current values (refreshed at every measurement) and charts from the flash history over the last hour, 24 hours or 7 days (buttons above the charts), and has a link to download the whole history as CSV. Each point of a chart is an average: of 1 minute over 1 hour (60 points), of 15 minutes over 24 hours (96 points) and of 2 hours over 7 days (84 points); few points keep the answer small, which matters on a weak Wi-Fi link. The station keeps the chart data of the three periods ready in RAM, updated at every measurement, so it answers in a few milliseconds whatever the measurement interval; the page loads all three periods when it opens, so switching between them is instant, and then only adds the new measurements, so it stays light even when left open; after the page was in the background (or the computer asleep) it reloads the data, so the lines stay continuous; the CSV keeps every single measurement. Point at a chart with the mouse (or touch it on a phone) to see the time and the values of that moment in all three charts at once. At the top right, the status line ends with the Wi-Fi signal bars, and the line below shows the signal in dBm (for example `-67 dBm`), refreshed at every measurement, which helps to find a good place for the station. The footer shows, on the right of the CSV download link, the firmware version and the heap memory (free now, lowest since the start, largest free block), the same figures as `sys`, so the station can be checked without the serial console. It needs no internet access and works on phones. Tip: reserve the station's IP in your router (DHCP reservation) so the address does not change.

| Address | Content |
|---|---|
| `/` | the page |
| `/api/current` | current values (with sea-level pressure and dew point), identifiers, calibration, METAR check, Wi-Fi signal (`rssi_dbm`), firmware version and heap as JSON |
| `/api/info` | device, firmware, intervals and sensors as JSON (same as the MQTT info message) |
| `/api/charts` | the three chart periods of the page (1 h, 24 h, 7 d) in one answer, as JSON |
| `/api/history?hours=24&points=96` | history of the last N hours as JSON, averaged into P points (defaults: 24 hours; the chart's number of points for 1, 24 and 168 hours, else 150) |
| `/history.csv` | the whole history as CSV, named `history-<device_id>.csv` (see below) |

**Downloading the CSV:** the station sends the file gzip-compressed (about 5 times less data over Wi-Fi) to clients that ask for it, and they unpack it while saving, so the saved file is always the plain CSV. Browsers do this by themselves. With curl, `--compressed` asks for the compressed transfer and unpacks it; without it the file comes uncompressed, with the same content:

```bash
curl --compressed -o history.csv http://<ip>/history.csv   # faster
curl -OJ --compressed http://<ip>/history.csv              # keeps the name history-<device_id>.csv
```

The page has no password and is read-only; anyone on the local network can see it. While the full CSV is downloading, the page pauses its updates (the server handles one request at a time).

## Measurement quality

**Valid range.** A reading outside the operating range of the BME280 (−40 to 85 °C, 0 to 100 %RH, 300 to 1100 hPa) is impossible and comes from a disturbance on the I2C bus or a damaged chip. It is rejected like a failed read: the display keeps showing the previous reading, and nothing new is sent, stored or added to the charts; after 3 in a row the sensor is re-initialized and the display shows `SENSOR ERROR` until a valid reading arrives. Each rejected reading is logged with its raw values, and `sys` counts them (`rejected samples N`).

**Altitude and sea-level pressure.** Weather services and airports report the pressure reduced to sea level (QNH); the pressure measured at 900 m is about 100 hPa lower. Set the altitude of the station on the setup page or in menuconfig ("Altitude of the station", in metres, including the floor it stands on): the display, the web page (main value and chart) and MQTT (`pressure_msl_hpa`) then show the sea-level pressure. Each 8 m of error in the altitude gives about 1 hPa of error; a topographic map or Google Earth is better than the GPS of a phone. The history and the CSV keep the pressure measured at the station.

**Dew point.** Computed from the temperature and the humidity (Magnus formula), shown on the web page below the humidity and sent by MQTT (`dew_point_c`).

**Calibration.** Each channel can be corrected by an offset, set on the serial console:

| Command | Effect |
|---|---|
| `cal` | shows the offsets |
| `cal set temperature -1.25` | subtracts 1.25 °C from every reading (limits: ±5 °C, ±10 %RH, ±10 hPa) |
| `cal clear pressure` / `cal clear` | resets one offset / all of them |

The offsets are applied to every reading before it is shown, sent or stored, and are kept in NVS across restarts and firmware updates. They belong to the chip: after a sensor replacement they are reset to 0, with a warning. Every change is recorded in the flash history, so it is known which readings were corrected.

**Check against an airport.** With the ICAO code of a nearby airport set on the setup page or in menuconfig ("METAR airport", for example `SBGR`), the station fetches its METAR from aviationweather.gov once an hour and compares its own sea-level pressure with the airport's QNH. After a day of samples, `sys`, the web page footer and the MQTT topic `weather-station/<device_id>/metar` show the median difference of the last 7 days and the two corrections that would cancel it: a pressure offset (`cal set pressure ...`) or a different altitude. The station never applies them by itself; set the altitude first if it is uncertain, and the offset for what remains. A warning is logged when the median exceeds ±2 hPa. Pick an airport less than about 50 km away; leave the code empty to disable the check (no request leaves the station).

## Measurement history

Every 60 s (configurable) the latest reading is stored in the `history` partition of the internal flash (5 MB, about 325,000 readings, roughly 7.4 months at 60 s). When it is full, the oldest readings are overwritten. The history survives restarts and power loss. Each stored reading names the device and the sensor that took it.

It is read in the serial monitor (`idf.py monitor`), at the `weather> ` prompt:

| Command | What it does |
|---|---|
| `log info` | number of stored readings, space used, device and sensor |
| `log dump 100` | the last 100 readings as CSV (`log dump` alone prints everything, which takes a while at 115200 baud) |
| `log clear` | erases the history |
| `help` | lists the commands |

```
device_id,sensor_id,seq,time,temperature_c,humidity_rh,pressure_hpa
ws-a0b76551a842,bme280-3fa91c07,1520,2026-10-16 14:31:00,23.41,56.12,1012.8
ws-a0b76551a842,bme280-3fa91c07,1521,2026-10-16 14:32:00,23.44,56.05,1012.8
# 2 records
```

To save the CSV on the PC, press `Ctrl+T` then `Ctrl+L` in the monitor before `log dump`: the output is saved to a log file in the current directory until the shortcut is pressed again.

> **Updating from v1.x: download the history first.** v2.0.0 stores the history in a new format and does not read the old one: download `/history.csv` (or `log dump`) before flashing v2.0.0. The old data are ignored and overwritten as new readings arrive. Going back to a v1.x firmware afterwards needs `log clear`.

> **Updating from v1.5.0 or earlier:** v1.6.0 uses its own partition table (`partitions.csv`). New checkouts get it from `sdkconfig.defaults`; in an existing local `sdkconfig`, choose it in `idf.py menuconfig` → "Partition Table" → "Custom partition table CSV" (file `partitions.csv`). `idf.py flash` writes the new table; the Wi-Fi settings are not lost.

## Troubleshooting

**Wi-Fi signal first.** The web page needs a signal of about −60 dBm or better (`sys` shows it, and the `health:` log line every 10 minutes). Around −65 to −75 dBm the link loses packets: the page and its charts can take seconds or stall. Move the station (or a mesh node) closer and keep the end of the ESP32 module, where the antenna is, free of cables and metal. An answer that takes a second or more is logged, for example `W (...) web: Slow answer: /api/charts, 16384 bytes in 3.2 s`; many of them mean a lossy link. `ping` from a computer to the station shows it too: more than a few percent packet loss is too much. Close the web page while downloading the full CSV: the server answers one request at a time, and the page's requests wait (and fail with `error in send : 104` in the log) until the download ends. Every Wi-Fi connection is logged with the access point, channel, channel width, security and signal, for example `Associated with "IoT" (ca:06:c3:51:a8:42), channel 6 HT20, WPA2, -57 dBm`, and every disconnection with the access point and the meaning of the reason, for example `Wi-Fi disconnected from "IoT" (ca:06:c3:51:a8:42, -61 dBm): reason 2, access point did not answer authentication`.

To see how often Wi-Fi drops, `sys` and the `health:` line count the connections lost since start per disconnection reason, for example `Wi-Fi drops 3 (reason 6: 2, reason 200: 1), last 420 s ago`.

In the serial monitor, `sys` shows the uptime, why the station last started, the free memory, the source of the settings, the device and sensor identifiers and the firmware version, the calibration, the samples rejected as out of range, the altitude, the METAR check, the Wi-Fi link (signal, IP, gateway), the network watchdog (last sign of traffic and where it came from), the MQTT state and how many web requests were served. Every 10 minutes the same figures appear in the log as a `health:` line.

**One device cannot reach the station while others can.** On the serial console, `arp clear`, then try to open the page from that device, then `arp`. If the device's address is listed, its packets reach the station and the answers are lost on the way back; if not, its packets never arrive (often a mesh that does not forward between its nodes or bands; try that device on the same node as the station). `ping <ip>` from the station tests the other direction, and `ping <ip> 20 1400` tests large packets:

```
weather> arp
IP address       MAC address        interface
192.168.0.1      02:10:18:51:79:8c  st
192.168.0.106    8c:17:59:3f:7e:9c  st
weather> ping 192.168.0.106 3
PING 192.168.0.106, 56 bytes, 3 times
64 bytes from 192.168.0.106: seq=1 ttl=128 time=12 ms
seq=2: no reply
64 bytes from 192.168.0.106: seq=3 ttl=128 time=9 ms
3 sent, 2 received, 33 % lost
```

If Wi-Fi stays associated but no traffic gets through, the station notices it within about 3 minutes (every minute it looks for MQTT or web traffic, and pings the router only when there is none), reconnects Wi-Fi, and after two reconnections without success restarts itself. The history in flash is kept. After a restart, the first log lines show the reason (`health: Started by: software restart`).

The MQTT client ID is the device identifier (`ws-` followed by the board's MAC address); it appears in the `Connected to MQTT broker ... as "..."` log line and in `sys`, which helps to find the station in the broker's log.

## Configuration

The station name, the altitude (−500 to 9000 m, default 0), the METAR airport for the pressure check (default empty, off), the measurement interval (1 to 3600 s, default 2 s), the Wi-Fi credentials, and the MQTT broker URI, user, password and publish interval (1 to 86400 s, default 60 s), the history log interval (1 to 86400 s, default 60 s), and the display switch-off after inactivity (0 to 1440 min, default 5 min) are set with `idf.py menuconfig` in the "Weather Station" menu. The station name, the altitude, the METAR airport, the Wi-Fi network and the MQTT broker (address, user, password) can also be set from a phone ([Setup from a phone](#setup-from-a-phone)); once stored there, they replace the menuconfig values, which then serve as factory defaults for `setup clear`.

All other tunable parameters live in [main/config.h](main/config.h):

- I2C pins, frequency and port;
- BME280 and SSD1306 addresses;
- display size (128x64 or 128x32), contrast and yellow band;
- valid range of the sensor readings, calibration limits and the parameters of the METAR check;
- setup mode: hold time of the button, access point, idle timeout, test timeouts and the limits of the settings;
- task periods, stack sizes and priorities;
- NTP server, time zone (default UTC-3), Wi-Fi retry interval and signal icon thresholds;
- MQTT topic prefix, QoS, retain flag and reconnect interval;
- Wi-Fi power save (off by default: some routers stop delivering packets to a sleeping ESP32);
- history partition label and console prompt;
- web server port, chart window, maximum points and connections.

The flash layout is in [partitions.csv](partitions.csv).

## Architecture

The firmware runs five FreeRTOS tasks, a sixth when a METAR airport is set, plus the serial console:

- **`taskMeasure`**: reads the BME280 at the measurement interval (2 s by default) and applies the calibration.
- **`taskDisplay`**: refreshes the display every 1 s, and right away when a new measurement arrives, with a measurement dot, the date and time and a Wi-Fi signal icon in the header.
- **`taskPublish`**: sends the latest reading to the MQTT broker at the publish interval (60 s by default), only when the broker is configured.
- **`taskLog`**: stores the latest reading in the flash history at the log interval (60 s by default).
- **`taskHealth`**: pings the router every minute and recovers a stuck network, and logs a health line every 10 minutes.
- **`taskMetar`**: once an hour, compares the sea-level pressure with the METAR of the configured airport.

The HTTP server (`web_server`) runs in its own ESP-IDF task once Wi-Fi has an IP address. In setup mode (`provisioning`), none of the network tasks run: the station opens its own access point with a DNS server and the setup page instead, and a task of its own tests new settings.

Wi-Fi and NTP run in the background (`net_time`), as does the MQTT client (`telemetry`); all of them reconnect on their own and network failures never stop the station. The MQTT client waits while Wi-Fi is down and reconnects as soon as Wi-Fi is back.

The `i2cMutex` mutex controls access to the I2C bus, and `dataMutex` protects the data shared between the tasks. After 3 consecutive failures, the device is re-initialized automatically. If the display or the sensor fails to start at boot, the system halts and signals the error through the LED and the serial console.

```
main/
├── main.cpp      # app_main, initialization and FreeRTOS tasks
├── config.h      # central configuration
├── bme280.*      # BME280 driver (Bosch compensation formulas, range check)
├── ssd1306.*     # SSD1306 driver with a local frame buffer
├── font5x7.h     # 5x7 font (ASCII + degree sign)
├── net_time.*    # Wi-Fi station + NTP time of day
├── telemetry.*   # MQTT client, JSON payload
├── history.*     # measurement history in flash + `log` console command
├── web_server.*  # HTTP server: page, JSON and CSV
├── health.*      # health log, `sys` command, network watchdog
├── identity.*    # device and sensor identifiers, station name
├── gzip_stream.* # compact gzip compressor for the CSV download
├── charts.*      # chart tables (1 h / 24 h / 7 d) kept in RAM for the web page
├── quality.*     # calibration + `cal` command, sea-level pressure, dew point
├── metar.*       # hourly check of the pressure against an airport METAR
├── settings.*    # per-installation settings: stored in NVS, or the menuconfig values
├── provisioning.* # setup mode: access point, captive DNS, setup page, `setup` command
├── web_page.html # the web page, embedded in the firmware
├── setup_page.html # the setup page, embedded in the firmware
├── idf_component.yml  # external component: espressif/mqtt
└── Kconfig.projbuild  # menuconfig menu: intervals, and factory defaults of name, altitude, METAR airport, Wi-Fi, MQTT broker
partitions.csv    # flash layout: nvs, phy_init, factory app (2 MB), history (5 MB)
```

The full functional specification is in [esp32-weather-station.md](esp32-weather-station.md).

## Roadmap

- Hardware RTC module so the time survives power loss without Wi-Fi
- A second sensor (the history format and the identifiers already allow it)
