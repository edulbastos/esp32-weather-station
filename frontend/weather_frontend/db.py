"""SQLite database: schema, migrations and queries (section 5.2)."""

from __future__ import annotations

import json
import os
import sqlite3
from typing import Iterator

from . import derive

SCHEMA_VERSION = 1

SCHEMA = """
CREATE TABLE IF NOT EXISTS meta (
    key   TEXT PRIMARY KEY,
    value TEXT NOT NULL
);
CREATE TABLE IF NOT EXISTS stations (
    device_id        TEXT PRIMARY KEY,
    name             TEXT,
    firmware         TEXT,
    measure_period_s INTEGER,
    publish_period_s INTEGER,
    log_period_s     INTEGER,
    altitude_m       INTEGER,
    info_json        TEXT,
    info_time        INTEGER,
    status           TEXT,
    status_time      INTEGER,
    first_seen       INTEGER NOT NULL,
    last_seen        INTEGER NOT NULL
);
CREATE TABLE IF NOT EXISTS sensors (
    device_id     TEXT NOT NULL,
    sensor_id     TEXT NOT NULL,
    type          TEXT,
    idx           INTEGER,
    address       TEXT,
    channels_json TEXT,
    first_seen    INTEGER NOT NULL,
    last_seen     INTEGER NOT NULL,
    PRIMARY KEY (device_id, sensor_id)
);
CREATE TABLE IF NOT EXISTS readings (
    device_id         TEXT NOT NULL,
    sensor_id         TEXT NOT NULL,
    time              INTEGER NOT NULL,
    temperature_c     REAL,
    humidity_rh       REAL,
    pressure_hpa      REAL,
    pressure_msl_hpa  REAL,
    dew_point_c       REAL,
    rssi_dbm          INTEGER,
    received_at       INTEGER,
    time_from_receipt INTEGER NOT NULL DEFAULT 0,
    PRIMARY KEY (device_id, sensor_id, time)
) WITHOUT ROWID;
CREATE INDEX IF NOT EXISTS readings_time ON readings (time);
CREATE TABLE IF NOT EXISTS latest (
    device_id    TEXT NOT NULL,
    sensor_id    TEXT NOT NULL,
    time         INTEGER NOT NULL,
    received_at  INTEGER NOT NULL,
    payload_json TEXT NOT NULL,
    PRIMARY KEY (device_id, sensor_id)
);
CREATE TABLE IF NOT EXISTS metar (
    device_id    TEXT PRIMARY KEY,
    received_at  INTEGER NOT NULL,
    payload_json TEXT NOT NULL
);
CREATE TABLE IF NOT EXISTS sensor_changes (
    device_id TEXT NOT NULL,
    idx       INTEGER NOT NULL,
    time      INTEGER NOT NULL,
    old_id    TEXT NOT NULL,
    new_id    TEXT NOT NULL,
    PRIMARY KEY (device_id, idx, time)
);
CREATE TABLE IF NOT EXISTS users (
    name       TEXT PRIMARY KEY,
    hash       TEXT NOT NULL,
    created    INTEGER NOT NULL,
    last_login INTEGER
);
CREATE TABLE IF NOT EXISTS sessions (
    token_hash TEXT PRIMARY KEY,
    user       TEXT NOT NULL REFERENCES users (name) ON DELETE CASCADE,
    created    INTEGER NOT NULL,
    last_used  INTEGER NOT NULL,
    expires    INTEGER NOT NULL
);
CREATE INDEX IF NOT EXISTS sessions_user ON sessions (user);
"""

# Fields of a data message kept with every reading (FR-3.4).
READING_FIELDS = ("temperature_c", "humidity_rh", "pressure_hpa", "pressure_msl_hpa", "dew_point_c", "rssi_dbm")

# Chart periods of FR-5.2: hours and number of buckets (station FR-8.4 since v2.3.2).
CHART_PERIODS = ((1, 60), (24, 96), (168, 84))


def open_conn(path: str) -> sqlite3.Connection:
    """Opens a connection in autocommit mode (explicit BEGIN where needed)."""
    conn = sqlite3.connect(path, timeout=10, isolation_level=None, check_same_thread=False)
    conn.row_factory = sqlite3.Row
    conn.execute("PRAGMA synchronous=NORMAL")
    conn.execute("PRAGMA foreign_keys=ON")
    conn.execute("PRAGMA busy_timeout=10000")
    return conn


def connect(path: str) -> sqlite3.Connection:
    """Opens the database in WAL mode (NFR-2.3) and creates or migrates the schema."""
    directory = os.path.dirname(os.path.abspath(path))
    os.makedirs(directory, exist_ok=True)
    conn = open_conn(path)
    conn.execute("PRAGMA journal_mode=WAL")
    migrate(conn)
    return conn


