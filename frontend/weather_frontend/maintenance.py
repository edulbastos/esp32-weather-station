"""Periodic jobs: session clean-up, retention (FR-5.1) and backups (NFR-2.3)."""

from __future__ import annotations

import asyncio
import datetime as dt
import glob
import logging
import os
import time

from . import auth, db
from .config import Config

log = logging.getLogger(__name__)


def backup(cfg: Config) -> str:
    """Online copy of the database, keeping the newest `backup_keep` copies."""
    os.makedirs(cfg.backup_dir, exist_ok=True)
    stamp = dt.datetime.now().strftime("%Y%m%d-%H%M%S")
    path = os.path.join(cfg.backup_dir, f"weather-{stamp}.db")
    src = db.open_conn(cfg.db_path)
    dst = db.open_conn(path)
    try:
        src.backup(dst)
    finally:
        dst.close()
        src.close()
    copies = sorted(glob.glob(os.path.join(cfg.backup_dir, "weather-*.db")))
    for old in copies[: max(0, len(copies) - cfg.backup_keep)]:
        os.remove(old)
    return path


def apply_retention(cfg: Config, now: int) -> int:
    if cfg.retention_days <= 0:
        return 0
    conn = db.open_conn(cfg.db_path)
    try:
        return conn.execute("DELETE FROM readings WHERE time < ?", (now - cfg.retention_days * 86400,)).rowcount
    finally:
        conn.close()


def hourly(cfg: Config, limiter: auth.RateLimiter | None) -> None:
    now = int(time.time())
    conn = db.open_conn(cfg.db_path)
    try:
        ended = auth.purge_sessions(conn, now)
    finally:
        conn.close()
    if ended:
        log.info("Removed %d expired sessions", ended)
    if limiter is not None:
        limiter.prune(time.time())


def daily(cfg: Config) -> None:
    deleted = apply_retention(cfg, int(time.time()))
    if deleted:
        log.info("Retention of %d days: deleted %d readings", cfg.retention_days, deleted)
    path = backup(cfg)
    log.info("Backup written to %s", path)


def _seconds_until(hour: int) -> float:
    now = dt.datetime.now()
    target = now.replace(hour=hour, minute=30, second=0, microsecond=0)
    if target <= now:
        target += dt.timedelta(days=1)
    return (target - now).total_seconds()


async def run(cfg: Config, limiter: auth.RateLimiter | None = None) -> None:
    next_daily = time.monotonic() + _seconds_until(cfg.backup_hour)
    while True:
        await asyncio.sleep(min(3600, max(1.0, next_daily - time.monotonic())))
        try:
            await asyncio.to_thread(hourly, cfg, limiter)
            if time.monotonic() >= next_daily:
                await asyncio.to_thread(daily, cfg)
                next_daily = time.monotonic() + _seconds_until(cfg.backup_hour)
        except Exception:
            log.exception("Maintenance failed")
