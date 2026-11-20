import json

from conftest import DEV, INFO, SENSOR, T0, data_msg
from weather_frontend import db

TOPIC = f"weather-station/{DEV}/"


def test_data_stored_once(ing, conn):
    ev = ing.handle(TOPIC + "data", data_msg(), False, T0 + 1)
    assert ev and ev[0]["type"] == "reading" and ev[0]["sensor_id"] == SENSOR
    # QoS 1 duplicate and the retained copy on reconnection: no new reading, no event (FR-3.4)
    assert ing.handle(TOPIC + "data", data_msg(), False, T0 + 2) == []
    assert ing.handle(TOPIC + "data", data_msg(), True, T0 + 100) == []
    rows = conn.execute("SELECT * FROM readings").fetchall()
    assert len(rows) == 1 and rows[0]["pressure_msl_hpa"] == 1012.95 and rows[0]["rssi_dbm"] == -67
    st = conn.execute("SELECT * FROM stations").fetchone()
    assert st["device_id"] == DEV  # discovered automatically (FR-3.3)


def test_tendency_only_in_latest(ing, conn):
    ing.handle(TOPIC + "data", data_msg(pressure_trend_hpa=-1.82, pressure_trend="falling", forecast_code="O",
                                        forecast="Showery, becoming less settled"), False, T0)
    latest = json.loads(conn.execute("SELECT payload_json FROM latest").fetchone()[0])
    assert latest["forecast_code"] == "O" and latest["pressure_trend"] == "falling"
    cols = [r[1] for r in conn.execute("PRAGMA table_info(readings)")]
    assert "forecast" not in cols  # FR-3.5


def test_old_firmware_fields_missing(ing, conn):
    msg = json.dumps({"device_id": DEV, "sensor_id": SENSOR, "time": T0, "temperature_c": 20.0,
                      "humidity_rh": 50.0, "pressure_hpa": 1000.0}).encode()
    assert ing.handle(TOPIC + "data", msg, False, T0)
    r = conn.execute("SELECT * FROM readings").fetchone()
    assert r["pressure_msl_hpa"] is None and r["dew_point_c"] is None and r["rssi_dbm"] is None  # FR-3.9


def test_without_time(ing, conn):
    assert ing.handle(TOPIC + "data", data_msg(t=None), False, T0 + 5)
    r = conn.execute("SELECT * FROM readings").fetchone()
    assert r["time"] == T0 + 5 and r["time_from_receipt"] == 1
    # the retained copy of a message without time is not stored again with a new receive time
    ing.handle(TOPIC + "data", data_msg(t=None), True, T0 + 500)
    assert conn.execute("SELECT COUNT(*) FROM readings").fetchone()[0] == 1


def test_malformed_dropped(ing, conn):
    assert ing.handle(TOPIC + "data", b"not json", False, T0) == []
    assert ing.handle(TOPIC + "data", data_msg(device_id="ws-000000000000"), False, T0) == []
    assert ing.handle(TOPIC + "data", data_msg(temperature_c=150.0), False, T0) == []  # station FR-1.6
    assert ing.handle(TOPIC + "data", data_msg(sensor_id=None), False, T0) == []
    assert ing.handle("weather-station/NotAnId/data", data_msg(), False, T0) == []
    assert ing.handle(TOPIC + "status", b"maybe", False, T0) == []
    assert conn.execute("SELECT COUNT(*) FROM readings").fetchone()[0] == 0
    assert conn.execute("SELECT COUNT(*) FROM stations").fetchone()[0] == 0
    # the ingestion goes on after malformed messages (FR-3.7)
    assert ing.handle(TOPIC + "data", data_msg(), False, T0)


def test_info_and_groups(ing, conn):
    ing.handle(TOPIC + "info", json.dumps(INFO).encode(), True, T0)
    ing.handle(TOPIC + "data", data_msg(), False, T0)
    st = db.get_station(conn, DEV)
    assert st["name"] == "Varanda" and st["altitude_m"] == 760 and st["publish_period_s"] == 60
    assert len(st["sensors"]) == 1
    g = st["sensors"][0]
    assert g["key"] == "p0" and g["sensor_id"] == SENSOR and g["latest"]["temperature_c"] == 23.41
    assert g["channels"][2]["offset"] == -1.0


def test_sensor_replacement(ing, conn):
    ing.handle(TOPIC + "info", json.dumps(INFO).encode(), True, T0)
    ing.handle(TOPIC + "data", data_msg(t=T0), False, T0)
    new = json.loads(json.dumps(INFO))
    new["sensors"][0]["id"] = "bme280-91aa0e33"
    ing.handle(TOPIC + "info", json.dumps(new).encode(), False, T0 + 60)
    ing.handle(TOPIC + "data", data_msg(t=T0 + 120, sensor="bme280-91aa0e33"), False, T0 + 120)
    st = db.get_station(conn, DEV)
    assert len(st["sensors"]) == 1  # one position, one card (FR-3.8)
    g = st["sensors"][0]
    assert g["sensor_id"] == "bme280-91aa0e33" and set(g["sensor_ids"]) == {SENSOR, "bme280-91aa0e33"}
    assert st["sensor_changes"][0]["old_id"] == SENSOR
    # the chart of the position continues across the replacement
    p = db.chart_period(conn, st, 1, 60, T0 + 130)
    assert len(p["sensors"][0]["records"]) == 2


def test_status(ing, conn):
    ev = ing.handle(TOPIC + "status", b"online", True, T0)
    assert ev[0]["status"] == "online" and ev[0]["time"] == T0
    # the retained status again on reconnection keeps its first time
    ev = ing.handle(TOPIC + "status", b"online", True, T0 + 500)
    assert ev[0]["time"] == T0
    ev = ing.handle(TOPIC + "status", b"offline", False, T0 + 600)
    assert ev[0]["status"] == "offline" and ev[0]["time"] == T0 + 600


def test_metar(ing, conn):
    m = {"station": "SBGR", "obs_time": T0, "qnh_ref_hpa": 1025.5, "qnh_hpa": 1026.3, "diff_hpa": 0.8,
         "median_diff_hpa": None, "samples": 3, "suggested_offset_hpa": None, "suggested_altitude_m": None}
    assert ing.handle(TOPIC + "metar", json.dumps(m).encode(), True, T0)
    assert ing.handle(TOPIC + "metar", json.dumps(m).encode(), True, T0 + 9) == []  # same retained message
    assert db.get_station(conn, DEV)["metar"]["station"] == "SBGR"
    ing.handle(TOPIC + "metar", b"", True, T0 + 10)  # cleared on the broker
    assert db.get_station(conn, DEV)["metar"] is None


def test_chart_buckets(ing, conn):
    ing.handle(TOPIC + "info", json.dumps(INFO).encode(), True, T0)
    now = T0 + 3600
    # two readings in one 15-minute bucket, one in the next
    for t, temp in ((now - 1800, 20.0), (now - 1790, 22.0), (now - 600, 25.0)):
        ing.handle(TOPIC + "data", data_msg(t=t, temperature_c=temp), False, t)
    st = db.get_station(conn, DEV)
    p = db.chart_period(conn, st, 24, 96, now)
    assert p["bucket_s"] == 900 and p["available"]
    recs = p["sensors"][0]["records"]
    temps = [r[1] for r in recs]
    assert 21.0 in temps and 25.0 in temps
    assert recs[0][3] == 1012.95  # sea-level pressure as received
    # a reading older than the period is left out
    p1 = db.chart_period(conn, st, 1, 60, now + 2900)
    assert [r[1] for r in p1["sensors"][0]["records"]] == [25.0]
