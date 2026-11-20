import json
import os
import sys

import pytest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from weather_frontend import db  # noqa: E402
from weather_frontend.config import Config  # noqa: E402
from weather_frontend.ingest import Ingestor  # noqa: E402

DEV = "ws-a0b76551a842"
SENSOR = "bme280-3fa91c07"
T0 = 1_794_931_200  # a time with the station clock synchronized

INFO = {
    "device_id": DEV, "name": "Varanda", "firmware": "v2.8.1", "measure_period_s": 2,
    "publish_period_s": 60, "log_period_s": 60, "altitude_m": 760,
    "sensors": [{"id": SENSOR, "type": "bme280", "index": 0, "address": "i2c@0x76", "channels": [
        {"id": "temperature", "unit": "°C", "resolution": 0.01, "offset": 0.00},
        {"id": "humidity", "unit": "%RH", "resolution": 0.01, "offset": 0.00},
        {"id": "pressure", "unit": "hPa", "resolution": 0.1, "offset": -1.0}]}],
}


def data_msg(t=T0, sensor=SENSOR, **extra):
    msg = {"device_id": DEV, "sensor_id": sensor, "time": t, "temperature_c": 23.41, "humidity_rh": 56.12,
           "pressure_hpa": 921.40, "pressure_msl_hpa": 1012.95, "dew_point_c": 14.21, "rssi_dbm": -67}
    if t is None:
        del msg["time"]
    msg.update(extra)
    return json.dumps(msg).encode()


@pytest.fixture
def cfg(tmp_path):
    c = Config()
    c.db_path = str(tmp_path / "weather.db")
    c.backup_dir = str(tmp_path / "backup")
    c.public_origin = "https://weather.example"
    return c


@pytest.fixture
def conn(cfg):
    c = db.connect(cfg.db_path)
    yield c
    c.close()


@pytest.fixture
def ing(conn):
    return Ingestor(conn)
