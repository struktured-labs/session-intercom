"""Broadcast channels: list and create."""

from __future__ import annotations

from ..models import Channel
from ._common import get_connection, validate_name


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
