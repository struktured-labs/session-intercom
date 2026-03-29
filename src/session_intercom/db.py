from __future__ import annotations

import re
import uuid
from datetime import datetime, timezone
from pathlib import Path

import aiosqlite

from .inbox import write_to_inbox
from .models import Channel, Message, Session

DB_DIR = Path.home() / ".local" / "share" / "session-intercom"
DB_PATH = DB_DIR / "intercom.db"

MAX_BODY_SIZE = 32768
NAME_RE = re.compile(r"^[a-zA-Z0-9][a-zA-Z0-9_-]{0,63}$")
STALE_MINUTES = 10
CLEANUP_MINUTES = 30

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
    return datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%S.%fZ")


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


# --- Sessions ---


async def register_session(
    name: str, metadata: str | None = None, team_name: str | None = None
) -> Session:
    validate_name(name)
    if team_name is not None:
        validate_name(team_name)
    db = await get_connection()
    try:
        # Check if name already taken
        row = await db.execute_fetchall(
            "SELECT id, last_heartbeat FROM sessions WHERE name = ?", (name,)
        )
        if row:
            hb = row[0]["last_heartbeat"]
            # Check if stale
            cursor = await db.execute(
                "SELECT strftime('%s', 'now') - strftime('%s', ?) AS age_seconds", (hb,)
            )
            age_row = await cursor.fetchone()
            age_seconds = age_row["age_seconds"] if age_row else 0
            if age_seconds < STALE_MINUTES * 60:
                raise ValueError(
                    f"Session name '{name}' is already registered and active "
                    f"(last heartbeat {age_seconds}s ago). Choose a different name."
                )
            # Replace stale session — clear FK references first
            old_id = row[0]["id"]
            await db.execute("DELETE FROM read_cursors WHERE session_id = ?", (old_id,))
            await db.execute("UPDATE messages SET recipient_id = NULL WHERE recipient_id = ?", (old_id,))
            # Detach thread replies pointing at messages we're about to delete
            await db.execute(
                "UPDATE messages SET thread_id = NULL WHERE thread_id IN "
                "(SELECT id FROM messages WHERE sender_id = ?)", (old_id,)
            )
            await db.execute("DELETE FROM messages WHERE sender_id = ?", (old_id,))
            await db.execute("DELETE FROM sessions WHERE id = ?", (old_id,))

        session_id = str(uuid.uuid4())
        now = _now()
        await db.execute(
            "INSERT INTO sessions (id, name, created_at, last_heartbeat, metadata, team_name)"
            " VALUES (?, ?, ?, ?, ?, ?)",
            (session_id, name, now, now, metadata, team_name),
        )
        await db.commit()
        return Session(
            id=session_id, name=name, created_at=now,
            last_heartbeat=now, metadata=metadata, team_name=team_name,
        )
    finally:
        await db.close()


async def heartbeat(name: str) -> Session:
    db = await get_connection()
    try:
        now = _now()
        await db.execute(
            "UPDATE sessions SET last_heartbeat = ? WHERE name = ?", (now, name)
        )
        await db.commit()
        row = await db.execute_fetchall("SELECT * FROM sessions WHERE name = ?", (name,))
        if not row:
            raise ValueError(f"Session '{name}' not found")
        r = row[0]
        return Session(id=r["id"], name=r["name"], created_at=r["created_at"],
                       last_heartbeat=r["last_heartbeat"], metadata=r["metadata"],
                       team_name=r["team_name"])
    finally:
        await db.close()


async def list_sessions(include_stale: bool = False) -> list[Session]:
    db = await get_connection()
    try:
        # Opportunistic cleanup
        await _cleanup_stale(db, CLEANUP_MINUTES)
        if include_stale:
            rows = await db.execute_fetchall("SELECT * FROM sessions ORDER BY name")
        else:
            rows = await db.execute_fetchall(
                "SELECT * FROM sessions WHERE last_heartbeat > datetime('now', ?)",
                (f"-{STALE_MINUTES} minutes",),
            )
        return [
            Session(id=r["id"], name=r["name"], created_at=r["created_at"],
                    last_heartbeat=r["last_heartbeat"], metadata=r["metadata"],
                    team_name=r["team_name"])
            for r in rows
        ]
    finally:
        await db.close()


