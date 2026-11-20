"""MQTT ingestion (section 3.3): subscribes to the topics of all stations and stores the messages.

`Ingestor.handle()` holds the logic without any network, so tests feed it
messages directly; `run()` is the connection loop of the service.
"""

from __future__ import annotations

import asyncio
import json
import logging
import re
import sqlite3
import time

import aiomqtt

from . import db, derive
from .config import Config
from .events import Broadcaster

log = logging.getLogger(__name__)

DEVICE_ID_RE = re.compile(r"^ws-[0-9a-f]{12}$")  # station FR-10.1
KINDS = ("info", "status", "data", "metar")
MIN_TIME = 1_000_000_000  # earlier "time" values are not a synchronized clock


class Malformed(Exception):
    """A message that is dropped (FR-3.7)."""


def _number(obj: dict, key: str, required: bool = True) -> float | None:
    value = obj.get(key)
    if value is None and not required:
        return None
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        raise Malformed(f"{key} missing or not a number")
    return float(value)


class Ingestor:
    def __init__(self, conn: sqlite3.Connection, prefix: str = "weather-station", throttle_s: int = 60) -> None:
        self.conn = conn
        self.prefix = prefix
        self.throttle_s = throttle_s
        self._last_warning: dict[tuple[str, str], float] = {}

    def topics(self) -> list[tuple[str, int]]:
        """FR-3.1: the four topics of every station, QoS 1."""
        return [(f"{self.prefix}/+/{kind}", 1) for kind in KINDS]

    def handle(self, topic: str, payload: bytes, retained: bool, now: int) -> list[dict]:
        """Stores one message and returns the events for the open pages."""
        parts = topic.split("/")
        if len(parts) != 3 or parts[0] != self.prefix or parts[2] not in KINDS:
            return []
        device_id, kind = parts[1], parts[2]
        if not DEVICE_ID_RE.match(device_id):
            self._warn(device_id, "device identifier not of the form ws-<12 hex digits>", topic)
            return []
        if not payload:
            # An empty retained message only deletes the retained one on the broker.
            if kind == "metar":
                self.conn.execute("DELETE FROM metar WHERE device_id = ?", (device_id,))
            return []
        try:
            self.conn.execute("BEGIN")
            try:
                events = getattr(self, "_" + kind)(device_id, payload, retained, now)
                self.conn.execute("COMMIT")
            except BaseException:
                self.conn.execute("ROLLBACK")
                raise
        except Malformed as e:
            self._warn(device_id, str(e), topic)
            return []
        return events

    # -- message types ------------------------------------------------------

    def _json(self, device_id: str, payload: bytes):
        try:
            obj = json.loads(payload.decode("utf-8"))
        except (UnicodeDecodeError, ValueError):
            raise Malformed("not JSON")
        if isinstance(obj, dict) and "device_id" in obj and obj["device_id"] != device_id:
            raise Malformed("device_id of the payload differs from the topic")
        return obj

    def _data(self, device_id: str, payload: bytes, retained: bool, now: int) -> list[dict]:
        obj = self._json(device_id, payload)
        if not isinstance(obj, dict):
            raise Malformed("not a JSON object")
        sensor_id = obj.get("sensor_id")
        if not isinstance(sensor_id, str) or not sensor_id or len(sensor_id) > 64:
            raise Malformed("sensor_id missing")
        temp = _number(obj, "temperature_c")
        hum = _number(obj, "humidity_rh")
        press = _number(obj, "pressure_hpa")
        if not derive.in_range(temp, hum, press):
            raise Malformed("value outside the sensor range")
        values = {"temperature_c": temp, "humidity_rh": hum, "pressure_hpa": press}
        for key in ("pressure_msl_hpa", "dew_point_c", "rssi_dbm"):
            values[key] = _number(obj, key, required=False)  # FR-3.9: older firmware
        if values["rssi_dbm"] is not None:
            values["rssi_dbm"] = int(values["rssi_dbm"])

        t = obj.get("time")
        time_from_receipt = not (isinstance(t, (int, float)) and not isinstance(t, bool) and t >= MIN_TIME)
        reading_time = now if time_from_receipt else int(t)

        db.touch_station(self.conn, device_id, now)
        db.touch_sensor(self.conn, device_id, sensor_id, now)
        # FR-3.4: a retained message without a time comes again on every
        # connection, each time with a new receive time; it is not stored again.
        stored = False
        if not (time_from_receipt and retained):
            stored = db.insert_reading(self.conn, device_id, sensor_id, reading_time, values, now,
                                       time_from_receipt)

        # FR-3.5: the latest reading keeps the payload as received (tendency, forecast).
        latest = dict(obj)
        latest.pop("device_id", None)
        latest["time_from_receipt"] = time_from_receipt
        cur = self.conn.execute(
            "INSERT INTO latest (device_id, sensor_id, time, received_at, payload_json) VALUES (?, ?, ?, ?, ?) "
            "ON CONFLICT (device_id, sensor_id) DO UPDATE SET time = excluded.time, "
            "received_at = excluded.received_at, payload_json = excluded.payload_json "
            "WHERE excluded.time > latest.time",
            (device_id, sensor_id, reading_time, now, json.dumps(latest, separators=(",", ":"))),
        )
        if not stored and cur.rowcount == 0:
            return []  # a repeated message: nothing new for the pages
        reading = dict(latest)
        reading["time"] = reading_time
        reading["received_at"] = now
        return [{"type": "reading", "device_id": device_id, "sensor_id": sensor_id, "reading": reading}]

    def _info(self, device_id: str, payload: bytes, retained: bool, now: int) -> list[dict]:
        obj = self._json(device_id, payload)
        if not isinstance(obj, dict):
            raise Malformed("not a JSON object")
        sensors = obj.get("sensors")
        if not isinstance(sensors, list):
            raise Malformed("sensors missing")

        def text(key, limit=64):
            v = obj.get(key)
            return v[:limit] if isinstance(v, str) else None

        def integer(key):
            v = obj.get(key)
            return int(v) if isinstance(v, (int, float)) and not isinstance(v, bool) else None

        db.touch_station(self.conn, device_id, now)
        row = self.conn.execute("SELECT info_json FROM stations WHERE device_id = ?", (device_id,)).fetchone()
        before = {}
        if row and row["info_json"]:
            try:
                for s in json.loads(row["info_json"]).get("sensors") or []:
                    before[s.get("index")] = s.get("id")
            except (ValueError, AttributeError):
                pass

        self.conn.execute(
            "UPDATE stations SET name = ?, firmware = ?, measure_period_s = ?, publish_period_s = ?, "
            "log_period_s = ?, altitude_m = ?, info_json = ?, info_time = ? WHERE device_id = ?",
            (text("name"), text("firmware"), integer("measure_period_s"), integer("publish_period_s"),
             integer("log_period_s"), integer("altitude_m"), json.dumps(obj, separators=(",", ":")), now,
             device_id),
        )
        for s in sensors:
            if not isinstance(s, dict) or not isinstance(s.get("id"), str) or not s["id"]:
                continue
            index = s.get("index") if isinstance(s.get("index"), int) else None
            # FR-3.8: a different sensor at a known position is a replacement.
            old = before.get(index)
            if index is not None and old and old != s["id"]:
                self.conn.execute(
                    "INSERT OR IGNORE INTO sensor_changes (device_id, idx, time, old_id, new_id) VALUES (?, ?, ?, ?, ?)",
                    (device_id, index, now, old, s["id"]),
                )
                log.warning("Sensor replaced on %s at position %s: %s -> %s", device_id, index, old, s["id"])
            db.touch_sensor(self.conn, device_id, s["id"], now)
            self.conn.execute(
                "UPDATE sensors SET type = ?, idx = ?, address = ?, channels_json = ? "
                "WHERE device_id = ? AND sensor_id = ?",
                (s.get("type") if isinstance(s.get("type"), str) else None, index,
                 s.get("address") if isinstance(s.get("address"), str) else None,
                 json.dumps(s.get("channels") or [], separators=(",", ":")), device_id, s["id"]),
            )
        return [{"type": "info", "device_id": device_id}]

    def _status(self, device_id: str, payload: bytes, retained: bool, now: int) -> list[dict]:
        status = payload.decode("utf-8", "replace").strip()
        if status not in ("online", "offline"):
            raise Malformed("status neither online nor offline")
        db.touch_station(self.conn, device_id, now)
        # A retained status that comes again on reconnection keeps the time it first arrived.
        self.conn.execute(
            "UPDATE stations SET status = ?, status_time = ? WHERE device_id = ? "
            "AND (status IS NOT ? OR status_time IS NULL OR ? = 0)",
            (status, now, device_id, status, 1 if retained else 0),
        )
        row = self.conn.execute("SELECT status_time FROM stations WHERE device_id = ?", (device_id,)).fetchone()
        return [{"type": "status", "device_id": device_id, "status": status, "time": row["status_time"]}]

    def _metar(self, device_id: str, payload: bytes, retained: bool, now: int) -> list[dict]:
        obj = self._json(device_id, payload)
        if obj is None:
            return []  # no sample yet
        if not isinstance(obj, dict) or not isinstance(obj.get("station"), str):
            raise Malformed("not a METAR check object")
        db.touch_station(self.conn, device_id, now)
        old = self.conn.execute("SELECT payload_json FROM metar WHERE device_id = ?", (device_id,)).fetchone()
        text = json.dumps(obj, separators=(",", ":"))
        if old and old["payload_json"] == text:
            return []  # the retained message again
        self.conn.execute(
            "INSERT INTO metar (device_id, received_at, payload_json) VALUES (?, ?, ?) "
            "ON CONFLICT (device_id) DO UPDATE SET received_at = excluded.received_at, "
            "payload_json = excluded.payload_json",
            (device_id, now, text),
        )
        return [{"type": "metar", "device_id": device_id}]

    def _warn(self, device_id: str, reason: str, topic: str) -> None:
        """FR-3.7: at most once per minute per station and reason."""
        key = (device_id, reason)
        t = time.monotonic()
        if t - self._last_warning.get(key, -self.throttle_s - 1) >= self.throttle_s:
            self._last_warning[key] = t
            log.warning("Dropped message on %s: %s", topic, reason)


