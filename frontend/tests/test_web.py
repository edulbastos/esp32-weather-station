import gzip
import json

import pytest
from fastapi.testclient import TestClient

from conftest import DEV, INFO, T0, data_msg
from weather_frontend import __version__, auth, db
from weather_frontend.ingest import Ingestor
from weather_frontend.web import create_app, safe_next

PASSWORD = "correct horse battery"


@pytest.fixture
def client(cfg, conn):
    auth.add_user(conn, "alice", PASSWORD, cfg.password_min_length)
    ing = Ingestor(conn)
    ing.handle(f"weather-station/{DEV}/info", json.dumps(INFO).encode(), True, T0)
    ing.handle(f"weather-station/{DEV}/status", b"online", True, T0)
    for i in range(5):
        ing.handle(f"weather-station/{DEV}/data", data_msg(t=T0 + 60 * i), False, T0 + 60 * i)
    app = create_app(cfg, background=False)
    with TestClient(app, base_url="https://testserver", follow_redirects=False) as c:
        yield c


def login(client, user="alice", password=PASSWORD, **headers):
    return client.post("/login", data={"username": user, "password": password, "next": "/station/" + DEV},
                       headers=headers)


def test_everything_needs_a_session(client):
    for path in ("/api/stations", f"/api/stations/{DEV}", f"/api/stations/{DEV}/charts", "/api/me",
                 f"/stations/{DEV}/history.csv", "/api/events"):
        assert client.get(path).status_code == 401, path  # FR-1.2
    r = client.get("/station/" + DEV)
    assert r.status_code == 303 and r.headers["location"] == f"/login?next=/station/{DEV}"
    assert client.get("/login").status_code == 200
    assert client.get("/static/style.css").status_code == 200


def test_static_links_carry_the_version(client):
    body = client.get("/login").text  # FR-4.8 since v1.0.4
    assert f'href="/static/style.css?v={__version__}"' in body
    assert 'href="/static/style.css"' not in body
    assert client.get(f"/static/style.css?v={__version__}").status_code == 200


def test_login_and_logout(client):
    r = login(client)
    assert r.status_code == 303 and r.headers["location"] == "/station/" + DEV
    cookie = r.headers["set-cookie"]
    for attr in ("ws_session=", "HttpOnly", "Secure", "SameSite=lax", "Path=/"):
        assert attr.lower() in cookie.lower()  # FR-2.4
    d = client.get("/api/stations").json()
    assert d["user"] == "alice" and d["stations"][0]["device_id"] == DEV
    assert client.get("/").status_code == 200
    r = client.post("/logout")
    assert r.status_code == 303
    client.cookies.clear()
    assert client.get("/api/stations").status_code == 401


def test_session_deleted_on_server(client, conn):
    login(client)
    assert client.get("/api/me").status_code == 200
    auth.set_password(conn, "alice", "another long password", 10)  # FR-2.5: ends all sessions
    assert client.get("/api/me").status_code == 401


def test_wrong_password_and_rate_limit(client):
    r = login(client, password="wrong password")
    assert r.status_code == 401 and "Invalid user name or password" in r.text
    r = login(client, user="nobody")
    assert r.status_code == 401 and "Invalid user name or password" in r.text
    for _ in range(3):
        login(client, password="wrong password")
    r = login(client)  # 5 failures from this address: even the right password is refused (FR-2.6)
    assert r.status_code == 429 and "Too many attempts" in r.text


def test_forwarded_address_only_from_proxy(client):
    # The test client is not 127.0.0.1, so X-Forwarded-For is ignored and all failures count for one address.
    for i in range(5):
        login(client, user=f"user{i}", password="x" * 12, **{"X-Forwarded-For": f"10.0.0.{i}"})
    assert login(client).status_code == 429


def test_origin_check(client):
    r = login(client, Origin="https://evil.example")
    assert r.status_code == 403  # FR-2.7
    assert login(client, Origin="https://weather.example").status_code == 303


def test_next_is_local():
    assert safe_next("/station/x?a=1") == "/station/x?a=1"
    for bad in ("https://evil.example/", "//evil.example", "/\\evil.example", "javascript:alert(1)", "", None):
        assert safe_next(bad) == "/"


def test_security_headers(client):
    r = client.get("/login")
    for h in ("strict-transport-security", "x-content-type-options", "referrer-policy", "content-security-policy"):
        assert h in r.headers  # NFR-3.2
    assert "default-src 'self'" in r.headers["content-security-policy"]


def test_station_and_charts(client):
    login(client)
    st = client.get(f"/api/stations/{DEV}").json()["station"]
    assert st["name"] == "Varanda" and st["sensors"][0]["latest"]["time"] == T0 + 240
    assert client.get("/api/stations/ws-000000000000").status_code == 404
    charts = client.get(f"/api/stations/{DEV}/charts").json()
    assert [p["hours"] for p in charts["periods"]] == [1, 24, 168]
    assert [p["bucket_s"] for p in charts["periods"]] == [60, 900, 7200]
    h = client.get(f"/api/stations/{DEV}/history?hours=24&points=96")
    assert h.status_code == 200
    assert client.get(f"/api/stations/{DEV}/history?hours=0").status_code == 422


def test_csv(client):
    login(client)
    r = client.get(f"/stations/{DEV}/history.csv", headers={"Accept-Encoding": "identity"})
    assert r.status_code == 200 and f"history-{DEV}.csv" in r.headers["content-disposition"]
    lines = r.text.strip().split("\n")
    assert lines[0] == "device_id,sensor_id,time,temperature_c,humidity_rh,pressure_hpa,pressure_msl_hpa,dew_point_c,rssi_dbm"
    assert len(lines) == 6
    r = client.get(f"/stations/{DEV}/history.csv?from={T0 + 60}&to={T0 + 120}",
                   headers={"Accept-Encoding": "gzip"})
    assert r.headers["content-encoding"] == "gzip"
    assert len(r.text.strip().split("\n")) == 3  # httpx decompresses


def test_healthz_without_broker(client):
    r = client.get("/healthz")
    assert r.status_code == 503 and r.text == "broker not connected"
    client.app.state.broadcaster.mqtt_connected = True
    assert client.get("/healthz").text == "ok"