async def get_session_by_name(db: aiosqlite.Connection, name: str) -> dict:
    rows = await db.execute_fetchall("SELECT * FROM sessions WHERE name = ?", (name,))
    if not rows:
        raise ValueError(f"Session '{name}' not found. Register first with intercom_register.")
    return dict(rows[0])


# --- Messages ---


async def send_message(
    from_name: str, to_name: str, body: str, thread_id: int | None = None
) -> Message:
    validate_body(body)
    db = await get_connection()
    try:
        sender = await get_session_by_name(db, from_name)
        recipient = await get_session_by_name(db, to_name)

        if thread_id is not None:
            rows = await db.execute_fetchall("SELECT id FROM messages WHERE id = ?", (thread_id,))
            if not rows:
                raise ValueError(f"Thread root message {thread_id} not found")

        cursor = await db.execute(
            "INSERT INTO messages (sender_id, recipient_id, body, thread_id) VALUES (?, ?, ?, ?)",
            (sender["id"], recipient["id"], body, thread_id),
        )
        msg_id = cursor.lastrowid
        await db.commit()

        row = await db.execute_fetchall("SELECT created_at FROM messages WHERE id = ?", (msg_id,))
        created_at = row[0]["created_at"] if row else _now()

        # Bridge to native inbox if recipient has a team
        if recipient.get("team_name"):
            write_to_inbox(
                recipient["team_name"], from_name, body,
                summary=f"intercom DM from {from_name}",
            )

        return Message(
            id=msg_id, sender_id=sender["id"], sender_name=from_name,
            recipient_id=recipient["id"], recipient_name=to_name,
            body=body, thread_id=thread_id, created_at=created_at,
        )
    finally:
        await db.close()


async def broadcast_message(
    from_name: str, body: str, channel: str = "general", thread_id: int | None = None
) -> Message:
    validate_body(body)
    db = await get_connection()
    try:
        sender = await get_session_by_name(db, from_name)

        # Verify channel exists
        rows = await db.execute_fetchall("SELECT name FROM channels WHERE name = ?", (channel,))
        if not rows:
            raise ValueError(f"Channel '{channel}' not found. Create it with intercom_create_channel.")

        if thread_id is not None:
            rows = await db.execute_fetchall("SELECT id FROM messages WHERE id = ?", (thread_id,))
            if not rows:
                raise ValueError(f"Thread root message {thread_id} not found")

        cursor = await db.execute(
            "INSERT INTO messages (sender_id, channel, body, thread_id) VALUES (?, ?, ?, ?)",
            (sender["id"], channel, body, thread_id),
        )
        msg_id = cursor.lastrowid
        await db.commit()

        row = await db.execute_fetchall("SELECT created_at FROM messages WHERE id = ?", (msg_id,))
        created_at = row[0]["created_at"] if row else _now()

        # Bridge broadcast to native inboxes for all sessions with teams (except sender)
        team_rows = await db.execute_fetchall(
            "SELECT team_name FROM sessions WHERE team_name IS NOT NULL AND id != ?",
            (sender["id"],),
        )
        for tr in team_rows:
            write_to_inbox(
                tr["team_name"], from_name, body,
                summary=f"[{channel}] {from_name}",
            )

        return Message(
            id=msg_id, sender_id=sender["id"], sender_name=from_name,
            channel=channel, body=body, thread_id=thread_id, created_at=created_at,
        )
    finally:
        await db.close()


