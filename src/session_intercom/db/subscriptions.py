"""Channel subscriptions: who receives which broadcast channels.

DMs are always delivered — you can't unsubscribe from being messaged
directly. Channels are opt-in, defaulting to `general` on registration.
"""

from __future__ import annotations

import aiosqlite

from ._common import DEFAULT_CHANNEL, _now, get_connection, validate_name
from .sessions import get_session_by_name


async def _subscribed_channels(db: aiosqlite.Connection, session_id: str) -> list[str]:
    rows = await db.execute_fetchall(
        "SELECT channel FROM channel_subscriptions WHERE session_id = ? ORDER BY channel",
        (session_id,),
    )
    return [r["channel"] for r in rows]


async def list_subscriptions(name: str) -> list[str]:
    db = await get_connection()
    try:
        session = await get_session_by_name(db, name)
        return await _subscribed_channels(db, session["id"])
    finally:
        await db.close()


async def subscribe(name: str, channel: str) -> list[str]:
    """Subscribe to a channel. Returns the full subscription list after."""
    validate_name(channel)
    db = await get_connection()
    try:
        session = await get_session_by_name(db, name)
        rows = await db.execute_fetchall("SELECT name FROM channels WHERE name = ?", (channel,))
        if not rows:
            raise ValueError(
                f"Channel '{channel}' not found. Create it with intercom_create_channel."
            )
        await db.execute(
            "INSERT OR IGNORE INTO channel_subscriptions (session_id, channel, subscribed_at)"
            " VALUES (?, ?, ?)",
            (session["id"], channel, _now()),
        )
        await db.commit()
        return await _subscribed_channels(db, session["id"])
    finally:
        await db.close()


async def unsubscribe(name: str, channel: str) -> list[str]:
    """Unsubscribe from a channel. Returns the full subscription list after."""
    db = await get_connection()
    try:
        session = await get_session_by_name(db, name)
        await db.execute(
            "DELETE FROM channel_subscriptions WHERE session_id = ? AND channel = ?",
            (session["id"], channel),
        )
        await db.commit()
        return await _subscribed_channels(db, session["id"])
    finally:
        await db.close()


async def ensure_default_subscription(db: aiosqlite.Connection, session_id: str) -> None:
    """Subscribe a session to the default channel if it has no subscriptions.

    Called during registration. Only fires when the session has zero rows —
    so an agent that deliberately unsubscribed from everything stays that way
    across re-registration.
    """
    rows = await db.execute_fetchall(
        "SELECT 1 FROM channel_subscriptions WHERE session_id = ? LIMIT 1", (session_id,)
    )
    if not rows:
        await db.execute(
            "INSERT OR IGNORE INTO channel_subscriptions (session_id, channel) VALUES (?, ?)",
            (session_id, DEFAULT_CHANNEL),
        )
