"""Web application: pages, API, SSE stream and authentication (sections 3.1, 3.2, 3.4, 6.2)."""

from __future__ import annotations

import asyncio
import csv
import html
import io
import json
import logging
import re
import sqlite3
import threading
import time
import zlib
from contextlib import asynccontextmanager
from pathlib import Path
from urllib.parse import quote

from fastapi import FastAPI, HTTPException, Query, Request
from fastapi.responses import HTMLResponse, JSONResponse, PlainTextResponse, RedirectResponse, Response, StreamingResponse
from fastapi.staticfiles import StaticFiles
from starlette.concurrency import run_in_threadpool

from . import __version__, auth, db, ingest, maintenance
from .config import Config
from .events import Broadcaster

log = logging.getLogger(__name__)

HERE = Path(__file__).parent
PAGES = HERE / "pages"
STATIC_LINK = re.compile(r'\b(href|src)="/static/([^"?]+)"')
STATIC = HERE / "static"

DEVICE_ID_RE = re.compile(r"^ws-[0-9a-f]{12}$")

# NFR-3.2: only resources of the same origin; no inline script or style.
SECURITY_HEADERS = [
    (b"strict-transport-security", b"max-age=31536000"),
    (b"x-content-type-options", b"nosniff"),
    (b"referrer-policy", b"same-origin"),
    (b"x-frame-options", b"DENY"),
    (b"content-security-policy",
     b"default-src 'self'; script-src 'self'; style-src 'self'; img-src 'self' data:; connect-src 'self'; "
     b"base-uri 'none'; frame-ancestors 'none'; form-action 'self'"),
]


class SecurityHeaders:
    """Pure ASGI middleware, so the SSE stream is not buffered."""

    def __init__(self, app) -> None:
        self.app = app

    async def __call__(self, scope, receive, send):
        if scope["type"] != "http":
            return await self.app(scope, receive, send)

        async def send_with_headers(message):
            if message["type"] == "http.response.start":
                names = {k.lower() for k, _ in message.get("headers", [])}
                message["headers"] = list(message.get("headers", [])) + [
                    (k, v) for k, v in SECURITY_HEADERS if k not in names
                ]
            await send(message)

        await self.app(scope, receive, send_with_headers)


def safe_next(target: str | None) -> str:
    """FR-2.2: only a path on the same site."""
    if not target or not target.startswith("/") or target.startswith("//") or "\\" in target:
        return "/"
    if any(ord(c) < 0x20 for c in target):
        return "/"
    return target


