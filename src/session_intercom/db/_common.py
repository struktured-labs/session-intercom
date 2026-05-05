"""Shared connection, schema, validation, and constants for the db package."""

from __future__ import annotations

import re
from datetime import UTC, datetime
from pathlib import Path

import aiosqlite

DB_DIR = Path.home() / ".local" / "share" / "session-intercom"
DB_PATH = DB_DIR / "intercom.db"

MAX_BODY_SIZE = 32768
NAME_RE = re.compile(r"^[a-zA-Z0-9][a-zA-Z0-9_-]{0,63}$")
STALE_MINUTES = 20160  # 2 weeks
CLEANUP_MINUTES = 20160  # 2 weeks

SCHEMA = """\
CREATE TABLE IF NOT EXISTS sessions (
    id TEXT PRIMARY KEY,
    name TEXT NOT NULL UNIQUE,
    created_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now')),
    last_heartbeat TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now')),
    metadata TEXT
);

CREATE TABLE IF NOT EXISTS channels (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    name TEXT NOT NULL UNIQUE,
    created_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now')),
    description TEXT
);

INSERT OR IGNORE INTO channels (name, description) VALUES ('general', 'Default broadcast channel');

CREATE TABLE IF NOT EXISTS messages (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    sender_id TEXT NOT NULL,
    recipient_id TEXT,
    channel TEXT,
    body TEXT NOT NULL,
    thread_id INTEGER,
    created_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now')),
    FOREIGN KEY (sender_id) REFERENCES sessions(id),
    FOREIGN KEY (recipient_id) REFERENCES sessions(id),
    FOREIGN KEY (thread_id) REFERENCES messages(id)
);

CREATE TABLE IF NOT EXISTS read_cursors (
    session_id TEXT NOT NULL,
    source TEXT NOT NULL,
    last_read_id INTEGER NOT NULL DEFAULT 0,
    PRIMARY KEY (session_id, source),
    FOREIGN KEY (session_id) REFERENCES sessions(id)
);

CREATE INDEX IF NOT EXISTS idx_messages_recipient ON messages(recipient_id, id);
CREATE INDEX IF NOT EXISTS idx_messages_channel ON messages(channel, id);
CREATE INDEX IF NOT EXISTS idx_messages_thread ON messages(thread_id, id);
CREATE INDEX IF NOT EXISTS idx_messages_sender ON messages(sender_id, id);
CREATE INDEX IF NOT EXISTS idx_sessions_heartbeat ON sessions(last_heartbeat);
CREATE INDEX IF NOT EXISTS idx_sessions_name ON sessions(name);
"""

MIGRATION_TEAM_NAME = """\
ALTER TABLE sessions ADD COLUMN team_name TEXT;
"""


def _now() -> str:
    return datetime.now(UTC).strftime("%Y-%m-%dT%H:%M:%S.%fZ")


def validate_name(name: str) -> None:
    if not NAME_RE.match(name):
        raise ValueError(
            f"Invalid session name '{name}': must be 1-64 chars, alphanumeric/hyphens/underscores, "
            "starting with alphanumeric"
        )


def validate_body(body: str) -> None:
    if len(body.encode("utf-8")) > MAX_BODY_SIZE:
        raise ValueError(f"Message body exceeds {MAX_BODY_SIZE} byte limit")


async def get_connection() -> aiosqlite.Connection:
    DB_DIR.mkdir(parents=True, exist_ok=True)
    db = await aiosqlite.connect(str(DB_PATH))
    await db.execute("PRAGMA journal_mode=WAL")
    await db.execute("PRAGMA busy_timeout=5000")
    await db.execute("PRAGMA foreign_keys=ON")
    db.row_factory = aiosqlite.Row
    return db


async def init_db() -> None:
    db = await get_connection()
    try:
        await db.executescript(SCHEMA)
        # Migrate: add team_name column if missing
        cursor = await db.execute("PRAGMA table_info(sessions)")
        columns = {row[1] for row in await cursor.fetchall()}
        if "team_name" not in columns:
            await db.executescript(MIGRATION_TEAM_NAME)
        await db.commit()
    finally:
        await db.close()
