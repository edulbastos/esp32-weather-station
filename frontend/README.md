# Weather Station Frontend

Read-only web frontend at `https://weather.elbastos.com.br/` for the ESP32 weather stations of this repository. It subscribes to the MQTT messages of every station on the local Mosquitto broker, stores the readings in SQLite and shows them behind a login: one card per station, one card per sensor, and per station the same values and charts as the page built into the station, updated live.

Specification: [`../weather-frontend.md`](../weather-frontend.md). Releases are tagged `frontend-vX.Y.Z`.

## Layout

| Path | Content |
|------|---------|
| `weather_frontend/config.py` | Configuration file and defaults |
| `weather_frontend/db.py` | SQLite schema, migrations, queries (stations, sensor cards, chart buckets, CSV) |
| `weather_frontend/ingest.py` | MQTT ingestion (`info`, `status`, `data`, `metar`) |
| `weather_frontend/auth.py` | Users, sessions, login rate limit |
| `weather_frontend/events.py` | Broadcaster from the ingestion to the live event streams |
| `weather_frontend/web.py` | Pages, API, Server-Sent Events, security headers |
| `weather_frontend/maintenance.py` | Session clean-up, retention, daily backup |
| `weather_frontend/cli.py` | `weather-frontend` command (`serve`, `user`, `import`, `backup`) |
| `weather_frontend/pages/`, `static/` | HTML pages, style sheet, scripts (no external resources) |
| `tests/` | `pytest` tests |
| `config.example.toml` | Example configuration with every setting and its default |
| `weather-frontend.service` | systemd unit |

## Install on the server

Python 3.11 or later. The commands below assume Debian or Ubuntu and are run as root.

1. Service user, program and configuration:

   ```sh
   useradd --system --home /var/lib/weather-frontend --shell /usr/sbin/nologin weather-frontend
   python3 -m venv /opt/weather-frontend/venv
   /opt/weather-frontend/venv/bin/pip install -r frontend/requirements.txt ./frontend
   install -d -m 750 -o root -g weather-frontend /etc/weather-frontend
   install -m 640 -o root -g weather-frontend frontend/config.example.toml /etc/weather-frontend/config.toml
   ```

   Edit `/etc/weather-frontend/config.toml`: at least `[mqtt] password`.

2. Broker account (Mosquitto, port 1883, password authentication, no access list):

   ```sh
   mosquitto_passwd /etc/mosquitto/pwfile weather-frontend   # never -c: it recreates the file without the other accounts
   systemctl reload mosquitto
   mosquitto_sub -h 127.0.0.1 -u weather-frontend -P '<password>' -t 'weather-station/#' -v -C 3 -W 10   # check
   ```

   Put the same password into `[mqtt] password` of the configuration. If the check is refused although the password is right, see where the local listener listens (`ss -ltnp | grep 1883`): a `listener 1883 localhost` may bind `::1` only; then set `host = "::1"` in `[mqtt]`.

   The frontend never publishes; the broker does not enforce it, since it has no access list (an `acl_file` would restrict every account of the shared broker). A read-only access list is planned for a later version; section 6.5 of the specification describes what it requires.

3. Service:

   ```sh
   cp frontend/weather-frontend.service /etc/systemd/system/
   systemctl daemon-reload
   systemctl enable --now weather-frontend
   journalctl -u weather-frontend -f     # expect "Connected to the broker 127.0.0.1:1883 as weather-frontend"
   ```

4. First user (the password is asked twice, at least 10 characters):

   ```sh
   sudo -u weather-frontend /opt/weather-frontend/venv/bin/weather-frontend --config /etc/weather-frontend/config.toml user add <name>
   ```

5. nginx, in the `server` block of `weather.elbastos.com.br` (TLS as it is):

   ```nginx
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
   ```

   `nginx -t && systemctl reload nginx`, then open `https://weather.elbastos.com.br/`.

6. Optional: the history from before the frontend. Download `/history.csv` from each station's own page and import it:

   ```sh
   sudo -u weather-frontend /opt/weather-frontend/venv/bin/weather-frontend --config /etc/weather-frontend/config.toml import history-ws-a0b76551a842.csv
   ```

## Commands

`weather-frontend [--config FILE] <command>`; without `--config`, `$WEATHER_FRONTEND_CONFIG` or `/etc/weather-frontend/config.toml`.

| Command | Does |
|---------|------|
| `serve` | Runs the service (used by systemd) |
| `user add <name>` | Adds a user |
| `user passwd <name>` | Changes a password and ends the user's sessions |
| `user del <name>` | Deletes a user and ends the user's sessions |
| `user list` | Users with creation time, last login and open sessions |
| `import <file>` | Imports a `history.csv` downloaded from a station; rows already stored are skipped |
| `backup` | Writes a backup now (the service writes one every day at 03:30 and keeps 7) |

`/healthz` answers `ok` while the database is readable and the broker connected, `503` with the reason otherwise; it needs no login.

## Local test

On a development machine, without nginx (section 6.6 of the specification):

```sh
cd frontend
python3 -m venv .venv
.venv/bin/pip install -r requirements-dev.txt
.venv/bin/python -m pytest                 # tests

cat > config.toml <<'EOF'
public_origin = "http://localhost:8081"
db_path = "dev.db"
backup_dir = "dev-backup"
[mqtt]
username = "weather-frontend"
password = "..."
client_id = "weather-frontend-dev"        # not the one of the service on the server
EOF
# Tunnel to the broker of the server. <server> is your SSH login (user@host); the tunnel only
# forwards TCP, and the frontend logs in to Mosquitto with [mqtt] username/password above.
# If port 1883 is taken locally (a local broker), use e.g. -L 11883:127.0.0.1:1883 and port = 11883.
ssh -N -L 1883:127.0.0.1:1883 <server> &
.venv/bin/python -m weather_frontend --config config.toml user add admin
.venv/bin/python -m weather_frontend --config config.toml serve
```

Then open `http://localhost:8081/`. `config.toml` and `*.db` are ignored by git.
