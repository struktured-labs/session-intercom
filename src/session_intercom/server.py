from __future__ import annotations

import json
import logging
from contextlib import asynccontextmanager
from dataclasses import asdict

from mcp.server.fastmcp import FastMCP

from . import db

logger = logging.getLogger("session-intercom")

# Per-process state: each Claude session has its own MCP subprocess, so a
# module-level "current session" is exactly the right scope. After register,
# all other tools default to this name unless an explicit from_name overrides.
_current_session: str | None = None


@asynccontextmanager
async def lifespan(server: FastMCP):
    await db.init_db()
    logger.info("session-intercom: DB initialized at %s", db.DB_PATH)
    yield {}


mcp = FastMCP("session-intercom", lifespan=lifespan)


def _json(obj) -> str:
    if hasattr(obj, "__dataclass_fields__"):
        return json.dumps(asdict(obj), indent=2)
    return json.dumps(obj, indent=2, default=str)


def _resolve_name(provided: str | None) -> str:
    """Return the explicit name if given, otherwise the registered session name."""
    if provided:
        return provided
    if _current_session:
        return _current_session
    raise ValueError(
        "No session name available. Call intercom_register(name=...) first, "
        "or pass an explicit name argument."
    )


# --- Tools ---


@mcp.tool()
async def intercom_register(
    name: str, team_name: str | None = None, metadata: str | None = None
) -> str:
    """Register this session with the intercom system.

    After registering, all other intercom tools default to this name — you
    don't need to pass it on every call. Pass team_name (typically the same
    as name) to enable zero-polling native inbox delivery.

    Setup (one-time per session):
      1. Call TeamCreate(team_name=<name>)
      2. Call intercom_register(name=<name>, team_name=<name>)

    Args:
        name: Your session name (alphanumeric/hyphens, 1-64 chars). E.g. "rom-hacker".
        team_name: Claude Code team name for native delivery. Usually same as name.
        metadata: Optional free-form string with session info.
    """
    global _current_session
    try:
        session = await db.register_session(name, metadata, team_name)
        _current_session = name
        result: dict = {"status": "registered", "session": asdict(session)}
        if team_name:
            from .inbox import ensure_inbox
            result["inbox_file_ready"] = ensure_inbox(team_name)
            if not result["inbox_file_ready"]:
                result["next_step"] = (
                    f"Call TeamCreate(team_name='{team_name}') now to enable "
                    f"native zero-polling delivery, then re-call intercom_register. "
                    f"Registration is idempotent."
                )
            else:
                result["delivery_caveat"] = (
                    "Inbox file is set up, but the CLI's InboxPoller binds to "
                    "leadSessionId at conversation startup. If your conversation "
                    "started before TeamCreate ran, native delivery may silently "
                    "fail. Run intercom_diagnose() to verify."
                )
        else:
            result["tip"] = (
                "For zero-polling delivery: call TeamCreate(team_name=<name>), "
                "then re-register with team_name set. Or use the "
                "/session-intercom:intercom slash command."
            )
        return _json(result)
    except ValueError as e:
        return _json({"error": str(e)})


@mcp.tool()
async def intercom_send(
    to_name: str,
    body: str,
    thread_id: int | None = None,
    from_name: str | None = None,
) -> str:
    """Send a direct message to another session.

    Defaults `from_name` to the currently registered session — you don't
    need to pass it unless you want to send as a different identity.

    Args:
        to_name: Recipient session name.
        body: Message content (max 32KB).
        thread_id: Optional message ID to reply to (creates a thread).
        from_name: Sender override. Defaults to the registered session.
    """
    try:
        sender = _resolve_name(from_name)
        msg = await db.send_message(sender, to_name, body, thread_id)
        return _json({
            "status": "sent",
            "message_id": msg.id,
            "from": sender,
            "to": to_name,
            "created_at": msg.created_at,
        })
    except ValueError as e:
        return _json({"error": str(e)})


@mcp.tool()
async def intercom_broadcast(
    body: str,
    channel: str = "general",
    thread_id: int | None = None,
    from_name: str | None = None,
) -> str:
    """Broadcast a message to a channel (all sessions on the channel see it).

    Defaults `from_name` to the currently registered session.

    Args:
        body: Message content (max 32KB).
        channel: Channel name (default: "general").
        thread_id: Optional message ID to reply to (creates a thread).
        from_name: Sender override. Defaults to the registered session.
    """
    try:
        sender = _resolve_name(from_name)
        msg = await db.broadcast_message(sender, body, channel, thread_id)
        return _json({
            "status": "broadcast",
            "message_id": msg.id,
            "from": sender,
            "channel": channel,
            "created_at": msg.created_at,
        })
    except ValueError as e:
        return _json({"error": str(e)})


@mcp.tool()
async def intercom_poll(
    name: str | None = None,
    mark_read: bool = True,
    limit: int = 50,
    channel: str | None = None,
) -> str:
    """Poll for new/unread messages. Updates your heartbeat.

    With native inbox delivery active, you don't need to call this — messages
    arrive automatically between turns. Use it for explicit drains or as a
    workaround when intercom_diagnose reports broken delivery.

    Args:
        name: Session name override. Defaults to the registered session.
        mark_read: Advance read cursors so these messages won't appear again (default: true).
        limit: Max messages (1-200, default: 50).
        channel: Filter to a specific channel. Omit to get DMs + all channels.
    """
    try:
        session_name = _resolve_name(name)
        messages, remaining = await db.poll_messages(session_name, mark_read, limit, channel)
        return _json({
            "messages": [asdict(m) for m in messages],
            "count": len(messages),
            "remaining": remaining,
        })
    except ValueError as e:
        return _json({"error": str(e)})


