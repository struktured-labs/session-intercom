# session-intercom

MCP server for P2P communication between independent Claude Code sessions. Messages are delivered directly to Claude's native inbox with zero polling — no `/loop`, no token bleed.

## The problem

Claude Code has no built-in way for independent sessions to talk to each other. Agent Teams only work within a single parent session. If you have 5 sessions working on different parts of a project, they can't coordinate.

session-intercom solves this with an MCP server that provides named session registration, direct messaging, broadcast channels, threading, and message history — all backed by SQLite.

**With the native inbox bridge**, messages land in Claude's built-in `InboxPoller` and get delivered between turns automatically, just like teammate messages. No polling tool calls eating your context.

## Quick start

### 1. Install

```bash
# Clone
git clone git@github.com:struktured-labs/session-intercom.git
cd session-intercom

# Install with uv
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
      "args": [
        "run",
        "--directory",
        "/path/to/session-intercom",
        "session-intercom"
      ]
    }
  }
}
```

Add permissions in `~/.claude/settings.json`:

```json
{
  "permissions": {
    "allow": [
      "mcp__session-intercom__*"
    ]
  }
}
```

### 3. Session startup (two lines)

Every Claude Code session that wants to receive messages runs these at startup:

```
TeamCreate("my-session-name")
intercom_register("my-session-name", team_name="my-session-name")
```

That's it. The session now has:
- A personal team (activates the CLI's built-in InboxPoller)
- An intercom registration with native inbox delivery

### 4. Send messages

From any registered session:

```
intercom_send("sender-name", "recipient-name", "Your message here")
```

The recipient gets the message delivered automatically between turns. No `/loop`, no `intercom_poll` needed.

## How it works

```
Session A                          Session B
    |                                  |
    | intercom_send("A", "B", "hi")    |
    |           |                      |
    |     [intercom MCP]               |
    |       |         |                |
    |   [SQLite]   [inbox file]        |
    |              ~/.claude/teams/    |
    |              B/inboxes/          |
    |              team-lead.json      |
    |                  |               |
    |            [InboxPoller ~1s]     |
    |                  |               |
    |                  +---> delivered  |
```

1. Sender calls `intercom_send` via MCP
2. Intercom writes to SQLite (history, threading, cursors) AND the recipient's native inbox file
3. Claude CLI's built-in `InboxPoller` picks up the message within ~1 second
4. Message delivered as a teammate notification — appears between turns like a user message

## MCP tools

| Tool | Purpose |
|------|---------|
| `intercom_register` | Register session + enable native inbox delivery |
| `intercom_send` | Direct message to another session |
| `intercom_broadcast` | Broadcast to a channel (all sessions) |
| `intercom_poll` | Manual poll (only needed without native inbox) |
| `intercom_list_sessions` | Discover registered sessions |
| `intercom_heartbeat` | Keep-alive ping (send/poll do this automatically) |
| `intercom_history` | Retrieve message history with pagination |
| `intercom_list_channels` | List available channels |
| `intercom_create_channel` | Create a new broadcast channel |
| `intercom_cleanup` | Remove stale sessions |

## Features

- **Native inbox delivery** — zero-polling message receipt via Claude's built-in InboxPoller
- **Direct messages** — P2P between named sessions
- **Broadcast channels** — pub/sub with named channels (default: `general`)
- **Threading** — reply to specific messages with `thread_id`
- **Message history** — paginated history with `before_id` cursor
- **Read cursors** — efficient tracking of unread messages per session
- **Idempotent registration** — re-register anytime without errors; crashed sessions just reconnect
- **Session discovery** — list active sessions with heartbeat status
- **Explicit cleanup only** — sessions persist for weeks; cleanup runs only when you ask for it
- **Concurrent-safe** — SQLite WAL mode + flock on inbox files

## Architecture

```
src/session_intercom/
  server.py   — FastMCP server, 10 tool definitions
  db.py       — SQLite schema, queries, inbox bridge calls
  models.py   — Session, Message, Channel dataclasses
  inbox.py    — Native Claude Code inbox file writer
```

- **Database**: `~/.local/share/session-intercom/intercom.db` (SQLite, WAL mode)
- **Inbox files**: `~/.claude/teams/{team}/inboxes/team-lead.json`
- **Transport**: MCP stdio (each Claude session runs its own server process, all share the DB)

## Running tests

```bash
uv run --extra dev pytest tests/ -v
```

## Without native inbox (legacy mode)

Sessions that don't set `team_name` in `intercom_register` still work — they just need to poll manually:

```
intercom_register("my-session")
# Then periodically:
intercom_poll("my-session")
```

This was the original mode before the inbox bridge. It works but burns context tokens on polling.

## Requirements

- Python >= 3.11
- Claude Code with `CLAUDE_CODE_EXPERIMENTAL_AGENT_TEAMS=1`
- uv (for running)
