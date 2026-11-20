# Functional Specification: Weather Station Frontend

**Version:** 1.0.2

## Document Control

### Revision History

| Version | Date       | Changes                                   |
|---------|------------|-------------------------------------------|
| 1.0.0   | 2026-11-20 | Initial specification: authenticated, read-only web frontend at `weather.elbastos.com.br` fed by the MQTT messages of the stations. |
| 1.0.1   | 2026-11-20 | Broker access list optional: the broker is shared with other services and has none, so the backend account is protected by its password only (FR-1.3, FR-3.1 changed; section 6.5). Access list and TLS moved to section 7. |
| 1.0.2   | 2026-11-20 | Documentation only: the installation guide (`frontend/README.md`) rewritten as one sequence from `git clone` to the first login, with a check per step, the three accounts explained, Python older than 3.11 (`uv`), and troubleshooting. |

### Requirement Markers
*   **`[PROPOSED]`**: The requirement is new and not implemented yet.
*   **`[vX.Y.Z]`**: The requirement was added in this version of the frontend.
*   **`[CHANGED vX.Y.Z]`**: The requirement was modified in this version.
*   **`[REMOVED vX.Y.Z]`**: The requirement was dropped in this version. Its text is kept and struck through (`~~...~~`).
*   Requirement IDs are never reused or renumbered.

### Relation to the Station Specification
The station firmware is specified in `esp32-weather-station.md` (referred to here as **the station spec**; its requirements are cited as "station FR-x.y"). This document specifies a separate product, the **frontend**, with its own requirement IDs and its own SemVer version. Releases are annotated git tags `frontend-vX.Y.Z`, so they never collide with the firmware tags `vX.Y.Z`. The version bump rules of the station spec apply unchanged.

The frontend depends only on the MQTT interface of the station (station FR-6.x, FR-10.4, FR-6.7, FR-11.4, FR-13.x). It never talks to a station directly.

## 1. Introduction

### 1.1 Purpose of the Document
This document describes the functional and non-functional requirements of the weather station frontend: a web application on the internet that shows the data of every station, published over MQTT, behind a login.

### 1.2 Scope
In scope: receiving the MQTT messages of all stations, storing them, the web page with current values and charts per station and per sensor, user authentication, and the deployment behind the existing nginx server.

Out of scope: any action that changes a station (calibration, settings, setup mode, display); the frontend is read-only (FR-1.3). The nginx installation, the TLS certificates and the DNS name `weather.elbastos.com.br` exist already. The MQTT broker exists already and runs on the same server.

### 1.3 Target Audience
Developers and testers of the frontend, and the administrator of the server.

## 2. Product Overview

### 2.1 Brief Description
A backend service on the server subscribes to the topics of all stations on the local MQTT broker, stores every reading in an SQLite database and serves a web page through nginx at `https://weather.elbastos.com.br/`. After login, the page shows one card per station and, inside it, one card per sensor, with the same values, charts and details as the page built into the station (station FR-8.x), updated live as new messages arrive.

```
 Stations (ESP32)              Server (internet)
 ┌──────────────┐   mqtts    ┌───────────────────────────────────────────────┐
 │ ws-a0b7…a842 │──────────▶ │ MQTT broker ──localhost──▶ weather-frontend   │
 └──────────────┘            │  (existing)   subscribe     (Python service)  │
 ┌──────────────┐   mqtts    │                             │     ▲           │
 │ ws-…         │──────────▶ │                       SQLite DB    │ HTTP      │
 └──────────────┘            │                                    │ 127.0.0.1 │
                             │ Browser ──https──▶ nginx (existing)┘           │
                             └───────────────────────────────────────────────┘
```

### 2.2 Goals and Vision
*   See every station from anywhere, without opening ports on the home network.
*   Keep a history longer than the 7 days of charts the station serves, independent of the station's own flash history.
*   Be ready for several stations and several sensors per station, which the station spec already allows in its identifiers (station FR-10.3) but the hardware does not have yet (station section 7).
*   Later: become the place where stations are managed (see section 7); version 1 stays read-only.