@mcp.tool()
async def intercom_list_sessions(include_stale: bool = False) -> str:
    """List all registered sessions.

    Args:
        include_stale: Include sessions with no recent heartbeat (default: false).
    """
    sessions = await db.list_sessions(include_stale)
    return _json({
        "sessions": [asdict(s) for s in sessions],
        "count": len(sessions),
    })


@mcp.tool()
async def intercom_history(
    with_session: str | None = None,
    channel: str | None = None,
    thread_id: int | None = None,
    limit: int = 50,
    before_id: int | None = None,
    name: str | None = None,
) -> str:
    """Retrieve message history (read-only — does not advance cursors).

    Useful for inspecting messages without consuming them. Defaults to your
    registered session.

    Args:
        with_session: Filter to DM conversation with this session.
        channel: Filter to messages in this channel.
        thread_id: Get all messages in a specific thread.
        limit: Max messages (1-200, default: 50).
        before_id: Pagination cursor — get messages before this ID.
        name: Session name override. Defaults to the registered session.
    """
    try:
        session_name = _resolve_name(name)
        messages = await db.get_history(session_name, with_session, channel, thread_id, limit, before_id)
        return _json({
            "messages": [asdict(m) for m in messages],
            "count": len(messages),
        })
    except ValueError as e:
        return _json({"error": str(e)})


@mcp.tool()
async def intercom_list_channels() -> str:
    """List all available channels."""
    channels = await db.list_channels()
    return _json({
        "channels": [asdict(c) for c in channels],
        "count": len(channels),
    })


@mcp.tool()
async def intercom_create_channel(channel_name: str, description: str | None = None) -> str:
    """Create a new channel for broadcast messages.

    Args:
        channel_name: Channel name (alphanumeric/hyphens, 1-64 chars).
        description: Optional channel description.
    """
    try:
        channel = await db.create_channel(channel_name, description)
        return _json({"status": "created", "channel": asdict(channel)})
    except (ValueError, Exception) as e:
        return _json({"error": str(e)})


@mcp.tool()
async def intercom_diagnose(name: str | None = None) -> str:
    """Diagnose native-inbox delivery health.

    Checks team config, file inbox state, and MCP-side cursors to detect when
    the CLI's InboxPoller isn't actually delivering messages.

    Args:
        name: Session name override. Defaults to the registered session.
    """
    try:
        session_name = _resolve_name(name)
        diag = await db.diagnose_session(session_name)
    except ValueError as e:
        return _json({"error": str(e)})

    session = diag["session"]
    team_name = session.get("team_name")
    result: dict = {
        "session_name": session["name"],
        "team_name": team_name,
        "last_heartbeat": session["last_heartbeat"],
        "mcp_unread_dms": diag["mcp_unread_dms"],
        "mcp_unread_channel_msgs": diag["mcp_unread_channel_msgs"],
        "latest_addressed_message": diag["latest_addressed_message"],
    }

    if not team_name:
        result["verdict"] = "no_team"
        result["explanation"] = (
            "This session has no team_name set. Native inbox delivery is "
            "disabled. Re-register with a team_name (after TeamCreate), or "
            "rely on manual intercom_poll."
        )
        return _json(result)

    from .inbox import inbox_stats
    stats = inbox_stats(team_name)
    result["inbox_stats"] = stats

    if stats is None:
        result["verdict"] = "no_team_config"
        result["explanation"] = (
            f"~/.claude/teams/{team_name}/config.json does not exist. "
            f"Call TeamCreate(team_name='{team_name}') to create it."
        )
    elif stats["unread_messages"] > 0:
        result["verdict"] = "delivery_likely_broken"
        result["explanation"] = (
            f"File inbox has {stats['unread_messages']} unread message(s) that "
            f"haven't been delivered to your conversation. The CLI's "
            f"InboxPoller likely isn't bound to this session — common cause: "
            f"leadSessionId mismatch.\n\nFix without restarting Claude:\n"
            f"  1. TeamDelete()  — clears the in-process binding\n"
            f"  2. TeamCreate(team_name='{team_name}')\n"
            f"  3. intercom_register(name='{session['name']}', team_name='{team_name}')\n"
            f"\nUntil then, drain via intercom_poll() (no args needed)."
        )
    elif diag["latest_addressed_message"] is None:
        result["verdict"] = "no_messages_yet"
        result["explanation"] = (
            "No messages have been addressed to this session yet. Have someone "
            "send a test DM to verify delivery."
        )
    else:
        result["verdict"] = "ok"
        result["explanation"] = (
            "File inbox is empty (or fully read) and config exists. Native "
            "delivery is plausibly working. To confirm, have a sender send a "
            "fresh DM and re-run intercom_diagnose — if a new DM lands in the "
            "file inbox and stays unread, delivery is broken."
        )
    return _json(result)


@mcp.tool()
async def intercom_cleanup(ttl_minutes: int = db.CLEANUP_MINUTES) -> str:
    """Remove stale sessions (no heartbeat within TTL).

    Default TTL matches the durability promise (2 weeks). Pass a smaller value
    to force an aggressive sweep, but be aware this will delete other agents'
    sessions if they haven't checked in within that window.

    Args:
        ttl_minutes: Minutes of inactivity before a session is considered stale.
    """
    removed = await db.cleanup_sessions(ttl_minutes)
    return _json({
        "status": "cleaned",
        "removed": removed,
        "count": len(removed),
        "ttl_minutes": ttl_minutes,
    })


def main():
    mcp.run()


if __name__ == "__main__":
    main()
