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

This guide goes from an empty server to the login page at `https://weather.elbastos.com.br/`. Run every command as root on the server, in the order given; each step ends with a check, so a problem shows up where it happens.

**What must exist already:** nginx serving `weather.elbastos.com.br` with its TLS certificate, Mosquitto on the same server with password authentication (`/etc/mosquitto/pwfile`), and `git`.

**Three accounts, not to be mixed up:**

| Account | Created in | Used for |
|---------|------------|----------|
| Linux user `weather-frontend` | step 2 | runs the service; no password, no login |
| Mosquitto account `weather-frontend` | step 4 | the service reads the station messages; its password goes only into the configuration file |
| Web user, e.g. `admin` | step 7 | you log in to the page with it; its own password, never the MQTT one |

### Step 1: Get the code

```sh
git clone https://github.com/edulbastos/esp32-weather-station.git /opt/weather-frontend/src
cd /opt/weather-frontend/src
git checkout $(git tag -l 'frontend-v*' --sort=-v:refname | head -1)     # the latest frontend release
ls frontend
```

Check: `ls frontend` lists `README.md`, `config.example.toml`, `pyproject.toml`, `requirements.txt`, `weather-frontend.service` and `weather_frontend`.

If `git clone` asks for a login (private repository), send the code from your development machine instead, in the project folder there:

```sh
git archive frontend-vX.Y.Z frontend | ssh root@<server> 'mkdir -p /opt/weather-frontend/src && tar -x -C /opt/weather-frontend/src'
```

### Step 2: Create the service user

```sh
useradd --system --home /var/lib/weather-frontend --shell /usr/sbin/nologin weather-frontend
```

Check: `id weather-frontend` shows the user. Its folder `/var/lib/weather-frontend` (the database) does not exist yet; systemd creates it in step 6.

### Step 3: Python environment and program

The frontend needs Python 3.11 or later. See what the server has:

```sh
python3 --version
```

**3.11 or later** (Debian 12, Ubuntu 24.04):

```sh
python3 -m venv /opt/weather-frontend/venv
```

**Older** (e.g. Debian 10 with 3.7): install a Python 3.12 for the frontend only with `uv`, inside `/opt/weather-frontend` (the service cannot read `/root`), without touching the Python of the system:

```sh
curl -LsSf https://astral.sh/uv/install.sh | env UV_INSTALL_DIR=/usr/local/bin INSTALLER_NO_MODIFY_PATH=1 sh
export UV_PYTHON_INSTALL_DIR=/opt/weather-frontend/python
uv python install 3.12
uv venv --seed --python 3.12 /opt/weather-frontend/venv
```

Then, in both cases, install the program into that environment:

```sh
/opt/weather-frontend/venv/bin/pip install -r /opt/weather-frontend/src/frontend/requirements.txt /opt/weather-frontend/src/frontend
/opt/weather-frontend/venv/bin/weather-frontend --version
```

Check: the last command prints `weather-frontend X.Y.Z`.

### Step 4: Mosquitto account for the service

```sh
mosquitto_passwd /etc/mosquitto/pwfile weather-frontend     # asks the password twice
systemctl reload mosquitto                                  # older Mosquitto: systemctl restart mosquitto
```

Never use `mosquitto_passwd -c`: it recreates the file and deletes every other account. The broker has no access list (an `acl_file` would restrict every account of the shared broker), so this account could publish; the frontend never does (specification FR-1.3, section 6.5).

Check that the account receives the station messages (the password is read without echo, so it stays out of the shell history):

```sh
read -rsp 'MQTT password: ' P; echo
mosquitto_sub -h 127.0.0.1 -u weather-frontend -P "$P" -t 'weather-station/#' -v -C 3 -W 10; unset P
```

It prints three messages of the stations (`info`, `data`, `status`). If it is refused although the password is right, see where Mosquitto listens: `ss -ltnp | grep 1883`. When the local listener is on `[::1]` only, use `host = "::1"` in step 5.

### Step 5: Configuration file

```sh
install -d -m 750 -o root -g weather-frontend /etc/weather-frontend
install -m 640 -o root -g weather-frontend /opt/weather-frontend/src/frontend/config.example.toml /etc/weather-frontend/config.toml
nano /etc/weather-frontend/config.toml
```