async def poll_messages(
    name: str, mark_read: bool = True, limit: int = 50, channel: str | None = None
) -> tuple[list[Message], int]:
    limit = min(max(limit, 1), 200)
    db = await get_connection()
    try:
        session = await get_session_by_name(db, name)
        session_id = session["id"]

        # Update heartbeat
        await db.execute(
            "UPDATE sessions SET last_heartbeat = ? WHERE id = ?", (_now(), session_id)
        )

        # Opportunistic cleanup
        await _cleanup_stale(db, CLEANUP_MINUTES)

        messages: list[Message] = []

        if channel:
            # Channel messages only
            messages.extend(await _poll_channel(db, session_id, channel, limit))
        else:
            # DMs addressed to this session
            messages.extend(await _poll_dms(db, session_id, limit))
            # Channel messages (all channels)
            messages.extend(await _poll_all_channels(db, session_id, limit))

        # Sort by id (monotonic order) and apply limit
        messages.sort(key=lambda m: m.id)
        result = messages[:limit]
        remaining = len(messages) - len(result)

        if mark_read and result:
            await _advance_cursors(db, session_id, result)

        await db.commit()
        return result, remaining
    finally:
        await db.close()


async def _poll_dms(
    db: aiosqlite.Connection, session_id: str, limit: int
) -> list[Message]:
    # Get cursor for each sender
    rows = await db.execute_fetchall(
        """
        SELECT m.*, s.name AS sender_name
        FROM messages m
        JOIN sessions s ON s.id = m.sender_id
        WHERE m.recipient_id = ? AND m.channel IS NULL
          AND m.id > COALESCE(
            (SELECT last_read_id FROM read_cursors WHERE session_id = ? AND source = m.sender_id), 0
          )
        ORDER BY m.id
        LIMIT ?
        """,
        (session_id, session_id, limit),
    )
    return [_row_to_message(r) for r in rows]


async def _poll_channel(
    db: aiosqlite.Connection, session_id: str, channel: str, limit: int
) -> list[Message]:
    rows = await db.execute_fetchall(
        """
        SELECT m.*, s.name AS sender_name
        FROM messages m
        JOIN sessions s ON s.id = m.sender_id
        WHERE m.channel = ? AND m.sender_id != ?
          AND m.id > COALESCE(
            (SELECT last_read_id FROM read_cursors WHERE session_id = ? AND source = ?), 0
          )
        ORDER BY m.id
        LIMIT ?
        """,
        (channel, session_id, session_id, f"ch:{channel}", limit),
    )
    return [_row_to_message(r) for r in rows]


async def _poll_all_channels(
    db: aiosqlite.Connection, session_id: str, limit: int
) -> list[Message]:
    rows = await db.execute_fetchall(
        """
        SELECT m.*, s.name AS sender_name
        FROM messages m
        JOIN sessions s ON s.id = m.sender_id
        WHERE m.channel IS NOT NULL AND m.sender_id != ?
          AND m.id > COALESCE(
            (SELECT last_read_id FROM read_cursors
             WHERE session_id = ? AND source = 'ch:' || m.channel), 0
          )
        ORDER BY m.id
        LIMIT ?
        """,
        (session_id, session_id, limit),
    )
    return [_row_to_message(r) for r in rows]


async def _advance_cursors(
    db: aiosqlite.Connection, session_id: str, messages: list[Message]
) -> None:
    # Group messages by source
    dm_max: dict[str, int] = {}
    ch_max: dict[str, int] = {}
    for m in messages:
        if m.channel:
            key = f"ch:{m.channel}"
            ch_max[key] = max(ch_max.get(key, 0), m.id)
        else:
            dm_max[m.sender_id] = max(dm_max.get(m.sender_id, 0), m.id)

    for source, max_id in {**dm_max, **ch_max}.items():
        await db.execute(
            """
            INSERT INTO read_cursors (session_id, source, last_read_id)
            VALUES (?, ?, ?)
            ON CONFLICT (session_id, source)
            DO UPDATE SET last_read_id = MAX(read_cursors.last_read_id, excluded.last_read_id)
            """,
            (session_id, source, max_id),
        )


def _row_to_message(r: aiosqlite.Row) -> Message:
    return Message(
        id=r["id"],
        sender_id=r["sender_id"],
        sender_name=r["sender_name"],
        recipient_id=r["recipient_id"],
        body=r["body"],
        channel=r["channel"],
        thread_id=r["thread_id"],
        created_at=r["created_at"],
    )