def migrate(conn: sqlite3.Connection) -> None:
    """NFR-4.3: brings an existing database to SCHEMA_VERSION."""
    conn.executescript(SCHEMA)
    row = conn.execute("SELECT value FROM meta WHERE key = 'schema_version'").fetchone()
    version = int(row["value"]) if row else 0
    if version > SCHEMA_VERSION:
        raise RuntimeError(f"database schema {version} is newer than this frontend ({SCHEMA_VERSION})")
    # Future migrations: `if version < 2: ...`, each in its own transaction.
    if version < SCHEMA_VERSION:
        conn.execute(
            "INSERT OR REPLACE INTO meta (key, value) VALUES ('schema_version', ?)", (str(SCHEMA_VERSION),)
        )


# ---------------------------------------------------------------------------
# Writes (ingestion and import)
# ---------------------------------------------------------------------------


def touch_station(conn: sqlite3.Connection, device_id: str, now: int) -> None:
    """FR-3.3: any message of a new device creates the station."""
    conn.execute(
        "INSERT INTO stations (device_id, first_seen, last_seen) VALUES (?, ?, ?) "
        "ON CONFLICT (device_id) DO UPDATE SET last_seen = MAX(last_seen, excluded.last_seen)",
        (device_id, now, now),
    )


def touch_sensor(conn: sqlite3.Connection, device_id: str, sensor_id: str, now: int) -> None:
    conn.execute(
        "INSERT INTO sensors (device_id, sensor_id, first_seen, last_seen) VALUES (?, ?, ?, ?) "
        "ON CONFLICT (device_id, sensor_id) DO UPDATE SET last_seen = MAX(last_seen, excluded.last_seen)",
        (device_id, sensor_id, now, now),
    )


def insert_reading(conn: sqlite3.Connection, device_id: str, sensor_id: str, time: int, values: dict,
                   received_at: int | None, time_from_receipt: bool) -> bool:
    """Stores one reading; False when one with the same key exists already (FR-3.4)."""
    cur = conn.execute(
        "INSERT OR IGNORE INTO readings (device_id, sensor_id, time, temperature_c, humidity_rh, pressure_hpa, "
        "pressure_msl_hpa, dew_point_c, rssi_dbm, received_at, time_from_receipt) "
        "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)",
        (device_id, sensor_id, time, *(values.get(f) for f in READING_FIELDS), received_at,
         1 if time_from_receipt else 0),
    )
    return cur.rowcount == 1


# ---------------------------------------------------------------------------
# Reads (web)
# ---------------------------------------------------------------------------


def _loads(text: str | None):
    if not text:
        return None
    try:
        return json.loads(text)
    except ValueError:
        return None


def _station_row(row: sqlite3.Row) -> dict:
    return {
        "device_id": row["device_id"],
        "name": row["name"] or "",
        "firmware": row["firmware"],
        "measure_period_s": row["measure_period_s"],
        "publish_period_s": row["publish_period_s"],
        "log_period_s": row["log_period_s"],
        "altitude_m": row["altitude_m"],
        "status": row["status"],
        "status_time": row["status_time"],
        "first_seen": row["first_seen"],
        "last_seen": row["last_seen"],
    }


def sensor_groups(conn: sqlite3.Connection, device_id: str, info: dict | None) -> list[dict]:
    """The sensor cards of a station (FR-4.1, FR-4.2, FR-3.8).

    One group per position listed in the latest info message, holding every
    sensor that ever had that position (a replaced chip keeps its readings and
    the chart continues), plus one group per sensor seen only in data messages.
    """
    known = {
        r["sensor_id"]: r
        for r in conn.execute("SELECT * FROM sensors WHERE device_id = ?", (device_id,))
    }
    latest = {
        r["sensor_id"]: r
        for r in conn.execute("SELECT * FROM latest WHERE device_id = ?", (device_id,))
    }
    groups: list[dict] = []
    claimed: set[str] = set()
    for s in (info or {}).get("sensors") or []:
        if not isinstance(s, dict) or not isinstance(s.get("id"), str):
            continue
        index = s.get("index")
        ids = [sid for sid, r in known.items() if index is not None and r["idx"] == index]
        if s["id"] not in ids:
            ids.append(s["id"])
        claimed.update(ids)
        groups.append({
            "key": f"p{index}" if index is not None else f"s{s['id']}",
            "index": index,
            "sensor_id": s["id"],
            "type": s.get("type"),
            "address": s.get("address"),
            "channels": s.get("channels") or [],
            "sensor_ids": sorted(ids),
        })
    for sid in sorted((set(known) | set(latest)) - claimed):
        r = known.get(sid)
        groups.append({
            "key": f"s{sid}",
            "index": r["idx"] if r else None,
            "sensor_id": sid,
            "type": r["type"] if r else sid.split("-")[0],
            "address": r["address"] if r else None,
            "channels": (_loads(r["channels_json"]) if r else None) or [],
            "sensor_ids": [sid],
        })
    for g in groups:
        rows = [latest[sid] for sid in g["sensor_ids"] if sid in latest]
        current = latest.get(g["sensor_id"])
        best = current or (max(rows, key=lambda r: r["time"]) if rows else None)
        if best is not None:
            reading = _loads(best["payload_json"]) or {}
            reading["received_at"] = best["received_at"]
            reading["time"] = best["time"]
            g["latest"] = reading
        else:
            g["latest"] = None
    groups.sort(key=lambda g: (g["index"] is None, g["index"] if g["index"] is not None else 0, g["sensor_id"]))
    return groups


