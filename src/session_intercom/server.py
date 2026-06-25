"""session-intercom MCP server, channels-API edition.

Uses the low-level `mcp.server.Server` so we can declare the experimental
`claude/channel` capability and push `notifications/claude/channel` events
directly into the host Claude Code session. The previous file-inbox bridge
(write to `~/.claude/teams/<name>/inboxes/team-lead.json` and let the CLI's
InboxPoller pick it up) is gone — Claude Code receives DMs and channel
broadcasts as `<channel>` tags between turns over the MCP transport itself.

Architecture:
  - Each Claude Code session spawns its own stdio MCP subprocess
  - This module sets up tool handlers (register, send, broadcast, ...)
  - After register, a background tailer polls SQLite for messages addressed
    to the registered session and emits channel notifications on the same
    write stream Claude Code is listening on
"""

from __future__ import annotations

import json
import logging
from dataclasses import asdict
from typing import Any

import anyio
import mcp.types as types
from mcp.server.lowlevel import NotificationOptions, Server
from mcp.server.models import InitializationOptions
from mcp.server.stdio import stdio_server
from mcp.shared.message import SessionMessage
from mcp.types import JSONRPCMessage, JSONRPCNotification

from . import db

__version__ = "0.6.1"

logger = logging.getLogger("session-intercom")

# Per-process state: each Claude Code session has its own MCP subprocess.
_current_session: str | None = None
# Set once on register; the tailer waits on this before polling.
_tailer_ready = anyio.Event()


def _resolve_name(provided: str | None) -> str:
    if provided:
        return provided
    if _current_session:
        return _current_session
    raise ValueError(
        "No session name available. Call intercom_register(name=...) first, "
        "or pass an explicit name argument."
    )


def _ok(data: dict[str, Any]) -> list[types.TextContent]:
    return [types.TextContent(type="text", text=json.dumps(data, indent=2, default=str))]


def _err(msg: str) -> list[types.TextContent]:
    return [types.TextContent(type="text", text=json.dumps({"error": msg}))]


# --- Build the server ---

server: Server = Server("session-intercom")


@server.list_tools()
async def list_tools() -> list[types.Tool]:
    return [
        types.Tool(
            name="intercom_register",
            description=(
                "Register this session with the intercom network. After registering, "
                "all other intercom tools default to this name. Inbound messages arrive "
                'between turns as <channel source="session-intercom" ...> tags via MCP '
                "push — no polling, no file inbox."
            ),
            inputSchema={
                "type": "object",
                "properties": {
                    "name": {
                        "type": "string",
                        "description": "Your session name (alphanumeric/hyphens, 1-64 chars).",
                    },
                    "metadata": {"type": ["string", "null"]},
                },
                "required": ["name"],
            },
        ),
        types.Tool(
            name="intercom_send",
            description=(
                "Send a direct message. Recipient sees it as a <channel> tag on their "
                "next turn. from_name defaults to your registered name."
            ),
            inputSchema={
                "type": "object",
                "properties": {
                    "to_name": {"type": "string"},
                    "body": {"type": "string"},
                    "thread_id": {"type": ["integer", "null"]},
                    "from_name": {"type": ["string", "null"]},
                },
                "required": ["to_name", "body"],
            },
        ),
        types.Tool(
            name="intercom_broadcast",
            description="Broadcast to a channel (default: 'general'). from_name defaults to your registered session.",
            inputSchema={
                "type": "object",
                "properties": {
                    "body": {"type": "string"},
                    "channel": {"type": "string", "default": "general"},
                    "thread_id": {"type": ["integer", "null"]},
                    "from_name": {"type": ["string", "null"]},
                },
                "required": ["body"],
            },
        ),
        types.Tool(
            name="intercom_list_sessions",
            description="List all registered sessions.",
            inputSchema={
                "type": "object",
                "properties": {"include_stale": {"type": "boolean", "default": False}},
            },
        ),
        types.Tool(
            name="intercom_history",
            description="Retrieve message history (read-only — does not advance cursors).",
            inputSchema={
                "type": "object",
                "properties": {
                    "with_session": {"type": ["string", "null"]},
                    "channel": {"type": ["string", "null"]},
                    "thread_id": {"type": ["integer", "null"]},
                    "limit": {"type": "integer", "default": 50},
                    "before_id": {"type": ["integer", "null"]},
                    "name": {"type": ["string", "null"]},
                },
            },
        ),
        types.Tool(
            name="intercom_poll",
            description=(
                "Drain unread messages using the per-sender / per-channel cursors. "
                "Independent from channel-notification delivery: if the host wasn't "
                "launched with `--dangerously-load-development-channels server:session-intercom` "
                "(or channels otherwise silently fail), this is the recovery path — the "
                "tailer's cursor is separate and won't have advanced these cursors."
            ),
            inputSchema={
                "type": "object",
                "properties": {
                    "name": {"type": ["string", "null"]},
                    "mark_read": {"type": "boolean", "default": True},
                    "limit": {"type": "integer", "default": 50},
                    "channel": {"type": ["string", "null"]},
                },
            },
        ),
        types.Tool(
            name="intercom_list_channels",
            description="List available broadcast channels.",
            inputSchema={"type": "object", "properties": {}},
        ),
        types.Tool(
            name="intercom_create_channel",
            description="Create a new broadcast channel.",
            inputSchema={
                "type": "object",
                "properties": {
                    "channel_name": {"type": "string"},
                    "description": {"type": ["string", "null"]},
                },
                "required": ["channel_name"],
            },
        ),
        types.Tool(
            name="intercom_cleanup",
            description="Remove sessions inactive for ttl_minutes (default: 2 weeks).",
            inputSchema={
                "type": "object",
                "properties": {"ttl_minutes": {"type": "integer", "default": db.CLEANUP_MINUTES}},
            },
        ),
    ]


