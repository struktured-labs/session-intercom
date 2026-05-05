"""Session lifecycle: register (idempotent), heartbeat, list, lookup."""

from __future__ import annotations

import uuid

import aiosqlite

from ..models import Session
from ._common import STALE_MINUTES, _now, get_connection, validate_name


async def register_session(
    name: str, metadata: str | None = None, team_name: str | None = None
) -> Session:
    validate_name(name)
    if team_name is not None:
        validate_name(team_name)
    db = await get_connection()
    try:
        row = await db.execute_fetchall(
            "SELECT * FROM sessions WHERE name = ?", (name,)
        )
        now = _now()
        if row:
            # Idempotent: reclaim existing session, refresh heartbeat & metadata
            existing = row[0]
            await db.execute(
                "UPDATE sessions SET last_heartbeat = ?, metadata = COALESCE(?, metadata), "
                "team_name = COALESCE(?, team_name) WHERE id = ?",
                (now, metadata, team_name, existing["id"]),
            )
            await db.commit()
            return Session(
                id=existing["id"], name=name, created_at=existing["created_at"],
                last_heartbeat=now,
                metadata=metadata if metadata is not None else existing["metadata"],
                team_name=team_name if team_name is not None else existing["team_name"],
            )

        session_id = str(uuid.uuid4())
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