def station_summary(conn: sqlite3.Connection, row: sqlite3.Row) -> dict:
    st = _station_row(row)
    info = _loads(row["info_json"])
    st["sensors"] = sensor_groups(conn, st["device_id"], info)
    received = [g["latest"]["received_at"] for g in st["sensors"] if g["latest"]]
    st["last_reading"] = max(received) if received else None
    return st


def list_stations(conn: sqlite3.Connection) -> list[dict]:
    """FR-4.1: sorted by name, stations without a name after the named ones, by device identifier."""
    rows = conn.execute("SELECT * FROM stations").fetchall()
    stations = [station_summary(conn, r) for r in rows]
    stations.sort(key=lambda s: (s["name"] == "", s["name"].casefold(), s["device_id"]))
    return stations


def get_station(conn: sqlite3.Connection, device_id: str) -> dict | None:
    row = conn.execute("SELECT * FROM stations WHERE device_id = ?", (device_id,)).fetchone()
    if row is None:
        return None
    st = station_summary(conn, row)
    st["info"] = _loads(row["info_json"])
    m = conn.execute("SELECT * FROM metar WHERE device_id = ?", (device_id,)).fetchone()
    st["metar"] = {"received_at": m["received_at"], **(_loads(m["payload_json"]) or {})} if m else None
    st["sensor_changes"] = [
        dict(r) for r in conn.execute(
            "SELECT idx AS \"index\", time, old_id, new_id FROM sensor_changes WHERE device_id = ? ORDER BY time",
            (device_id,),
        )
    ]
    first = conn.execute("SELECT MIN(time) AS t, COUNT(*) AS n FROM readings WHERE device_id = ?",
                         (device_id,)).fetchone()
    st["history"] = {"first": first["t"], "readings": first["n"]}
    return st


def chart_period(conn: sqlite3.Connection, station: dict, hours: int, points: int, now: int) -> dict:
    """FR-5.2: the readings of the last `hours`, averaged into `points` buckets aligned in UTC.

    Records are [time, temperature_c, humidity_rh, pressure_msl_hpa], in the
    format of the station's /api/history, one list per sensor group.
    """
    bucket = max(1, round(hours * 3600 / points))
    start = (now // bucket) * bucket - (points - 1) * bucket
    factor = derive.msl_factor(station.get("altitude_m") or 0)
    sensors = []
    for g in station["sensors"]:
        ids = g["sensor_ids"]
        marks = ",".join("?" * len(ids))
        rows = conn.execute(
            f"SELECT time / ? AS b, AVG(time) AS t, AVG(temperature_c) AS temp, AVG(humidity_rh) AS hum, "
            f"AVG(COALESCE(pressure_msl_hpa, pressure_hpa * ?)) AS pres "
            f"FROM readings WHERE device_id = ? AND sensor_id IN ({marks}) AND time >= ? "
            f"GROUP BY b ORDER BY b",
            (bucket, factor, station["device_id"], *ids, start),
        ).fetchall()
        records = [
            [round(r["t"]), _round(r["temp"], 2), _round(r["hum"], 2), _round(r["pres"], 2)] for r in rows
        ]
        sensors.append({"key": g["key"], "sensor_id": g["sensor_id"], "index": g["index"], "records": records})
    return {
        "hours": hours,
        "points": points,
        "bucket_s": bucket,
        "publish_period_s": station.get("publish_period_s") or 60,
        "altitude_m": station.get("altitude_m") or 0,
        "available": any(s["records"] for s in sensors),
        "sensors": sensors,
    }


def _round(value, digits):
    return None if value is None else round(value, digits)


CSV_COLUMNS = ("device_id", "sensor_id", "time", *READING_FIELDS)


def iter_csv_rows(conn: sqlite3.Connection, device_id: str, start: int | None, end: int | None,
                  batch: int = 2000) -> Iterator[tuple]:
    """FR-4.7: every stored reading of a station, oldest first."""
    sql = f"SELECT {', '.join(CSV_COLUMNS)} FROM readings WHERE device_id = ?"
    args: list = [device_id]
    if start is not None:
        sql += " AND time >= ?"
        args.append(start)
    if end is not None:
        sql += " AND time <= ?"
        args.append(end)
    sql += " ORDER BY time, sensor_id"
    cur = conn.execute(sql, args)
    while True:
        rows = cur.fetchmany(batch)
        if not rows:
            return
        for r in rows:
            yield tuple(r)