@server.call_tool()
async def call_tool(name: str, arguments: dict[str, Any]) -> list[types.TextContent]:
    global _current_session
    try:
        if name == "intercom_register":
            session = await db.register_session(arguments["name"], arguments.get("metadata"), None)
            _current_session = arguments["name"]
            if not _tailer_ready.is_set():
                _tailer_ready.set()
            return _ok({"status": "registered", "session": asdict(session)})

        if name == "intercom_send":
            sender = _resolve_name(arguments.get("from_name"))
            msg = await db.send_message(
                sender, arguments["to_name"], arguments["body"], arguments.get("thread_id")
            )
            return _ok(
                {
                    "status": "sent",
                    "message_id": msg.id,
                    "from": sender,
                    "to": arguments["to_name"],
                    "created_at": msg.created_at,
                }
            )

        if name == "intercom_broadcast":
            sender = _resolve_name(arguments.get("from_name"))
            channel = arguments.get("channel", "general")
            msg = await db.broadcast_message(
                sender, arguments["body"], channel, arguments.get("thread_id")
            )
            return _ok(
                {
                    "status": "broadcast",
                    "message_id": msg.id,
                    "from": sender,
                    "channel": channel,
                    "created_at": msg.created_at,
                }
            )

        if name == "intercom_list_sessions":
            sessions = await db.list_sessions(arguments.get("include_stale", False))
            return _ok({"sessions": [asdict(s) for s in sessions], "count": len(sessions)})

        if name == "intercom_history":
            session_name = _resolve_name(arguments.get("name"))
            messages = await db.get_history(
                session_name,
                arguments.get("with_session"),
                arguments.get("channel"),
                arguments.get("thread_id"),
                arguments.get("limit", 50),
                arguments.get("before_id"),
            )
            return _ok({"messages": [asdict(m) for m in messages], "count": len(messages)})

        if name == "intercom_poll":
            session_name = _resolve_name(arguments.get("name"))
            messages, remaining = await db.poll_messages(
                session_name,
                arguments.get("mark_read", True),
                arguments.get("limit", 50),
                arguments.get("channel"),
            )
            return _ok(
                {
                    "messages": [asdict(m) for m in messages],
                    "count": len(messages),
                    "remaining": remaining,
                }
            )

        if name == "intercom_list_channels":
            channels = await db.list_channels()
            return _ok({"channels": [asdict(c) for c in channels], "count": len(channels)})

        if name == "intercom_create_channel":
            channel = await db.create_channel(
                arguments["channel_name"], arguments.get("description")
            )
            return _ok({"status": "created", "channel": asdict(channel)})

        if name == "intercom_cleanup":
            ttl = arguments.get("ttl_minutes", db.CLEANUP_MINUTES)
            removed = await db.cleanup_sessions(ttl)
            return _ok(
                {"status": "cleaned", "removed": removed, "count": len(removed), "ttl_minutes": ttl}
            )

        return _err(f"Unknown tool: {name}")

    except ValueError as e:
        return _err(str(e))


