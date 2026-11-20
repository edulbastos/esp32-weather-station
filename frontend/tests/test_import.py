import json

from conftest import DEV, INFO, SENSOR, T0
from weather_frontend import db
from weather_frontend.cli import import_csv
from weather_frontend.ingest import Ingestor


def test_import(tmp_path, conn):
    Ingestor(conn).handle(f"weather-station/{DEV}/info", json.dumps(INFO).encode(), True, T0)
    f = tmp_path / f"history-{DEV}.csv"
    f.write_text(
        "device_id,sensor_id,seq,time,temperature_c,humidity_rh,pressure_hpa\n"
        f"{DEV},{SENSOR},1,0,20.00,50.00,921.4\n"            # no time: skipped
        f"{DEV},{SENSOR},2,{T0},20.00,50.00,921.4\n"
        f"{DEV},{SENSOR},3,{T0 + 60},20.50,51.00,921.5\n"
        f"{DEV},{SENSOR},4,{T0 + 120},20.50,51.00,abc\n"     # invalid
    )
    c = import_csv(conn, str(f), T0 + 1000)
    assert (c["read"], c["stored"], c["duplicate"], c["invalid"]) == (4, 2, 0, 2)
    r = conn.execute("SELECT * FROM readings WHERE time = ?", (T0,)).fetchone()
    assert abs(r["pressure_msl_hpa"] - 1009.07) < 0.01  # altitude 760 m (station FR-11.2)
    assert abs(r["dew_point_c"] - 9.27) < 0.05       # Magnus (station FR-11.3)
    c = import_csv(conn, str(f), T0 + 2000)
    assert c["stored"] == 0 and c["duplicate"] == 2  # rows already stored are skipped
