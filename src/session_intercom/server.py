from __future__ import annotations

import json
import logging
from contextlib import asynccontextmanager
from dataclasses import asdict

from mcp.server.fastmcp import FastMCP

from . import db

logger = logging.getLogger("session-intercom")


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


# --- Tools ---


@mcp.tool()
async def intercom_register(
    name: str, metadata: str | None = None, team_name: str | None = None
) -> str:
    """Register this session with the intercom system.

    For zero-polling message delivery, first create a personal team with TeamCreate
    using your session name, then pass that name as team_name here. Messages will be
    written directly to your native Claude Code inbox — no /loop needed.

    Setup (one-time per session):
      1. Call TeamCreate with team_name=<your-name>
      2. Call intercom_register(name=<your-name>, team_name=<your-name>)

    Args:
        name: Human-readable session name (alphanumeric/hyphens, 1-64 chars). E.g. "rom-hacker", "fleet-ops".
        metadata: Optional JSON string with arbitrary session info.
        team_name: Optional Claude Code team name for native inbox delivery. Must match a team you created with TeamCreate.
    """
    try:
        session = await db.register_session(name, metadata, team_name)
        result = {"status": "registered", "session": asdict(session)}
        if team_name:
            from .inbox import ensure_inbox
            result["inbox_file_ready"] = ensure_inbox(team_name)
            if not result["inbox_file_ready"]:
                result["next_step"] = (
                    f"Call TeamCreate(team_name='{team_name}') now to enable native "
                    f"zero-polling inbox delivery, then re-call intercom_register with "
                    f"the same name. Registration is idempotent — safe to call again."
                )
            else:
                result["delivery_caveat"] = (
                    "Inbox file is ready, but the CLI's InboxPoller binds to "
                    "leadSessionId at conversation startup. If your conversation "
                    "started before TeamCreate ran (or before this team was registered "
                    "with this session), native delivery may NOT actually fire. Run "
                    "intercom_diagnose to verify, especially for long-lived sessions "
                    "across compactions."
                )
        else:
            result["tip"] = (
                "For zero-polling delivery: call TeamCreate(team_name=<name>), then "
                "re-call intercom_register(name=<name>, team_name=<name>). Or use the "
                "/session-intercom:intercom slash command if you have the plugin installed."
            )
        return _json(result)
    except ValueError as e:
        return _json({"error": str(e)})


@mcp.tool()
async def intercom_send(from_name: str, to_name: str, body: str, thread_id: int | None = None) -> str:
    """Send a direct message to another session.

    Args:
        from_name: Your registered session name.
        to_name: Recipient session name.
        body: Message content (max 32KB).
        thread_id: Optional message ID to reply to (creates a thread).
    """
    try:
        msg = await db.send_message(from_name, to_name, body, thread_id)
        return _json({"status": "sent", "message_id": msg.id, "to": to_name, "created_at": msg.created_at})
    except ValueError as e:
        return _json({"error": str(e)})


@mcp.tool()
async def intercom_broadcast(
    from_name: str, body: str, channel: str = "general", thread_id: int | None = None
) -> str:
    """Broadcast a message to a channel (all sessions can see it).

    Args:
        from_name: Your registered session name.
        body: Message content (max 32KB).
        channel: Channel name (default: "general").
        thread_id: Optional message ID to reply to (creates a thread).
    """
    try:
        msg = await db.broadcast_message(from_name, body, channel, thread_id)
        return _json({
            "status": "broadcast", "message_id": msg.id,
            "channel": channel, "created_at": msg.created_at,
        })
    except ValueError as e:
        return _json({"error": str(e)})