# --- Tailer: push new messages as channel notifications ---


async def _emit_channel(write_stream: Any, content: str, meta: dict[str, str]) -> None:
    """Write a raw notifications/claude/channel notification to the MCP stream.

    Bypasses the typed SendNotificationT path because the SDK doesn't ship a
    typed model for this Claude-specific method. The wire format is standard
    JSON-RPC 2.0; we hand-construct JSONRPCNotification and push it on the
    same stream Claude Code is reading on.
    """
    notification = JSONRPCNotification(
        jsonrpc="2.0",
        method="notifications/claude/channel",
        params={"content": content, "meta": meta},
    )
    await write_stream.send(SessionMessage(message=JSONRPCMessage(notification)))


def _meta_safe(d: dict[str, Any]) -> dict[str, str]:
    """Channels API requires meta keys to be identifiers (letters/digits/_)
    and values to be strings. Drop anything else, stringify the rest.
    """
    out: dict[str, str] = {}
    for k, v in d.items():
        if v is None:
            continue
        if not isinstance(k, str) or not k.replace("_", "").isalnum():
            continue
        out[k] = str(v)
    return out


async def _tailer_loop(write_stream: Any) -> None:
    """Background task: fetch new messages addressed to _current_session via a
    tailer-specific cursor, emit each as a `<channel>` tag.

    Critically: this uses `db.fetch_for_channel_tailer`, which advances a
    cursor under source `tailer:channel` — independent from the per-sender
    cursors that `intercom_poll` advances. If channel delivery silently fails
    (no host listener), `intercom_poll` still works as a recovery path
    because its cursors are untouched.
    """
    await _tailer_ready.wait()
    logger.info("channel tailer started for session %s", _current_session)

    while True:
        try:
            if _current_session is None:
                await anyio.sleep(1.0)
                continue

            messages = await db.fetch_for_channel_tailer(_current_session, limit=20)
            for msg in messages:
                meta = _meta_safe(
                    {
                        "from": msg.sender_name,
                        "message_id": msg.id,
                        "channel": msg.channel,
                        "thread_id": msg.thread_id,
                    }
                )
                await _emit_channel(write_stream, msg.body, meta)
                logger.debug("emitted channel notification for msg %d", msg.id)
        except Exception:
            logger.exception("tailer iteration failed; continuing")

        await anyio.sleep(1.0)


# --- Main entry point ---


async def _run() -> None:
    await db.init_db()
    logger.info("session-intercom v%s — DB at %s", __version__, db.DB_PATH)

    async with stdio_server() as (read_stream, write_stream):
        init_options = InitializationOptions(
            server_name="session-intercom",
            server_version=__version__,
            capabilities=server.get_capabilities(
                notification_options=NotificationOptions(),
                experimental_capabilities={"claude/channel": {}},
            ),
        )
        async with anyio.create_task_group() as tg:
            tg.start_soon(_tailer_loop, write_stream)
            await server.run(read_stream, write_stream, init_options)
            tg.cancel_scope.cancel()


def main() -> None:
    anyio.run(_run)


if __name__ == "__main__":
    main()
