"""MCP-side delivery diagnostics: unread counts, latest message, session row."""

from __future__ import annotations

from ._common import get_connection
from .sessions import get_session_by_name


async def diagnose_session(name: str) -> dict:
    """Inspect this session's delivery pipeline state from the MCP side.

    Returns a structured dict with: session row, unread DM/channel counts at
    the MCP cursor layer, and the latest message addressed to this session.
    The file-inbox side is checked separately by `inbox.inbox_stats` in the
    server layer — both halves are needed for a full delivery_health verdict.
    """
    db = await get_connection()
    try:
        session = await get_session_by_name(db, name)
        session_id = session["id"]

        dm_row = await db.execute_fetchall(
            """
            SELECT COUNT(*) AS n
            FROM messages m
            WHERE m.recipient_id = ? AND m.channel IS NULL
              AND m.id > COALESCE(
                (SELECT last_read_id FROM read_cursors
                 WHERE session_id = ? AND source = m.sender_id), 0
              )
            """,
            (session_id, session_id),
        )
        dm_unread = dm_row[0]["n"] if dm_row else 0

        ch_row = await db.execute_fetchall(
            """
            SELECT COUNT(*) AS n
            FROM messages m
            WHERE m.channel IS NOT NULL AND m.sender_id != ?
              AND m.id > COALESCE(
                (SELECT last_read_id FROM read_cursors
                 WHERE session_id = ? AND source = 'ch:' || m.channel), 0
              )
            """,
            (session_id, session_id),
        )
        ch_unread = ch_row[0]["n"] if ch_row else 0

        latest = await db.execute_fetchall(
            """
            SELECT m.id, m.created_at, s.name AS sender_name
            FROM messages m
            JOIN sessions s ON s.id = m.sender_id
            WHERE m.recipient_id = ? OR (m.channel IS NOT NULL AND m.sender_id != ?)
            ORDER BY m.id DESC LIMIT 1
            """,
            (session_id, session_id),
        )
        latest_msg = dict(latest[0]) if latest else None

        return {
            "session": dict(session),
            "mcp_unread_dms": dm_unread,
            "mcp_unread_channel_msgs": ch_unread,
            "latest_addressed_message": latest_msg,
        }
    finally:
        await db.close()