## 3. Functional Requirements

### 3.1 General
*   **FR-1.1** [v1.0.0]: The frontend MUST be served at `https://weather.elbastos.com.br/` through the existing nginx server, which terminates TLS and forwards to the backend on `127.0.0.1` (section 6.3). The backend MUST NOT listen on a public address.
*   **FR-1.2** [v1.0.0]: Every page and every API endpoint except the login page (FR-2.2), the static files under `/static/` (style sheet, scripts, icon; they hold no data) and `/healthz` (NFR-2.4) MUST require an authenticated session (FR-2.x). An unauthenticated request for a page is redirected to `/login?next=<path>`; one for an API endpoint gets `401 Unauthorized` with no data.
*   **FR-1.3** [v1.0.0, CHANGED v1.0.1]: Since v1.0.1 the read-only behavior is enforced by the code alone: the broker is shared with other services and has no access list, and adding one would restrict every account (section 6.5); a read-only access list for the backend account is optional and planned for a later version (section 7). The frontend MUST be read-only toward the stations: it offers no control that sends anything to a station, and the backend MUST NOT publish any MQTT message; the code has no publish call. Before v1.0.1: "This is enforced twice: the code has no publish call, and the broker account of the backend may only subscribe (FR-3.1)."
*   **FR-1.4** [v1.0.0]: All texts of the page are in English, as on the station. Times are shown in the time zone of the browser, in 24-hour format, as on the station page.

### 3.2 Authentication
*   **FR-2.1** [v1.0.0]: Access MUST be limited to user accounts created by the administrator. There is no self-registration and no password reset by e-mail. All users have the same rights: they see every station (roles and per-user station lists: section 7).
*   **FR-2.2** [v1.0.0]: The login page (`/login`) MUST ask for a user name and a password and, on success, create a session and redirect to `next` (only a path on the same site; anything else goes to `/`). A failure shows `Invalid user name or password` without saying which of the two was wrong.
*   **FR-2.3** [v1.0.0]: Passwords MUST be stored only as Argon2id hashes (`argon2-cffi` defaults). A password needs at least 10 characters.
*   **FR-2.4** [v1.0.0]: A session MUST be a random token of 256 bits in a cookie `ws_session` with the attributes `Secure`, `HttpOnly`, `SameSite=Lax` and `Path=/`. The database keeps only the SHA-256 of the token, the user, the creation time, the time of the last request and the expiry. A session expires 30 days after its last use (`session_days` in the configuration) and is renewed at most once per hour, so not every request writes to the database. Every page load sends the cookie again with a fresh lifetime, so it lasts as long as the session on the server.
*   **FR-2.5** [v1.0.0]: The page MUST show the user name and a `Log out` link in its header. Logging out (`POST /logout`) deletes the session in the database and the cookie. Changing a user's password or deleting the user ends all of their sessions.
*   **FR-2.6** [v1.0.0]: Repeated failures MUST be slowed down: after 5 failed logins for the same user name or from the same client address within 15 minutes, further attempts for that name or address are refused (`429 Too Many Requests`, `Too many attempts, try again later`) until the 15 minutes have passed. Each failure is logged with the address and the user name; the password never is. The client address comes from `X-Forwarded-For` only when the request arrives from the trusted proxy (`127.0.0.1`).
*   **FR-2.7** [v1.0.0]: Requests that change state (`POST /login`, `POST /logout`) MUST be refused when their `Origin` header is present and is not the public origin of the frontend (protection against cross-site request forgery in addition to `SameSite`). The public origin is `public_origin` in the configuration file, default `https://weather.elbastos.com.br`; a local test sets `http://localhost:8081` (section 6.6).
*   **FR-2.8** [v1.0.0]: Users MUST be managed on the server with a command-line tool (section 6.4): `user add <name>`, `user passwd <name>`, `user del <name>`, `user list` (name, creation time, last login, number of open sessions). Passwords are read from the terminal without echo, never from the command line.

