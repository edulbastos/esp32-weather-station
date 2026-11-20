"""Users, sessions and the login rate limit (section 3.2)."""

from __future__ import annotations

import hashlib
import secrets
import sqlite3
import time
from collections import defaultdict, deque

from argon2 import PasswordHasher
from argon2.exceptions import InvalidHashError, VerificationError

_hasher = PasswordHasher()  # FR-2.3: Argon2id with the argon2-cffi defaults
# Verified when the user does not exist, so both failures take the same time.
_DUMMY_HASH = _hasher.hash(secrets.token_hex(16))


class UserError(ValueError):
    pass


# -- users (FR-2.8) -----------------------------------------------------------


def check_password_rules(password: str, min_length: int) -> None:
    if len(password) < min_length:
        raise UserError(f"the password needs at least {min_length} characters")


def check_name(name: str) -> None:
    if not name or len(name) > 64 or not all(c.isalnum() or c in "._-@" for c in name):
        raise UserError("a user name has 1 to 64 letters, digits or . _ - @")


def add_user(conn: sqlite3.Connection, name: str, password: str, min_length: int) -> None:
    check_name(name)
    check_password_rules(password, min_length)
    try:
        conn.execute("INSERT INTO users (name, hash, created) VALUES (?, ?, ?)",
                     (name, _hasher.hash(password), int(time.time())))
    except sqlite3.IntegrityError:
        raise UserError(f"user {name} exists already")


def set_password(conn: sqlite3.Connection, name: str, password: str, min_length: int) -> int:
    """Changes a password and ends every session of the user (FR-2.5); returns the sessions ended."""
    check_password_rules(password, min_length)
    conn.execute("BEGIN")
    try:
        cur = conn.execute("UPDATE users SET hash = ? WHERE name = ?", (_hasher.hash(password), name))
        if cur.rowcount == 0:
            raise UserError(f"no user {name}")
        ended = conn.execute("DELETE FROM sessions WHERE user = ?", (name,)).rowcount
        conn.execute("COMMIT")
    except BaseException:
        conn.execute("ROLLBACK")
        raise
    return ended


def delete_user(conn: sqlite3.Connection, name: str) -> None:
    conn.execute("BEGIN")
    try:
        conn.execute("DELETE FROM sessions WHERE user = ?", (name,))
        if conn.execute("DELETE FROM users WHERE name = ?", (name,)).rowcount == 0:
            raise UserError(f"no user {name}")
        conn.execute("COMMIT")
    except BaseException:
        conn.execute("ROLLBACK")
        raise


def list_users(conn: sqlite3.Connection, now: int) -> list[dict]:
    rows = conn.execute(
        "SELECT u.name, u.created, u.last_login, "
        "(SELECT COUNT(*) FROM sessions s WHERE s.user = u.name AND s.expires > ?) AS sessions "
        "FROM users u ORDER BY u.name",
        (now,),
    )
    return [dict(r) for r in rows]


def verify(conn: sqlite3.Connection, name: str, password: str) -> bool:
    row = conn.execute("SELECT hash FROM users WHERE name = ?", (name,)).fetchone()
    try:
        _hasher.verify(row["hash"] if row else _DUMMY_HASH, password)
    except (VerificationError, InvalidHashError):
        return False
    if row is None:
        return False
    if _hasher.check_needs_rehash(row["hash"]):
        conn.execute("UPDATE users SET hash = ? WHERE name = ?", (_hasher.hash(password), name))
    return True


# -- sessions (FR-2.4) -------------------------------------------------------------


def _token_hash(token: str) -> str:
    return hashlib.sha256(token.encode("utf-8")).hexdigest()


def create_session(conn: sqlite3.Connection, name: str, days: int, now: int) -> str:
    token = secrets.token_urlsafe(32)  # 256 bits
    conn.execute(
        "INSERT INTO sessions (token_hash, user, created, last_used, expires) VALUES (?, ?, ?, ?, ?)",
        (_token_hash(token), name, now, now, now + days * 86400),
    )
    conn.execute("UPDATE users SET last_login = ? WHERE name = ?", (now, name))
    return token


def session_user(conn: sqlite3.Connection, token: str | None, days: int, renew_s: int, now: int) -> str | None:
    """The user of a valid session, renewing it at most once per `renew_s`."""
    if not token or len(token) > 100:
        return None
    h = _token_hash(token)
    row = conn.execute("SELECT user, last_used, expires FROM sessions WHERE token_hash = ?", (h,)).fetchone()
    if row is None:
        return None
    if row["expires"] <= now:
        conn.execute("DELETE FROM sessions WHERE token_hash = ?", (h,))
        return None
    if now - row["last_used"] >= renew_s:
        conn.execute("UPDATE sessions SET last_used = ?, expires = ? WHERE token_hash = ?",
                     (now, now + days * 86400, h))
    return row["user"]


def delete_session(conn: sqlite3.Connection, token: str | None) -> None:
    if token:
        conn.execute("DELETE FROM sessions WHERE token_hash = ?", (_token_hash(token),))


def purge_sessions(conn: sqlite3.Connection, now: int) -> int:
    return conn.execute("DELETE FROM sessions WHERE expires <= ?", (now,)).rowcount


# -- rate limit (FR-2.6) -------------------------------------------------------------


class RateLimiter:
    """Failed logins per user name and per client address, kept in memory."""

    def __init__(self, max_failures: int, window_s: int) -> None:
        self.max_failures = max_failures
        self.window_s = window_s
        self._failures: dict[str, deque] = defaultdict(deque)

    def _count(self, key: str, now: float) -> int:
        q = self._failures.get(key)
        if not q:
            return 0
        while q and q[0] <= now - self.window_s:
            q.popleft()
        if not q:
            del self._failures[key]
            return 0
        return len(q)

    def blocked(self, name: str, address: str, now: float) -> bool:
        return (self._count("u:" + name.casefold(), now) >= self.max_failures
                or self._count("a:" + address, now) >= self.max_failures)

    def prune(self, now: float) -> None:
        for key in list(self._failures):
            self._count(key, now)

    def failure(self, name: str, address: str, now: float) -> None:
        self._failures["u:" + name.casefold()].append(now)
        self._failures["a:" + address].append(now)