async def get_history(
    name: str,
    with_session: str | None = None,
    channel: str | None = None,
    thread_id: int | None = None,
    limit: int = 50,
    before_id: int | None = None,
) -> list[Message]:
    limit = min(max(limit, 1), 200)
    db = await get_connection()
    try:
        session = await get_session_by_name(db, name)
        session_id = session["id"]

        conditions = []
        params: list = []

        if thread_id is not None:
            conditions.append("(m.id = ? OR m.thread_id = ?)")
            params.extend([thread_id, thread_id])
        elif with_session:
            other = await get_session_by_name(db, with_session)
            other_id = other["id"]
            conditions.append(
                "((m.sender_id = ? AND m.recipient_id = ?) OR (m.sender_id = ? AND m.recipient_id = ?))"
            )
            params.extend([session_id, other_id, other_id, session_id])
            conditions.append("m.channel IS NULL")
        elif channel:
            conditions.append("m.channel = ?")
            params.append(channel)
        else:
            # All messages involving this session
            conditions.append(
                "(m.recipient_id = ? OR m.sender_id = ? OR (m.channel IS NOT NULL AND m.sender_id != ?))"
            )
            params.extend([session_id, session_id, session_id])

        if before_id is not None:
            conditions.append("m.id < ?")
            params.append(before_id)

        where = " AND ".join(conditions) if conditions else "1=1"
        params.append(limit)

        rows = await db.execute_fetchall(
            f"""
            SELECT m.*, s.name AS sender_name
            FROM messages m
            JOIN sessions s ON s.id = m.sender_id
            WHERE {where}
            ORDER BY m.id DESC
            LIMIT ?
            """,
            params,
        )
        return [_row_to_message(r) for r in reversed(rows)]
    finally:
        await db.close()


# --- Channels ---


async def list_channels() -> list[Channel]:
    db = await get_connection()
    try:
        rows = await db.execute_fetchall("SELECT * FROM channels ORDER BY name")
        return [
            Channel(id=r["id"], name=r["name"], created_at=r["created_at"], description=r["description"])
            for r in rows
        ]
    finally:
        await db.close()


async def create_channel(name: str, description: str | None = None) -> Channel:
    validate_name(name)
    db = await get_connection()
    try:
        cursor = await db.execute(
            "INSERT INTO channels (name, description) VALUES (?, ?)",
            (name, description),
        )
        await db.commit()
        row = await db.execute_fetchall("SELECT * FROM channels WHERE id = ?", (cursor.lastrowid,))
        r = row[0]
        return Channel(id=r["id"], name=r["name"], created_at=r["created_at"], description=r["description"])
    finally:
        await db.close()


# --- Cleanup ---


async def _cleanup_stale(db: aiosqlite.Connection, ttl_minutes: int) -> list[str]:
    rows = await db.execute_fetchall(
        "SELECT id, name FROM sessions WHERE last_heartbeat < datetime('now', ?)",
        (f"-{ttl_minutes} minutes",),
    )
    removed = []
    for r in rows:
        await db.execute("DELETE FROM read_cursors WHERE session_id = ?", (r["id"],))
        # Clear message FK references before deleting session
        await db.execute("UPDATE messages SET recipient_id = NULL WHERE recipient_id = ?", (r["id"],))
        # Detach thread replies pointing at messages we're about to delete
        await db.execute(
            "UPDATE messages SET thread_id = NULL WHERE thread_id IN "
            "(SELECT id FROM messages WHERE sender_id = ?)", (r["id"],)
        )
        await db.execute("DELETE FROM messages WHERE sender_id = ?", (r["id"],))
        await db.execute("DELETE FROM sessions WHERE id = ?", (r["id"],))
        removed.append(r["name"])
    if removed:
        await db.commit()
    return removed


async def cleanup_sessions(ttl_minutes: int = CLEANUP_MINUTES) -> list[str]:
    db = await get_connection()
    try:
        return await _cleanup_stale(db, ttl_minutes)
    finally:
        await db.close()