@mcp.tool()
async def intercom_poll(
    name: str, mark_read: bool = True, limit: int = 50, channel: str | None = None
) -> str:
    """Poll for new/unread messages. Also updates your heartbeat.

    If you registered with a team_name, messages are delivered to your native inbox
    automatically — you only need this for checking history or if not using native delivery.

    Args:
        name: Your registered session name.
        mark_read: Advance read cursors so these messages won't appear again (default: true).
        limit: Max messages to return (1-200, default: 50).
        channel: Filter to a specific channel. Omit to get DMs + all channels.
    """
    try:
        messages, remaining = await db.poll_messages(name, mark_read, limit, channel)
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
async def intercom_heartbeat(name: str) -> str:
    """Update your session heartbeat (intercom_poll does this automatically).

    Args:
        name: Your registered session name.
    """
    try:
        session = await db.heartbeat(name)
        return _json({"status": "ok", "name": session.name, "last_heartbeat": session.last_heartbeat})
    except ValueError as e:
        return _json({"error": str(e)})


@mcp.tool()
async def intercom_history(
    name: str,
    with_session: str | None = None,
    channel: str | None = None,
    thread_id: int | None = None,
    limit: int = 50,
    before_id: int | None = None,
) -> str:
    """Retrieve message history (including already-read messages).

    Args:
        name: Your registered session name.
        with_session: Filter to DM conversation with this session.
        channel: Filter to messages in this channel.
        thread_id: Get all messages in a specific thread.
        limit: Max messages (1-200, default: 50).
        before_id: Pagination cursor — get messages before this ID.
    """
    try:
        messages = await db.get_history(name, with_session, channel, thread_id, limit, before_id)
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
async def intercom_create_channel(name: str, description: str | None = None) -> str:
    """Create a new channel for broadcast messages.

    Args:
        name: Channel name (alphanumeric/hyphens, 1-64 chars).
        description: Optional channel description.
    """
    try:
        channel = await db.create_channel(name, description)
        return _json({"status": "created", "channel": asdict(channel)})
    except (ValueError, Exception) as e:
        return _json({"error": str(e)})


@mcp.tool()
async def intercom_diagnose(name: str) -> str:
    """Diagnose native-inbox delivery health for this session.

    Checks the team config, file inbox state, MCP-side cursors, and infers
    whether the CLI's InboxPoller is actually delivering messages between turns.

    Use this when:
    - Messages from other sessions don't seem to be arriving automatically
    - You suspect the leadSessionId binding is stale (long-lived conversation)
    - Before assuming intercom_register's inbox_file_ready=true means delivery works

    Returns a structured diagnosis with verdict + suggested next steps.

    Args:
        name: Your registered session name.
    """
    try:
        diag = await db.diagnose_session(name)
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
            "This session has no team_name set. Native inbox delivery is disabled. "
            "Either re-register with a team_name (after calling TeamCreate), or rely "
            "on manual intercom_poll."
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
            f"File inbox has {stats['unread_messages']} unread message(s) but they "
            f"haven't been delivered to your conversation. The CLI's InboxPoller "
            f"likely isn't bound to this session — common cause: leadSessionId in "
            f"team config didn't match this conversation's session ID at startup. "
            f"Workarounds: (a) call intercom_poll('{session['name']}') to drain "
            f"messages via MCP cursor, (b) restart this session for a clean "
            f"binding (loses context), or (c) keep using manual polling. The MCP "
            f"cannot fix the in-process binding."
        )
    elif diag["latest_addressed_message"] is None:
        result["verdict"] = "no_messages_yet"
        result["explanation"] = (
            "No messages have been addressed to this session yet. Have someone "
            "send a test DM with intercom_send to verify delivery."
        )
    else:
        result["verdict"] = "ok"
        result["explanation"] = (
            "File inbox is empty (or fully read) and config exists. Native "
            "delivery is plausibly working. Have a sender try a fresh DM and "
            "re-run intercom_diagnose if you want to confirm — if a new DM "
            "arrives in the file inbox and stays unread, delivery is broken."
        )
    return _json(result)


@mcp.tool()
async def intercom_cleanup(ttl_minutes: int = 30) -> str:
    """Remove stale sessions (no heartbeat within TTL).

    Args:
        ttl_minutes: Minutes of inactivity before a session is considered stale (default: 30).
    """
    removed = await db.cleanup_sessions(ttl_minutes)
    return _json({
        "status": "cleaned",
        "removed": removed,
        "count": len(removed),
    })


def main():
    mcp.run()


if __name__ == "__main__":
    main()
