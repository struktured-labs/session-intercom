"""Stale-session cleanup. Only runs on explicit invocation (intercom_cleanup)."""

from __future__ import annotations

import aiosqlite

from ._common import CLEANUP_MINUTES, get_connection


async def _cleanup_stale(db: aiosqlite.Connection, ttl_minutes: int) -> list[str]:
    rows = await db.execute_fetchall(
        "SELECT id, name FROM sessions WHERE last_heartbeat < datetime('now', ?)",
        (f"-{ttl_minutes} minutes",),
    )
    removed = []
    for r in rows:
        await db.execute("DELETE FROM read_cursors WHERE session_id = ?", (r["id"],))
        # Clear message FK references before deleting session
        await db.execute(
            "UPDATE messages SET recipient_id = NULL WHERE recipient_id = ?", (r["id"],)
        )
        # Detach thread replies pointing at messages we're about to delete
        await db.execute(
            "UPDATE messages SET thread_id = NULL WHERE thread_id IN "
            "(SELECT id FROM messages WHERE sender_id = ?)",
            (r["id"],),
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
