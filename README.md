# session-intercom

MCP server for P2P communication between independent Claude Code sessions. Messages arrive as `<channel>` tags between turns over the native MCP transport — zero polling, no file inbox, no `leadSessionId` binding to break.

## The problem

Claude Code has no built-in way for independent sessions to talk to each other. Agent Teams only work within a single parent session. If you have 5 sessions working on different parts of a project, they can't coordinate.

session-intercom solves this with an MCP server that registers named sessions, routes DMs and broadcast channels through shared SQLite state, and pushes inbound messages into each session as **Claude Code channel notifications** (`notifications/claude/channel`). The recipient sees them as `<channel source="session-intercom" from="alice" ...>body</channel>` tags injected between turns, like teammate messages.

## Quick start

### 1. Install

```bash
git clone git@github.com:struktured-labs/session-intercom.git
cd session-intercom
uv sync
```

### 2. Configure MCP

Add to `~/.claude.json` (or `~/.claude/mcp.json`):

```json
{
  "mcpServers": {
    "session-intercom": {
      "type": "stdio",
      "command": "uv",
      "args": ["run", "--directory", "/path/to/session-intercom", "session-intercom"]
    }
  }
}
```

Add permissions in `~/.claude/settings.json`:

```json
{
  "permissions": {
    "allow": ["mcp__session-intercom__*"]
  }
}
```

### 3. Launch Claude Code with channels enabled

session-intercom uses the [Channels API](https://code.claude.com/docs/en/channels-reference), currently in research preview. Custom channels aren't on Anthropic's allowlist, so launch with:

```bash
claude --dangerously-load-development-channels server:session-intercom
```

The first session in a project shows a one-time consent prompt. After that, channels are active for that session for as long as the flag is in the launch command.

### 4. Register the session

Just one call — no `TeamCreate`, no `team_name`:

```
intercom_register(name="my-session-name")
```

### 5. Send messages

Your own name is implicit after register:

```
intercom_send(to_name="recipient-name", body="Your message here")
```

The recipient's next turn includes the message as a `<channel source="session-intercom" from="my-session-name" message_id="42">Your message here</channel>` tag. No polling, no file inbox, no waiting on the CLI's old `InboxPoller`.

## How it works

```
Session A                                    Session B
    |                                            |
    | intercom_send(to_name="B", body="hi")      |
    |             |                              |
    |     [intercom MCP A]                       |
    |             |                              |
    |        [SQLite intercom.db]                |
    |             ^                              |
    |             | poll once / sec              |
    |             |                              |
    |     [intercom MCP B — tailer task]         |
    |             |                              |
    |             | notifications/claude/channel |
    |             v                              |
    |     [Claude Code B]                        |
    |             |                              |
    |             +------> <channel ...>hi</channel> arrives between turns
```

1. Sender's MCP inserts a row into the shared `intercom.db`
2. Recipient's MCP runs a background tailer that polls the DB for messages addressed to its registered session
3. For each new message, the tailer emits `notifications/claude/channel` directly on its stdio write stream
4. Claude Code injects the notification as a `<channel>` tag on the next turn — same path as teammate messages

## MCP tools

| Tool | Purpose |
|------|---------|
| `intercom_register(name)` | Register and set this name as the session's identity for all later calls |
| `intercom_send(to_name, body)` | Direct message — your own name is implicit |
| `intercom_broadcast(body, channel="general")` | Broadcast to a channel |
| `intercom_poll()` | Manual drain (rarely needed — channels deliver automatically) |
| `intercom_list_sessions()` | Discover registered sessions |
| `intercom_history(...)` | Read-only message history with pagination |
| `intercom_list_channels()` | List available broadcast channels |
| `intercom_create_channel(channel_name)` | Create a new broadcast channel |
| `intercom_cleanup(ttl_minutes=...)` | Remove sessions inactive for 2+ weeks (default) |

## Features

- **Channels-API delivery** — Claude Code receives messages as native MCP `<channel>` tags between turns, no polling
- **One-call setup** — just `intercom_register(name)`, no `TeamCreate` dance
- **Direct messages + broadcast channels** — pub/sub with named channels (default: `general`)
- **Threading** — reply to specific messages with `thread_id`
- **Message history** — paginated history with `before_id` cursor
- **Read cursors** — efficient tracking of unread messages per session
- **Idempotent registration** — re-register anytime without errors; crashed sessions just reconnect
- **Durable sessions** — two-week TTL, explicit `intercom_cleanup()` only
- **Concurrent-safe** — SQLite WAL mode

## Architecture

```
src/session_intercom/
  server.py   — low-level MCP server: tool handlers + channel tailer task
  db/         — SQLite layer (sessions / messages / channels / cleanup)
  models.py   — Session, Message, Channel dataclasses
```

- **Database**: `~/.local/share/session-intercom/intercom.db` (SQLite, WAL mode)
- **Transport**: MCP stdio. Inbound delivery via `notifications/claude/channel`
- **Capability declared**: `experimental: {"claude/channel": {}}` in the MCP `initialize` response — this is what makes Claude Code register a notification listener

## Running tests

```bash
uv run --extra dev pytest tests/ -v
```

## Migration from 0.5.x (file-inbox era)

Before 0.6, session-intercom wrote to `~/.claude/teams/<name>/inboxes/team-lead.json` and relied on the CLI's `InboxPoller` to pick it up. That path required `TeamCreate`, was vulnerable to stale `leadSessionId` bindings, and exposed `delivery_health` / `inbox_file_ready` / recovery recipes to work around the binding bug class.

All of that is gone in 0.6:

- No more `team_name` argument on `intercom_register`
- No more `inbox.py` / file inbox / `TeamCreate` setup
- No more `intercom_diagnose` (its purpose was diagnosing file-inbox brokenness)
- No more `delivery_health`, `inbox_file_ready`, `next_step`, `recovery`, `binding_mismatch` response fields
- No more session-restart-or-not debate

The Channels API gives us delivery semantics with the right invariant: the notification goes out on *this* session's MCP stdio, so it can't be misrouted by a stale binding.

## Requirements

- Python >= 3.11
- Claude Code v2.1.80 or later (channels support)
- Channels enabled per session: `claude --dangerously-load-development-channels server:session-intercom` until Anthropic allowlists this plugin
- uv (for running)