def _payload_bytes(payload) -> bytes:
    if payload is None:
        return b""
    if isinstance(payload, (bytes, bytearray)):
        return bytes(payload)
    return str(payload).encode("utf-8")


async def run(cfg: Config, conn: sqlite3.Connection, broadcaster: Broadcaster) -> None:
    """Keeps the connection to the broker (NFR-2.1) and feeds the ingestor."""
    m = cfg.mqtt
    ingestor = Ingestor(conn, m.topic_prefix, cfg.log_throttle_s)
    delay = m.reconnect_min_s
    while True:
        try:
            async with aiomqtt.Client(
                hostname=m.host,
                port=m.port,
                username=m.username or None,
                password=m.password or None,
                identifier=m.client_id,
                clean_session=False,  # FR-3.2: the broker queues messages while we are away
                keepalive=m.keepalive_s,
            ) as client:
                await client.subscribe(ingestor.topics())
                log.info("Connected to the broker %s:%d as %s", m.host, m.port, m.client_id)
                broadcaster.set_mqtt_connected(True)
                delay = m.reconnect_min_s
                async for message in client.messages:
                    now = int(time.time())
                    try:
                        events = ingestor.handle(message.topic.value, _payload_bytes(message.payload),
                                                 bool(message.retain), now)
                    except Exception:
                        log.exception("Message on %s could not be stored", message.topic.value)
                        continue
                    for event in events:
                        broadcaster.publish(event)
        except aiomqtt.MqttError as e:
            log.warning("Broker %s:%d: %s; retrying in %.0f s", m.host, m.port, e, delay)
        finally:
            broadcaster.set_mqtt_connected(False)
        await asyncio.sleep(delay)
        delay = min(delay * 2, m.reconnect_max_s)
