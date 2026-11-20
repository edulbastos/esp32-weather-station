"""Configuration file (TOML), with the defaults of the specification.

Every configurable value of the frontend lives here (NFR-4.2); the file
`/etc/weather-frontend/config.toml` overrides any of them.
"""

from __future__ import annotations

import os
import tomllib
from dataclasses import dataclass, field, fields

DEFAULT_PATH = "/etc/weather-frontend/config.toml"
ENV_PATH = "WEATHER_FRONTEND_CONFIG"


@dataclass
class MqttConfig:
    host: str = "127.0.0.1"
    port: int = 1883
    username: str = ""
    password: str = ""
    client_id: str = "weather-frontend"  # FR-3.2; another one for a local test (section 6.6)
    topic_prefix: str = "weather-station"  # MQTT_TOPIC_PREFIX of the station
    keepalive_s: int = 60
    reconnect_min_s: float = 1.0  # NFR-2.1: 1 s, doubling...
    reconnect_max_s: float = 60.0  # ...up to 60 s


@dataclass
class Config:
    public_origin: str = "https://weather.elbastos.com.br"  # FR-2.7
    listen_host: str = "127.0.0.1"  # FR-1.1: never a public address
    listen_port: int = 8081
    trusted_proxies: list[str] = field(default_factory=lambda: ["127.0.0.1", "::1"])  # FR-2.6
    db_path: str = "/var/lib/weather-frontend/weather.db"
    backup_dir: str = "/var/lib/weather-frontend/backup"  # NFR-2.3
    backup_keep: int = 7
    backup_hour: int = 3  # local time of the daily maintenance (backup, retention)
    retention_days: int = 0  # FR-5.1: 0 = keep forever
    session_days: int = 30  # FR-2.4
    session_renew_s: int = 3600  # FR-2.4: renewed at most once per hour
    login_max_failures: int = 5  # FR-2.6
    login_window_s: int = 900
    password_min_length: int = 10  # FR-2.3
    sse_keepalive_s: int = 25  # section 6.2
    log_throttle_s: int = 60  # FR-3.7
    mqtt: MqttConfig = field(default_factory=MqttConfig)

    @property
    def cookie_name(self) -> str:
        return "ws_session"


def _apply(obj, values: dict, where: str) -> None:
    known = {f.name: f for f in fields(obj)}
    for key, value in values.items():
        if key not in known:
            raise ValueError(f"unknown setting {where}{key}")
        current = getattr(obj, key)
        if isinstance(current, MqttConfig):
            if not isinstance(value, dict):
                raise ValueError(f"[{key}] must be a table")
            _apply(current, value, f"{key}.")
        else:
            if isinstance(current, bool) or not isinstance(value, type(current)):
                if isinstance(current, float) and isinstance(value, int):
                    value = float(value)
                else:
                    raise ValueError(f"{where}{key} must be of type {type(current).__name__}")
            setattr(obj, key, value)


def load(path: str | None = None) -> Config:
    """Reads the configuration file; a missing file at the default path gives the defaults."""
    path = path or os.environ.get(ENV_PATH) or DEFAULT_PATH
    cfg = Config()
    try:
        with open(path, "rb") as f:
            values = tomllib.load(f)
    except FileNotFoundError:
        if path != DEFAULT_PATH:
            raise
        return cfg
    _apply(cfg, values, "")
    cfg.public_origin = cfg.public_origin.rstrip("/")
    return cfg