def create_app(cfg: Config, broadcaster: Broadcaster | None = None, background: bool = True) -> FastAPI:
    broadcaster = broadcaster or Broadcaster()
    limiter = auth.RateLimiter(cfg.login_max_failures, cfg.login_window_s)
    local = threading.local()
    # FR-4.8 (since v1.0.4): the static files are linked with the version as a
    # query (`/static/style.css?v=1.0.4`), so every release reaches browsers
    # that keep an older copy without asking (the pages themselves are no-store).
    pages = {name: STATIC_LINK.sub(rf'\1="/static/\2?v={quote(__version__)}"',
                                   (PAGES / f"{name}.html").read_text(encoding="utf-8"))
             for name in ("login", "overview", "station")}

    def conn() -> sqlite3.Connection:
        c = getattr(local, "conn", None)
        if c is None:
            c = local.conn = db.open_conn(cfg.db_path)
        return c

    @asynccontextmanager
    async def lifespan(app: FastAPI):
        db.connect(cfg.db_path).close()  # creates or migrates the schema (NFR-4.3)
        tasks = []
        if background:
            ingest_conn = db.connect(cfg.db_path)
            tasks.append(asyncio.create_task(ingest.run(cfg, ingest_conn, broadcaster), name="ingest"))
            tasks.append(asyncio.create_task(maintenance.run(cfg, limiter), name="maintenance"))
            log.info("weather-frontend %s on %s:%d, public origin %s", __version__, cfg.listen_host,
                     cfg.listen_port, cfg.public_origin)
        try:
            yield
        finally:
            for t in tasks:
                t.cancel()
            await asyncio.gather(*tasks, return_exceptions=True)

    app = FastAPI(lifespan=lifespan, docs_url=None, redoc_url=None, openapi_url=None)
    app.state.broadcaster = broadcaster
    app.state.limiter = limiter
    app.add_middleware(SecurityHeaders)
    app.mount("/static", StaticFiles(directory=STATIC), name="static")

    # -- helpers -------------------------------------------------------------

    def client_address(request: Request) -> str:
        """FR-2.6: X-Forwarded-For only from the trusted proxy."""
        peer = request.client.host if request.client else "unknown"
        forwarded = request.headers.get("x-forwarded-for")
        if forwarded and peer in cfg.trusted_proxies:
            return forwarded.split(",")[-1].strip() or peer
        return peer

    def user_of(request: Request) -> str | None:
        return auth.session_user(conn(), request.cookies.get(cfg.cookie_name), cfg.session_days,
                                 cfg.session_renew_s, int(time.time()))

    def api_user(request: Request) -> str:
        user = user_of(request)
        if user is None:
            raise HTTPException(401, "Not logged in")
        return user

    def origin_ok(request: Request) -> bool:
        """FR-2.7: state-changing requests only from the public origin."""
        origin = request.headers.get("origin")
        return origin is None or origin.rstrip("/") == cfg.public_origin

    def set_session_cookie(response: Response, token: str) -> None:
        response.set_cookie(cfg.cookie_name, token, max_age=cfg.session_days * 86400, path="/",
                            secure=True, httponly=True, samesite="lax")

    def no_store(response: Response) -> Response:
        response.headers["cache-control"] = "no-store"
        return response

    def login_page(next_path: str, error: str = "", status: int = 200) -> HTMLResponse:
        body = (pages["login"]
                .replace("{{next}}", html.escape(next_path, quote=True))
                .replace("{{error}}", html.escape(error)))
        return no_store(HTMLResponse(body, status_code=status))

    def page(request: Request, name: str) -> Response:
        user = user_of(request)
        if user is None:
            target = request.url.path + (f"?{request.url.query}" if request.url.query else "")
            return RedirectResponse("/login?next=" + quote(target, safe="/"), status_code=303)
        response = no_store(HTMLResponse(pages[name]))
        # Keeps the cookie as long as the session on the server (FR-2.4).
        set_session_cookie(response, request.cookies[cfg.cookie_name])
        return response

    def station_or_404(device_id: str) -> dict:
        if not DEVICE_ID_RE.match(device_id):
            raise HTTPException(404, "Unknown station")
        st = db.get_station(conn(), device_id)
        if st is None:
            raise HTTPException(404, "Unknown station")
        return st

    # -- authentication (FR-2.x) ---------------------------------------------------

    @app.get("/login")
    def login_form(request: Request, next: str = "/"):
        if user_of(request):
            return RedirectResponse(safe_next(next), status_code=303)
        return login_page(safe_next(next))

    @app.post("/login")
    async def login(request: Request):
        if not origin_ok(request):
            return PlainTextResponse("Forbidden", status_code=403)
        form = await request.form()
        name = str(form.get("username", "")).strip()[:64]
        password = str(form.get("password", ""))[:1024]
        next_path = safe_next(str(form.get("next", "/")))
        address = client_address(request)

        def attempt():
            now = time.time()
            if limiter.blocked(name, address, now):
                return "blocked", None
            if not name or not auth.verify(conn(), name, password):
                limiter.failure(name, address, now)
                return "failed", None
            return "ok", auth.create_session(conn(), name, cfg.session_days, int(now))

        result, token = await run_in_threadpool(attempt)
        if result == "blocked":
            log.warning("Login refused (too many attempts) for %r from %s", name, address)
            return login_page(next_path, "Too many attempts, try again later", 429)
        if result == "failed":
            log.warning("Failed login for %r from %s", name, address)
            return login_page(next_path, "Invalid user name or password", 401)
        log.info("Login of %s from %s", name, address)
        response = RedirectResponse(next_path, status_code=303)
        set_session_cookie(response, token)
        return response

    @app.post("/logout")
    def logout(request: Request):
        if not origin_ok(request):
            return PlainTextResponse("Forbidden", status_code=403)
        auth.delete_session(conn(), request.cookies.get(cfg.cookie_name))
        response = RedirectResponse("/login", status_code=303)
        response.delete_cookie(cfg.cookie_name, path="/", secure=True, httponly=True, samesite="lax")
        return response

    # -- pages (FR-4.x) ---------------------------------------------------------

    @app.get("/")
    def overview(request: Request):
        return page(request, "overview")

    @app.get("/station/{device_id}")
    def station_page(request: Request, device_id: str):
        return page(request, "station")

    @app.get("/favicon.ico")
    def favicon():
        return Response(status_code=204)

    # -- API (section 6.2) ---------------------------------------------------------

    def envelope(user: str, **extra) -> dict:
        return {"server_time": int(time.time()), "mqtt_connected": broadcaster.mqtt_connected,
                "user": user, "version": __version__, **extra}

    @app.get("/api/me")
    def api_me(request: Request):
        return no_store(JSONResponse(envelope(api_user(request))))

    @app.get("/api/stations")
    def api_stations(request: Request):
        user = api_user(request)
        return no_store(JSONResponse(envelope(user, stations=db.list_stations(conn()))))

    @app.get("/api/stations/{device_id}")
    def api_station(request: Request, device_id: str):
        user = api_user(request)
        return no_store(JSONResponse(envelope(user, station=station_or_404(device_id))))

    @app.get("/api/stations/{device_id}/charts")
    def api_charts(request: Request, device_id: str):
        api_user(request)
        st = station_or_404(device_id)
        now = int(time.time())
        periods = [db.chart_period(conn(), st, h, p, now) for h, p in db.CHART_PERIODS]
        return no_store(JSONResponse({"server_time": now, "periods": periods}))

    @app.get("/api/stations/{device_id}/history")
    def api_history(request: Request, device_id: str, hours: int = Query(24, ge=1, le=8760),
                    points: int = Query(150, ge=10, le=1440)):
        api_user(request)
        st = station_or_404(device_id)
        return no_store(JSONResponse(db.chart_period(conn(), st, hours, points, int(time.time()))))

    @app.get("/stations/{device_id}/history.csv")
    def history_csv(request: Request, device_id: str, start: int | None = Query(None, alias="from"),
                    end: int | None = Query(None, alias="to")):
        if user_of(request) is None:
            raise HTTPException(401, "Not logged in")
        station_or_404(device_id)
        gzip = "gzip" in request.headers.get("accept-encoding", "")

        def rows():
            c = db.open_conn(cfg.db_path)
            try:
                buf = io.StringIO()
                writer = csv.writer(buf, lineterminator="\n")
                writer.writerow(db.CSV_COLUMNS)
                for i, row in enumerate(db.iter_csv_rows(c, device_id, start, end), 1):
                    writer.writerow(["" if v is None else v for v in row])
                    if i % 1000 == 0:
                        yield buf.getvalue()
                        buf.seek(0)
                        buf.truncate()
                yield buf.getvalue()
            finally:
                c.close()

        def compressed():
            z = zlib.compressobj(6, zlib.DEFLATED, 31)  # gzip container
            for chunk in rows():
                data = z.compress(chunk.encode("utf-8"))
                if data:
                    yield data
            yield z.flush()

        headers = {"content-disposition": f'attachment; filename="history-{device_id}.csv"',
                   "cache-control": "no-store", "vary": "Accept-Encoding"}
        if gzip:
            headers["content-encoding"] = "gzip"
            return StreamingResponse(compressed(), media_type="text/csv; charset=utf-8", headers=headers)
        return StreamingResponse((c.encode("utf-8") for c in rows()), media_type="text/csv; charset=utf-8",
                                 headers=headers)

    @app.get("/api/events")
    async def api_events(request: Request):
        if await run_in_threadpool(user_of, request) is None:
            raise HTTPException(401, "Not logged in")
        sub = broadcaster.subscribe()

        def sse(event: str, data: dict) -> str:
            return f"event: {event}\ndata: {json.dumps(data, separators=(',', ':'))}\n\n"

        async def stream():
            try:
                yield sse("hello", {"server_time": int(time.time()), "mqtt_connected": broadcaster.mqtt_connected})
                while True:
                    try:
                        event = await asyncio.wait_for(sub.queue.get(), timeout=cfg.sse_keepalive_s)
                    except asyncio.TimeoutError:
                        if sub.dropped or await run_in_threadpool(user_of, request) is None:
                            return  # too slow, or the session ended: the browser reconnects
                        yield ": keepalive\n\n"
                        continue
                    yield sse(event["type"], event)
                    if sub.dropped and sub.queue.empty():
                        return
            finally:
                broadcaster.unsubscribe(sub)

        return StreamingResponse(stream(), media_type="text/event-stream",
                                 headers={"cache-control": "no-store", "x-accel-buffering": "no"})

    @app.get("/healthz")
    def healthz():
        """NFR-2.4: no session needed, nothing revealed but the state."""
        try:
            conn().execute("SELECT 1 FROM meta LIMIT 1").fetchall()
        except sqlite3.Error:
            return PlainTextResponse("database not readable", status_code=503)
        if not broadcaster.mqtt_connected:
            return PlainTextResponse("broker not connected", status_code=503)
        return PlainTextResponse("ok")

    return app