### 3.3 MQTT Ingestion
*   **FR-3.1** [v1.0.0, CHANGED v1.0.1]: Since v1.0.1 the access list is optional (FR-1.3); before, the text read "whose access list allows only reading `weather-station/#`". The backend MUST connect to the Mosquitto broker on the same server (default `mqtt://127.0.0.1:1883`, configurable) with a user name and password of its own (Mosquitto `password_file`). It subscribes with QoS 1 to:
    *   `weather-station/+/info` (station FR-10.4),
    *   `weather-station/+/status` (station FR-6.7),
    *   `weather-station/+/data` (station FR-6.3, FR-10.4),
    *   `weather-station/+/metar` (station FR-11.4).
    The prefix `weather-station` is configurable, like `MQTT_TOPIC_PREFIX` of the station.
*   **FR-3.2** [v1.0.0]: The backend MUST use a persistent MQTT session (client ID `mqtt.client_id` of the configuration, default `weather-frontend`, clean session off), so the broker queues the QoS 1 messages that arrive while the backend restarts and delivers them on reconnection; no reading is lost by an update of the frontend. Retained messages received on (re)subscription are handled like any other message; a reading already stored is not stored twice (FR-3.4).
*   **FR-3.3** [v1.0.0]: Stations MUST be discovered automatically: the first message of any of the four topics with a new `<device_id>` creates the station. The `info` message fills or replaces its name, firmware, intervals, altitude and sensor list; the `status` message its online status with the time it was received. A station whose `info` has not arrived yet is shown with its device identifier only.
*   **FR-3.4** [v1.0.0]: Every `data` message MUST be stored as one reading, keyed by device, sensor (`sensor_id` of the message) and `time`: `temperature_c`, `humidity_rh`, `pressure_hpa`, `pressure_msl_hpa`, `dew_point_c` and `rssi_dbm` as received, plus the time the backend received it. A reading with a key already stored is ignored (QoS 1 may deliver a message twice; retained messages come again on every connection). A message without `time` (station clock not synchronized, station FR-5.1) is stored with the receive time and marked, so it is shown but can be told apart; it is left out of the live chart update, and a retained copy of it that arrives again on reconnection is not stored a second time.
*   **FR-3.5** [v1.0.0]: The tendency and forecast fields of a `data` message (`pressure_trend_hpa`, `pressure_trend`, `forecast_code`, `forecast`, station FR-13.1, FR-13.2) are kept only with the latest reading of each sensor, not in the history: they are derived values that the station computes from its own history, and the frontend shows them as received.
*   **FR-3.6** [v1.0.0]: The latest `metar` message of each station MUST be kept (one per station, replaced by each new one) and shown in the station details (FR-4.6).
*   **FR-3.7** [v1.0.0]: Malformed messages (not JSON, `device_id` in the payload different from the topic, values out of the station's sensor range, station FR-1.6) MUST be dropped and logged with the topic and the reason, at most once per minute per station and reason. They never stop the ingestion.
*   **FR-3.8** [v1.0.0]: A station sensor is any `sensor_id` listed in the `info` message or seen in a `data` message. When the `info` message of a station lists, at a position (`index`), a sensor identifier different from the one before, the change is recorded with its time (a sensor replacement, station FR-10.7); the readings of the old sensor stay in the history under its identifier. The charts of a position continue across a replacement, as on the station (station FR-10.5).
*   **FR-3.9** [v1.0.0]: Fields of a station with an older firmware that does not send them (e.g. `pressure_msl_hpa` before station v2.4.0) are stored as empty and shown as `–`. Stations before v2.0.0 (topic `weather-station/data` without a device identifier) are not supported.

### 3.4 Web Page
The layout has two levels: the **overview** (`/`) with one card per station, and the **station page** (`/station/<device_id>`) that shows one station as the page built into the station does, with one card per sensor. Both share the look of the station page: the same colors, fonts, light/dark theme and chart drawing (the code of `main/web_page.html` is reused).

*   **FR-4.1** [v1.0.0]: The **overview** MUST show one card per station, sorted by name (stations without a name by device identifier, after the named ones). Mock-up (wide screen; on a phone the station cards stack in one column):

    ```
    ┌ Weather Stations ───────────────────────────────────────── admin · Log out ┐
    │ ┌ Varanda ───────────────── ● online ─┐ ┌ Escritório ──────── ○ offline ──┐ │
    │ │ ws-a0b76551a842 · -67 dBm · 12 s ago│ │ ws-0c8b95f1e2d0 · last 2 h ago  │ │
    │ │ ┌ bme280-3fa91c07 ────────────────┐ │ │ ┌ bme280-91aa0e33 ────────────┐ │ │
    │ │ │ 23.4 °C   56.1 %RH  1012.9 hPa ↓│ │ │ │ 21.0 °C  48.3 %RH 1013.4 hPa│ │ │
    │ │ │ dew 14.2  falling −1.8 in 3 h   │ │ │ │ (greyed: offline)           │ │ │
    │ │ │ Showery, becoming less settled  │ │ │ └─────────────────────────────┘ │ │
    │ │ └─────────────────────────────────┘ │ └─────────────────────────────────┘ │
    │ │ ┌ (second sensor, when present) ──┐ │                                     │
    │ └─────────────────────────────────────┘                                     │
    └─────────────────────────────────────────────────────────────────────────────┘
    ```

    *   **Station card:** the name (or the device identifier when there is none) as a link to the station page; the status (FR-4.4); the device identifier; the RSSI of the latest reading (`rssi_dbm`, with the signal bars of station FR-5.7 derived from it); the age of the latest reading (`12 s ago`).
    *   **Sensor card**, one per sensor position of the station, in the order of `index`: the sensor identifier, and one value per channel of the sensor as listed in the `info` message (`channels`: identifier, unit, resolution). For the BME280 channels this is the temperature; the humidity with the dew point below it; the sea-level pressure with the tendency arrow and text below it (station FR-13.1); and the forecast text (station FR-13.2) on a line of its own. A channel the frontend does not know is shown with its identifier, value and unit as received, so a new sensor type needs no change of the layout.
*   **FR-4.2** [v1.0.0]: The **station page** MUST show the same content as the page of the station (station FR-8.2 to FR-8.9, FR-10.5, FR-11.x, FR-13.x), with these differences:
    *   **Header:** `Weather Station · <name>` and, on the right, the status (FR-4.4), the RSSI and the age of the latest reading, plus a link back to the overview.
    *   **One sensor card per sensor position**, each holding its value cards (as on the station: Temperature, Humidity with dew point, Pressure (sea level) with station pressure and tendency, Forecast) and below them its three charts (temperature, humidity, sea-level pressure). With a single sensor the page looks like the station page; with several, each sensor card has a heading with the sensor identifier and position (`bme280-3fa91c07 · position 0 · i2c@0x76`).
    *   **Chart periods:** 1 hour, 24 hours (default) and 7 days, chosen with buttons shared by all charts of the page and remembered by the browser, as in station FR-8.4. One cursor is shared by all charts of the page, with the behavior of station FR-8.7 (mouse and touch).
    *   **Footer:** device identifier, firmware, the intervals (measure, publish, log), the altitude, per sensor its identifier with `calibrated` and the offsets as tooltip when any offset is not 0 (from `info`, station FR-11.1), the METAR check (FR-4.6), and the CSV download link (FR-4.7). The heap figures of station FR-8.9 are not shown: they are not part of the MQTT messages.
*   **FR-4.3** [v1.0.0]: The pages MUST update live: the browser keeps one Server-Sent Events connection (`/api/events`, section 6.2) and applies each new reading, status, info or METAR message to the cards at once, and adds each new reading to the charts as the station page does (averaged into the last point while it is younger than one bucket). When the connection drops, the browser reconnects (after 5 s, then doubling up to 60 s) and reloads the current data and the charts, since events may have been missed. The page also reloads them when it becomes visible again (browsers slow down hidden pages, station FR-8.4 since v1.8.5) and every 30 minutes.
*   **FR-4.4** [v1.0.0]: Each station MUST show one of three states:
    *   **online** (green dot): the latest `status` message is `online` and a reading arrived within 3 publish intervals plus 10 s (`publish_period_s` of `info`, 60 s if unknown);
    *   **no data** (amber dot): the latest `status` is `online`, but no reading arrived within that time (e.g. sensor error, station FR-6.2);
    *   **offline** (grey ring): the latest `status` is `offline` (the last will of the station, station FR-6.7), with `last seen <age>` from the latest message of any kind.
    The cards of a station that is not online are shown greyed, with the last values and their age. A station without any `status` message yet counts as online while its readings are recent.
*   **FR-4.5** [v1.0.0]: Values MUST be shown with the precision of the station page: one decimal for temperature, humidity, dew point and pressure; the tendency with one decimal and its class, e.g. `↓ -1.8 hPa in 3 h, falling`; `–` for a value that is missing.
*   **FR-4.6** [v1.0.0]: The station footer MUST show the METAR check as the station page does (station FR-11.4): airport, time of the observation, reference QNH, station QNH, difference, the median with the number of samples, and the two suggestions; `–` when the station has no METAR airport.
*   **FR-4.7** [v1.0.0]: The station page MUST offer the stored readings of the station as a CSV file, `history-<device_id>.csv`, with the columns `device_id,sensor_id,time,temperature_c,humidity_rh,pressure_hpa,pressure_msl_hpa,dew_point_c,rssi_dbm`, sent gzip-compressed by the backend to clients that accept it. The file covers the whole stored history by default; `from` and `to` (Unix times) limit it.
*   **FR-4.8** [v1.0.0]: The page MUST work without external resources (no CDN, no web fonts, no analytics), adapt to phone screens and follow the light/dark setting of the browser, like the station page (station FR-8.2). It carries the tab icon of the station page.

### 3.5 History and Charts
*   **FR-5.1** [v1.0.0]: The backend MUST keep every reading for as long as the configuration says (`retention_days`, default 0 = forever). At the default publish interval of 60 s a sensor produces about 525,600 readings a year, some 40 MB of database with indexes. A daily job deletes readings older than the retention, when one is set.
*   **FR-5.2** [v1.0.0]: The chart data MUST be averaged into time buckets by the backend, with the numbers of the station (station FR-8.4 since v2.3.2): 60 buckets of 1 minute for 1 hour, 96 of 15 minutes for 24 hours, 84 of 2 hours for 7 days, aligned to the bucket size in UTC. A bucket without readings is left out, and the page breaks the line after a gap longer than three buckets or three publish intervals, as on the station. The pressure chart shows `pressure_msl_hpa` (for readings without it, the station pressure reduced with the `altitude_m` of `info` and the formula of station FR-11.2).
*   **FR-5.3** [v1.0.0]: The resolution of the frontend charts follows the publish interval, not the measurement interval: with the default 60 s, the 1-hour chart has one reading per bucket, while the station page, fed every 2 s, is smoother. This is expected and is stated below the charts (`each point is the average of N minutes; readings every 60 s`).
*   **FR-5.4** [v1.0.0]: The administrator MUST be able to import a CSV file downloaded from a station (`/history.csv`, station FR-10.6 format: `device_id,sensor_id,seq,time,temperature_c,humidity_rh,pressure_hpa`) with the command-line tool (`import <file>`), so the frontend holds the history from before it was started. Rows already stored (same device, sensor and time) are skipped; rows without a time are skipped; `pressure_msl_hpa` and `dew_point_c` are computed from the station's `altitude_m` (from the latest `info`) and the formulas of station FR-11.2 and FR-11.3. The tool reports the rows read, stored and skipped.

## 4. Non-Functional Requirements

### 4.1 Performance
*   **NFR-1.1** [v1.0.0]: The overview and a station page MUST be complete (values and the charts of the selected period) within 1 s on a broadband connection, for up to 20 stations with 4 sensors each and one year of readings. The chart query uses an index on (device, sensor, time).
*   **NFR-1.2** [v1.0.0]: A new reading MUST appear on an open page within 2 s of its arrival at the broker.
*   **NFR-1.3** [v1.0.0]: The backend MUST run on a small server: below 150 MB of RAM and with negligible CPU at idle.

### 4.2 Reliability
*   **NFR-2.1** [v1.0.0]: The backend MUST run as a systemd service that restarts on failure, and reconnect to the broker automatically (after 1 s, doubling up to 60 s), logging each disconnection and reconnection.
*   **NFR-2.2** [v1.0.0]: The web part MUST keep serving the stored data while the broker is unreachable; the page then shows `live data unavailable` in its header instead of the station states.
*   **NFR-2.3** [v1.0.0]: The database MUST use SQLite in WAL mode, so page requests read while the ingestion writes; besides the readings, only sessions and logins are written. A daily online backup (`sqlite3 .backup`) to a configurable directory keeps the last 7 copies.
*   **NFR-2.4** [v1.0.0]: `/healthz` MUST answer without authentication with `200 ok` while the database is readable and the MQTT client is connected, `503` with the reason otherwise, and reveal nothing else, for external monitoring.

### 4.3 Security
*   **NFR-3.1** [v1.0.0]: Only nginx is reachable from the internet; the backend listens on `127.0.0.1`, and the broker port of the backend account is not exposed beyond what the stations already use.
*   **NFR-3.2** [v1.0.0]: Every answer MUST carry `Strict-Transport-Security`, `X-Content-Type-Options: nosniff`, `Referrer-Policy: same-origin` and a `Content-Security-Policy` that allows only resources of the same origin (`default-src 'self'`; inline scripts and styles only with a nonce or hash). The page inserts MQTT values (e.g. the station name) as text, never as HTML.
*   **NFR-3.3** [v1.0.0]: Secrets (broker password, database path) live in the configuration file `/etc/weather-frontend/config.toml`, readable only by the service user, and are never committed. The repository holds an example file with placeholders.
*   **NFR-3.4** [v1.0.0]: The service MUST run as an unprivileged user of its own (`weather-frontend`), with systemd hardening (`ProtectSystem=strict`, `ProtectHome=yes`, `NoNewPrivileges=yes`, `PrivateTmp=yes`, write access only to the database and backup directories).

### 4.4 Maintainability
*   **NFR-4.1** [v1.0.0]: Python 3.11 or later; dependencies pinned in `frontend/requirements.txt`: FastAPI with Uvicorn (web and SSE), `python-multipart` (login form), `aiomqtt` (MQTT), `argon2-cffi` (passwords); SQLite through the standard `sqlite3` module. Test tools in `frontend/requirements-dev.txt`. No JavaScript build step and no front-end framework: the page is plain HTML, CSS and JavaScript, like the station page.
*   **NFR-4.2** [v1.0.0]: The code lives in `frontend/` of this repository, divided into modules: `config` (configuration file), `db` (schema and queries), `ingest` (MQTT), `auth` (users, sessions, rate limit), `web` (pages, API, SSE), `cli` (command-line tool), and the static files. All configurable values are in the configuration file, with defaults in `config`.
*   **NFR-4.3** [v1.0.0]: The database schema carries a version number; each release that changes it migrates an existing database at start-up.
*   **NFR-4.4** [v1.0.0]: Tests (`pytest`) cover the parsing of each MQTT message type, including those of older firmware, the bucket averaging, the authentication and the CSV import.

## 5. Technical Architecture (Summary)

### 5.1 Components
*   **MQTT broker** (existing, same server): receives the messages of the stations; one extra account for the frontend, subscribe-only on `weather-station/#`.
*   **`weather-frontend` service** (Python, one process): an asyncio MQTT task that writes to SQLite and publishes events to an in-process broadcaster; a FastAPI application for the pages, the API and the SSE stream; a periodic task for retention, backups and session clean-up.
*   **SQLite database** `/var/lib/weather-frontend/weather.db`.
*   **nginx** (existing): TLS, HTTP/2, gzip, reverse proxy to `127.0.0.1:8081`.

### 5.2 Data Model

| Table          | Key                              | Content |
|----------------|----------------------------------|---------|
| `stations`     | `device_id`                      | name, firmware, intervals, `altitude_m`, latest `info` JSON and its time, status and its time, last message time |
| `sensors`      | `device_id`, `sensor_id`         | type, position (`index`), address, channels JSON (with offsets), first and last seen |
| `readings`     | `device_id`, `sensor_id`, `time` | `temperature_c`, `humidity_rh`, `pressure_hpa`, `pressure_msl_hpa`, `dew_point_c`, `rssi_dbm`, `received_at`, `time_from_receipt` flag |
| `latest`       | `device_id`, `sensor_id`         | the latest `data` payload as received (with tendency and forecast) |
| `metar`        | `device_id`                      | the latest `metar` payload and its receive time |
| `sensor_changes` | `device_id`, `index`, `time`   | old and new sensor identifier (FR-3.8) |
| `users`        | `name`                           | Argon2id hash, created, last login |
| `sessions`     | SHA-256 of the token             | user, created, last used, expiry |
| `meta`         | `key`                            | schema version |

## 6. Interfaces

### 6.1 MQTT (input)
As defined by the station spec: topics `weather-station/<device_id>/{info,status,data,metar}`, JSON payloads of station FR-6.3, FR-10.4, FR-11.4, FR-13.1, FR-13.2; status payloads `online` / `offline`.

### 6.2 HTTP API (internal to the page)
All endpoints need a session (FR-1.2) and answer JSON, except where noted. Besides these: `GET /login`, `POST /login` (form fields `username`, `password`, `next`), `POST /logout`, the pages `/` and `/station/<device_id>`, and `/static/`.

| Endpoint | Answer |
|----------|--------|
| `GET /api/me` | The user of the session; the page uses it to find out whether a lost event stream was a logout. |
| `GET /api/stations` | All stations with status, `info`, and per sensor the latest reading (overview). |
| `GET /api/stations/<device_id>` | One station: `info`, status, latest reading per sensor, `metar`, sensor changes. |
| `GET /api/stations/<device_id>/charts` | The three periods of FR-5.2 in one answer, per sensor: `{"periods":[{"hours":24,"bucket_s":900,"publish_period_s":60,"sensors":[{"key":"p0","sensor_id":…,"index":0,"records":[[time,t,rh,p_msl],…]}]}]}` (format of the station's `/api/charts`, grouped by sensor). |
| `GET /api/stations/<device_id>/history?hours=N&points=P` | One period, same format (N 1 to 8760, P 10 to 1440). |
| `GET /api/events` | Server-Sent Events: `reading`, `status`, `info`, `metar`, each with the device identifier and the payload; a comment line every 25 s keeps the connection open. |
| `GET /stations/<device_id>/history.csv` | CSV of FR-4.7. |
| `GET /healthz` | `ok` or the reason (NFR-2.4), no session needed. |

### 6.3 nginx (example)
```nginx
server {
    listen 443 ssl http2;
    server_name weather.elbastos.com.br;
    # ssl_certificate … (existing)

    location / {
        proxy_pass http://127.0.0.1:8081;
        proxy_set_header Host $host;
        proxy_set_header X-Forwarded-For $remote_addr;
        proxy_set_header X-Forwarded-Proto https;
    }
    location = /api/events {
        proxy_pass http://127.0.0.1:8081;
        proxy_set_header Host $host;
        proxy_set_header X-Forwarded-For $remote_addr;
        proxy_buffering off;
        proxy_read_timeout 1h;
    }
}
```

### 6.4 Command-Line Tool
`weather-frontend [--config FILE] <command>` (installed with the package; `python -m weather_frontend` in a checkout), run as the service user on the server: `serve` (used by systemd), `user add|passwd|del|list`, `import <csv>`, `backup`. It reads the same configuration file.

### 6.5 Broker Account (Mosquitto)
[CHANGED v1.0.1] The broker is Mosquitto on port 1883, shared with other services, with user name and password authentication (`allow_anonymous false`, `password_file /etc/mosquitto/pwfile`) and without TLS or an access list, on purpose: older clients of other services do not support TLS. Without `per_listener_settings true` these settings apply to every listener, so the local listener the backend uses also asks for the password. The frontend gets an account of its own:

```
# add the account (password prompted); never -c, which recreates the file without the other accounts
mosquitto_passwd /etc/mosquitto/pwfile weather-frontend
systemctl reload mosquitto      # rereads the password file without dropping connections
```

The broker credentials go into `/etc/weather-frontend/config.toml` (`[mqtt] host`, `port`, `username`, `password`; NFR-3.3). The backend connects to `127.0.0.1`; when the listener is declared as `listener 1883 localhost` and the name resolves to `::1` only, `host = "::1"` is set instead (check with `ss -ltnp | grep 1883`).

**Optional access list** (planned for a later version, section 7). With an `acl_file`, everything not granted is denied to every account, and the rules add up (a `pattern` line also grants the frontend). On a shared broker each existing account therefore needs its own lines, e.g. `user <account>` followed by `topic readwrite #` (and `topic read $SYS/#` where `$SYS` is used), anonymous clients the same lines before the first `user` line, and every account created later as well; the backend account then gets only:

```
user weather-frontend
topic read weather-station/#
```

Before v1.0.1 this section made the access list part of the installation (`mosquitto_passwd /etc/mosquitto/passwd weather-frontend` and the read-only lines above, with `topic write weather-station/<device_id>/#` or `pattern write weather-station/%c/#` for the stations when the broker had no `acl_file` yet).

### 6.6 Local Test
The frontend can be run on a development machine without nginx, at `http://localhost:8081`, with a configuration file of its own:

*   `public_origin = "http://localhost:8081"` (FR-2.7); browsers accept the `Secure` session cookie on `localhost`.
*   The broker of the server reached through an SSH tunnel, `ssh -L 1883:127.0.0.1:1883 <server>`, with the account of section 6.5, so the test receives the real messages of the stations without affecting them (the frontend never publishes, FR-1.3). A second backend connected this way MUST use another client ID (`mqtt.client_id` in the configuration, default `weather-frontend`), or the broker would close the session of the service on the server (FR-3.2).
*   A database file of its own.

A local test covers the pages, the authentication, the live updates, the charts, the CSV download and import, and the `pytest` tests. The release is approved only after a test on the server behind nginx at `https://weather.elbastos.com.br`, which also covers what a local test cannot: the SSE stream through the proxy (`proxy_buffering off`), the client address from `X-Forwarded-For` for the rate limit (FR-2.6), TLS with HSTS, and the systemd service with its hardening (NFR-3.4).

## 7. Open Issues / Future Extensions

*   **Broker access list and TLS:** a read-only access list for the backend account, so the broker enforces FR-1.3 as well (section 6.5 describes what it requires on the shared broker), and a TLS listener (`mqtts://`, port 8883) for the stations and other clients that support it, next to the plain listener for the older ones.
*   **Per-sensor data topic in the station:** all sensors of a station publish to the same retained topic `weather-station/<device_id>/data`, so the broker keeps only the latest reading of the last sensor that published. The frontend does not depend on it (it is always subscribed and stores every reading), but a new subscriber would only see one sensor at once. A station requirement such as `weather-station/<device_id>/data/<index>` would fix this when a second sensor exists.
*   **Heap and diagnostics over MQTT:** the station page shows the heap (station FR-8.9), which the MQTT messages do not carry; a periodic `.../health` message would let the frontend show it.
*   **Station management** (no longer read-only): calibration, name, altitude, METAR airport, display, over MQTT commands to the station. Needs a station requirement for a command topic and roles in the frontend.
*   **Roles and per-user station lists:** e.g. a user who sees only some stations.
*   **Longer chart periods:** 30 days and 1 year, which the stored history allows.
*   **Comparing sensors:** one chart with a line per sensor or per station (indoor vs. outdoor).
*   **Alerts:** e-mail or push when a station goes offline or a value crosses a limit.
