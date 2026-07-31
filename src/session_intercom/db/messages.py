"""Message routing: send, broadcast, poll, history. Refreshes sender heartbeat."""

from __future__ import annotations

import aiosqlite

from ..models import Message
from ._common import _now, get_connection, validate_body
from .sessions import get_session_by_name


async def send_message(
    from_name: str, to_name: str, body: str, thread_id: int | None = None
) -> Message:
    validate_body(body)
    db = await get_connection()
    try:
        sender = await get_session_by_name(db, from_name)
        recipient = await get_session_by_name(db, to_name)

        # Update sender heartbeat
        await db.execute(
            "UPDATE sessions SET last_heartbeat = ? WHERE id = ?", (_now(), sender["id"])
        )

        if thread_id is not None:
            rows = await db.execute_fetchall("SELECT id FROM messages WHERE id = ?", (thread_id,))
            if not rows:
                raise ValueError(f"Thread root message {thread_id} not found")

        cursor = await db.execute(
            "INSERT INTO messages (sender_id, recipient_id, body, thread_id) VALUES (?, ?, ?, ?)",
            (sender["id"], recipient["id"], body, thread_id),
        )
        msg_id = cursor.lastrowid
        assert msg_id is not None, "INSERT must produce a rowid"
        await db.commit()

        row = await db.execute_fetchall("SELECT created_at FROM messages WHERE id = ?", (msg_id,))
        created_at = row[0]["created_at"] if row else _now()

        return Message(
            id=msg_id,
            sender_id=sender["id"],
            sender_name=from_name,
            recipient_id=recipient["id"],
            recipient_name=to_name,
            body=body,
            thread_id=thread_id,
            created_at=created_at,
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

        # Update sender heartbeat
        await db.execute(
            "UPDATE sessions SET last_heartbeat = ? WHERE id = ?", (_now(), sender["id"])
        )

        # Verify channel exists
        rows = await db.execute_fetchall("SELECT name FROM channels WHERE name = ?", (channel,))
        if not rows:
            raise ValueError(
                f"Channel '{channel}' not found. Create it with intercom_create_channel."
            )

        if thread_id is not None:
            rows = await db.execute_fetchall("SELECT id FROM messages WHERE id = ?", (thread_id,))
            if not rows:
                raise ValueError(f"Thread root message {thread_id} not found")

        cursor = await db.execute(
            "INSERT INTO messages (sender_id, channel, body, thread_id) VALUES (?, ?, ?, ?)",
            (sender["id"], channel, body, thread_id),
        )
        msg_id = cursor.lastrowid
        assert msg_id is not None, "INSERT must produce a rowid"
        await db.commit()

        row = await db.execute_fetchall("SELECT created_at FROM messages WHERE id = ?", (msg_id,))
        created_at = row[0]["created_at"] if row else _now()

        return Message(
            id=msg_id,
            sender_id=sender["id"],
            sender_name=from_name,
            channel=channel,
            body=body,
            thread_id=thread_id,
            created_at=created_at,
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

        messages: list[Message] = []

        if channel:
            messages.extend(await _poll_channel(db, session_id, channel, limit))
        else:
            messages.extend(await _poll_dms(db, session_id, limit))
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


_TAILER_CURSOR_SOURCE = "tailer:channel"

# What the tailer is allowed to push: DMs addressed to this session (always),
# plus broadcasts on channels this session subscribed to. Own messages never
# echo back. A channel message only counts once the subscription predates it,
# so subscribing mid-session doesn't replay that channel's whole history.
# The register-time default subscription is backdated (see sessions.py) so the
# backlog cap governs it instead. Binds session_id three times.
_TAILER_ELIGIBLE_SQL = """(
    m.recipient_id = ?
    OR (
        m.channel IS NOT NULL
        AND m.sender_id != ?
        AND EXISTS (
            SELECT 1 FROM channel_subscriptions cs
            WHERE cs.session_id = ? AND cs.channel = m.channel
              AND m.created_at >= cs.subscribed_at
        )
    )
)"""


async def fetch_for_channel_tailer(name: str, limit: int = 20) -> list[Message]:
    """Read new messages for the tailer to emit as channel notifications.

    Uses a tailer-specific cursor (`tailer:channel`) that is independent
    from the per-sender / per-channel cursors `poll_messages` advances. This
    means: tailer emission and explicit poll consumption are decoupled. If
    a notification silently fails to land (no host listener), the message is
    still consumable via `intercom_poll` because its per-sender cursor was
    never touched.

    Advances the tailer cursor atomically with the read so a message isn't
    re-emitted on the next tick.
    """
    limit = min(max(limit, 1), 200)
    db = await get_connection()
    try:
        session = await get_session_by_name(db, name)
        session_id = session["id"]

        rows = await db.execute_fetchall(
            f"""
            SELECT m.*, s.name AS sender_name
            FROM messages m
            JOIN sessions s ON s.id = m.sender_id
            WHERE {_TAILER_ELIGIBLE_SQL}
              AND m.id > COALESCE(
                (SELECT last_read_id FROM read_cursors
                 WHERE session_id = ? AND source = ?), 0
              )
            ORDER BY m.id
            LIMIT ?
            """,
            (session_id, session_id, session_id, session_id, _TAILER_CURSOR_SOURCE, limit),
        )  # 3 for _TAILER_ELIGIBLE_SQL, then cursor lookup (session_id, source), then limit
        messages = [_row_to_message(r) for r in rows]

        if messages:
            max_id = max(m.id for m in messages)
            await db.execute(
                """
                INSERT INTO read_cursors (session_id, source, last_read_id)
                VALUES (?, ?, ?)
                ON CONFLICT (session_id, source)
                DO UPDATE SET last_read_id = MAX(read_cursors.last_read_id, excluded.last_read_id)
                """,
                (session_id, _TAILER_CURSOR_SOURCE, max_id),
            )
            await db.commit()
        return messages
    finally:
        await db.close()


async def init_tailer_cursor(name: str, backlog: int) -> dict:
    """Cap how much history the tailer will replay on (re)registration.

    A fresh session's tailer cursor is 0, so without this it emits every
    message ever addressed to it — for a busy `general` channel that is
    hundreds of messages and enough context to force a compaction on startup.

    Jumps the cursor forward so at most `backlog` messages remain pending.
    Returns {"pending": n, "skipped": m} so the caller can tell the agent
    what was dropped (still reachable via intercom_history).
    """
    backlog = max(backlog, 0)
    db = await get_connection()
    try:
        session = await get_session_by_name(db, name)
        session_id = session["id"]
        args = (session_id, session_id, session_id, session_id, _TAILER_CURSOR_SOURCE)

        count_rows = await db.execute_fetchall(
            f"""
            SELECT COUNT(*) AS n
            FROM messages m
            WHERE {_TAILER_ELIGIBLE_SQL}
              AND m.id > COALESCE(
                (SELECT last_read_id FROM read_cursors
                 WHERE session_id = ? AND source = ?), 0
              )
            """,
            args,
        )
        eligible = count_rows[0]["n"] if count_rows else 0
        if eligible <= backlog:
            return {"pending": eligible, "skipped": 0}

        # The (backlog+1)-th newest message is the newest one we want to SKIP.
        # Parking the cursor there leaves exactly `backlog` pending.
        boundary_rows = await db.execute_fetchall(
            f"""
            SELECT m.id
            FROM messages m
            WHERE {_TAILER_ELIGIBLE_SQL}
              AND m.id > COALESCE(
                (SELECT last_read_id FROM read_cursors
                 WHERE session_id = ? AND source = ?), 0
              )
            ORDER BY m.id DESC
            LIMIT 1 OFFSET ?
            """,
            (*args, backlog),
        )
        if not boundary_rows:
            return {"pending": eligible, "skipped": 0}

        await db.execute(
            """
            INSERT INTO read_cursors (session_id, source, last_read_id)
            VALUES (?, ?, ?)
            ON CONFLICT (session_id, source)
            DO UPDATE SET last_read_id = MAX(read_cursors.last_read_id, excluded.last_read_id)
            """,
            (session_id, _TAILER_CURSOR_SOURCE, boundary_rows[0]["id"]),
        )
        await db.commit()
        return {"pending": backlog, "skipped": eligible - backlog}
    finally:
        await db.close()


async def _poll_dms(db: aiosqlite.Connection, session_id: str, limit: int) -> list[Message]:
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
        return [_row_to_message(r) for r in reversed(list(rows))]
    finally:
        await db.close()