Change one line, in `[mqtt]`: `password = "CHANGE-ME"` becomes the password of step 4. Every other value is already right for this server: address `https://weather.elbastos.com.br`, port 8081 on `127.0.0.1`, database in `/var/lib/weather-frontend`, broker `127.0.0.1:1883`, account `weather-frontend`.

Check: `sudo -u weather-frontend head -1 /etc/weather-frontend/config.toml` prints the first line (the service can read the file; other users cannot).

### Step 6: Start the service

```sh
cp /opt/weather-frontend/src/frontend/weather-frontend.service /etc/systemd/system/
systemctl daemon-reload
systemctl enable --now weather-frontend
journalctl -u weather-frontend -n 20 --no-pager
curl -s http://127.0.0.1:8081/healthz; echo
```

Check: the log shows `Connected to the broker 127.0.0.1:1883 as weather-frontend`, and `curl` prints `ok`. The service starts again by itself after a failure or a reboot.

### Step 7: Create your web user

There is no default user. Create the one you will log in with (name of your choice; the password is asked twice, at least 10 characters, and should differ from the MQTT one):

```sh
sudo -u weather-frontend /opt/weather-frontend/venv/bin/weather-frontend --config /etc/weather-frontend/config.toml user add admin
```

Check: `user list` instead of `user add admin` shows the user. Without `sudo`: `runuser -u weather-frontend -- /opt/weather-frontend/venv/bin/weather-frontend ...`.

### Step 8: nginx

Add these two blocks to the `server` block of `weather.elbastos.com.br` (the one with the TLS certificate):

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
    proxy_buffering off;          # live updates; without it they arrive late or in bursts
    proxy_read_timeout 1h;
}
```

```sh
nginx -t && systemctl reload nginx
curl -s -o /dev/null -w '%{http_code}\n' https://weather.elbastos.com.br/login
```

Check: `curl` prints `200`.

### Step 9: Log in

Open `https://weather.elbastos.com.br/` and log in with the web user of step 7. The overview shows one card per station that has published, with a green dot while it is online; click a station for its values and charts. New readings appear by themselves, at the publish interval of the station.

The charts start empty and fill as readings arrive. To show the history from before the frontend, download `/history.csv` from each station's own page (on the local network) and import it:

```sh
sudo -u weather-frontend /opt/weather-frontend/venv/bin/weather-frontend --config /etc/weather-frontend/config.toml import /path/to/history-ws-xxxxxxxxxxxx.csv
```

### Updating to a new release

```sh
cd /opt/weather-frontend/src
git fetch --tags && git checkout frontend-vX.Y.Z
/opt/weather-frontend/venv/bin/pip install -r frontend/requirements.txt ./frontend
systemctl restart weather-frontend
```

The database is kept and migrated at start when a release changes it. Compare `frontend/config.example.toml` and `frontend/weather-frontend.service` with the installed copies when the release notes mention them.

### Troubleshooting

| Symptom | Cause and fix |
|---------|---------------|
| `pip`: `Invalid requirement ... File '/opt/weather-frontend/src/frontend' does not exist` | The code is not in `/opt/weather-frontend/src` (step 1 failed or cloned elsewhere): `ls /opt/weather-frontend/src`. |
| `python3 -m venv` or `pip` fails on an old system | Python older than 3.11: use the `uv` variant of step 3. |
| Log: `Broker 127.0.0.1:1883: ... not authorised` (or similar), retrying | The `[mqtt] password` differs from step 4, or the account is missing in `/etc/mosquitto/pwfile`. Fix, then `systemctl restart weather-frontend`. |
| Log: `Broker 127.0.0.1:1883: ... Connection refused`, retrying | Mosquitto does not listen on `127.0.0.1`: `ss -ltnp \| grep 1883`; with `[::1]` only, set `host = "::1"`. |
| `healthz` prints `broker not connected` | See the two lines above; the page still shows the stored data. |
| nginx answers `502 Bad Gateway` | The service is not running: `systemctl status weather-frontend`, `journalctl -u weather-frontend -n 50`. |
| Login: `Invalid user name or password` | No web user yet, or the MQTT account was used: create one in step 7; `user list` shows the existing ones. |
| Login: `Too many attempts, try again later` | 5 failures within 15 minutes; wait, or `systemctl restart weather-frontend` (the counter is kept in memory). |
| The page loads but values only change on reload | The `location = /api/events` block of step 8 is missing or buffered. |
| A station is missing on the overview | It has not published since the service started and has no retained message on the broker; check with the `mosquitto_sub` of step 4. |

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
