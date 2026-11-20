"""Command-line tool (section 6.4): `weather-frontend [--config FILE] <command>`."""

from __future__ import annotations

import argparse
import csv
import datetime as dt
import getpass
import logging
import sys
import time

from . import __version__, auth, db, derive, maintenance
from .config import Config, load
from .ingest import DEVICE_ID_RE, MIN_TIME

IMPORT_REQUIRED = ("device_id", "sensor_id", "time", "temperature_c", "humidity_rh", "pressure_hpa")
IMPORT_BATCH = 5000


def _read_password(cfg: Config) -> str:
    """FR-2.8: from the terminal without echo, never from the command line."""
    first = getpass.getpass("Password: ")
    auth.check_password_rules(first, cfg.password_min_length)
    if getpass.getpass("Repeat the password: ") != first:
        raise auth.UserError("the passwords differ")
    return first


def _when(t: int | None) -> str:
    return dt.datetime.fromtimestamp(t).strftime("%Y-%m-%d %H:%M") if t else "never"


def cmd_serve(cfg: Config, args) -> int:
    import uvicorn

    from .web import create_app

    app = create_app(cfg)
    uvicorn.run(app, host=cfg.listen_host, port=cfg.listen_port, proxy_headers=False, access_log=False,
                log_config=None, timeout_graceful_shutdown=5)
    return 0


def cmd_user(cfg: Config, args) -> int:
    conn = db.connect(cfg.db_path)
    try:
        if args.action == "add":
            auth.check_name(args.name)
            auth.add_user(conn, args.name, _read_password(cfg), cfg.password_min_length)
            print(f"User {args.name} added")
        elif args.action == "passwd":
            ended = auth.set_password(conn, args.name, _read_password(cfg), cfg.password_min_length)
            print(f"Password of {args.name} changed, {ended} session(s) ended")
        elif args.action == "del":
            auth.delete_user(conn, args.name)
            print(f"User {args.name} deleted, sessions ended")
        else:
            users = auth.list_users(conn, int(time.time()))
            if not users:
                print("No users")
            for u in users:
                print(f"{u['name']:<24} created {_when(u['created'])}  last login {_when(u['last_login'])}  "
                      f"sessions {u['sessions']}")
    except auth.UserError as e:
        print(f"Error: {e}", file=sys.stderr)
        return 1
    finally:
        conn.close()
    return 0


def import_csv(conn, path: str, now: int) -> dict:
    """FR-5.4: a history.csv of a station (station FR-10.6 format)."""
    counts = {"read": 0, "stored": 0, "duplicate": 0, "invalid": 0}
    altitudes: dict[str, int | None] = {}
    with open(path, newline="", encoding="utf-8") as f:
        reader = csv.DictReader(f)
        missing = [c for c in IMPORT_REQUIRED if c not in (reader.fieldnames or [])]
        if missing:
            raise ValueError(f"columns missing: {', '.join(missing)}")
        conn.execute("BEGIN")
        try:
            for row in reader:
                counts["read"] += 1
                try:
                    device_id, sensor_id = row["device_id"].strip(), row["sensor_id"].strip()
                    t = int(row["time"])
                    temp, hum, press = (float(row[k]) for k in ("temperature_c", "humidity_rh", "pressure_hpa"))
                except (ValueError, TypeError, AttributeError):
                    counts["invalid"] += 1
                    continue
                if (not DEVICE_ID_RE.match(device_id) or not sensor_id or t < MIN_TIME
                        or not derive.in_range(temp, hum, press)):
                    counts["invalid"] += 1  # rows without a time are skipped
                    continue
                if device_id not in altitudes:
                    r = conn.execute("SELECT altitude_m FROM stations WHERE device_id = ?", (device_id,)).fetchone()
                    altitudes[device_id] = r["altitude_m"] if r else None
                    db.touch_station(conn, device_id, now)
                db.touch_sensor(conn, device_id, sensor_id, now)
                alt = altitudes[device_id]
                dew = derive.dew_point(temp, hum)
                values = {
                    "temperature_c": temp,
                    "humidity_rh": hum,
                    "pressure_hpa": press,
                    "pressure_msl_hpa": round(press * derive.msl_factor(alt), 2) if alt is not None else None,
                    "dew_point_c": round(dew, 2) if dew is not None else None,
                }
                if db.insert_reading(conn, device_id, sensor_id, t, values, None, False):
                    counts["stored"] += 1
                else:
                    counts["duplicate"] += 1
                if counts["read"] % IMPORT_BATCH == 0:
                    conn.execute("COMMIT")
                    conn.execute("BEGIN")
            conn.execute("COMMIT")
        except BaseException:
            conn.execute("ROLLBACK")
            raise
    counts["no_altitude"] = sorted(d for d, a in altitudes.items() if a is None)
    return counts


def cmd_import(cfg: Config, args) -> int:
    conn = db.connect(cfg.db_path)
    try:
        c = import_csv(conn, args.file, int(time.time()))
    except (OSError, ValueError) as e:
        print(f"Error: {e}", file=sys.stderr)
        return 1
    finally:
        conn.close()
    print(f"{c['read']} rows read, {c['stored']} stored, {c['duplicate']} already stored, "
          f"{c['invalid']} skipped (no time or invalid)")
    for d in c["no_altitude"]:
        print(f"Note: no info message of {d} yet, so its altitude is unknown; the charts reduce the "
              f"imported pressure to sea level with the altitude of its next info message.")
    return 0


def cmd_backup(cfg: Config, args) -> int:
    db.connect(cfg.db_path).close()
    print(f"Backup written to {maintenance.backup(cfg)}")
    return 0


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(prog="weather-frontend", description="Weather Station Frontend")
    parser.add_argument("--config", help="configuration file (default: $WEATHER_FRONTEND_CONFIG or "
                                         "/etc/weather-frontend/config.toml)")
    parser.add_argument("--version", action="version", version=f"%(prog)s {__version__}")
    sub = parser.add_subparsers(dest="command", required=True)
    sub.add_parser("serve", help="run the service (MQTT ingestion and web server)")
    user = sub.add_parser("user", help="manage the user accounts")
    user_sub = user.add_subparsers(dest="action", required=True)
    for action, text in (("add", "add a user"), ("passwd", "change a password and end its sessions"),
                         ("del", "delete a user and end its sessions")):
        user_sub.add_parser(action, help=text).add_argument("name")
    user_sub.add_parser("list", help="list the users")
    imp = sub.add_parser("import", help="import a history.csv downloaded from a station")
    imp.add_argument("file")
    sub.add_parser("backup", help="write a backup of the database now")
    args = parser.parse_args(argv)

    logging.basicConfig(level=logging.INFO, format="%(levelname)s %(name)s: %(message)s")
    try:
        cfg = load(args.config)
    except (OSError, ValueError) as e:
        print(f"Configuration: {e}", file=sys.stderr)
        return 2
    return {"serve": cmd_serve, "user": cmd_user, "import": cmd_import, "backup": cmd_backup}[args.command](
        cfg, args)
