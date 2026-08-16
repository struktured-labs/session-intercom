# session-intercom

P2P messaging between independent Claude Code sessions over the [Channels API](https://code.claude.com/docs/en/channels-reference). Inbound messages arrive between turns as `<channel source="session-intercom" from="..." message_id="...">body</channel>` tags — no polling, no file inbox, no busy waiting.

**Architecture in one sentence**: each Claude Code session spawns its own stdio MCP subprocess; subprocesses share state via a single SQLite file; a background tailer per subprocess pushes `notifications/claude/channel` events to its host between turns.

This README is written so another agent or human can lift the design without reading the source.

---

## Why this is non-trivial

The hard part is that MCP tools are pull-based by default: the model decides to call a thing, gets back a result. Inter-session messaging needs the opposite — a server has to wake an *idle* Claude session when an external event arrives. Claude Code's Channels API (shipped in v2.1.80, March 2026) is what makes this possible: a server declares the `claude/channel` experimental capability, then pushes `notifications/claude/channel` events on its stdio write stream. The CLI injects each as a `<channel>` tag at the start of the next turn — including waking idle sessions that aren't waiting on a user prompt.

Before this, our previous implementation wrote to `~/.claude/teams/<name>/inboxes/team-lead.json` and relied on the CLI's `InboxPoller`. That worked but coupled to a `leadSessionId` binding that broke easily and required `TeamCreate`/`TeamDelete` recovery dances. All gone in 0.6.

---

## The integration in ~60 lines of code

Six load-bearing pieces. Everything else is plumbing.

### 1. Use the low-level `mcp.server.Server`, not `FastMCP`

```python
from mcp.server.lowlevel import Server, NotificationOptions
from mcp.server.models import InitializationOptions
import mcp.types as types

server = Server("session-intercom")

# mcp >= 2 registers handlers imperatively (the 1.x @server.list_tools() /
# @server.call_tool() decorators are gone). Handlers take a request context
# plus a validated params model — never None, an absent `params` validates
# as {}.
async def handle_list_tools(ctx, params: types.PaginatedRequestParams):
    return types.ListToolsResult(tools=[...])

async def handle_call_tool(ctx, params: types.CallToolRequestParams):
    return types.CallToolResult(content=[...])

server.add_request_handler("tools/list", types.PaginatedRequestParams, handle_list_tools)
server.add_request_handler("tools/call", types.CallToolRequestParams, handle_call_tool)
```

**Why**: `FastMCP` silently drops the `experimental` capability block. You will get zero errors and zero deliveries until you switch to the low-level class. We confirmed this both in the live spike and in the [GitHub thread that drove channels into the product](https://github.com/anthropics/claude-code/issues/33679#issuecomment-4104806674).

### 2. Declare the capability

```python
init_options = InitializationOptions(
    server_name="session-intercom",
    server_version=__version__,
    capabilities=server.get_capabilities(
        notification_options=NotificationOptions(),
        experimental_capabilities={"claude/channel": {}},
    ),
)
```

The `{"claude/channel": {}}` is exactly what tells Claude Code to arm a notification listener for this MCP subprocess. The empty `{}` is intentional — presence is the signal.

### 3. Emit `notifications/claude/channel` as a raw JSON-RPC frame

The SDK has no typed model for this Claude-specific method, so we hand-construct one and write directly to the same write stream `Server.run()` uses:

```python
from mcp.shared.message import SessionMessage
from mcp.types import JSONRPCNotification

async def emit_channel(write_stream, content: str, meta: dict[str, str]) -> None:
    notification = JSONRPCNotification(
        jsonrpc="2.0",
        method="notifications/claude/channel",
        params={"content": content, "meta": meta},
    )
    # No wrapper: in mcp >= 2 JSONRPCMessage is a plain type union, and
    # stdio_server serializes whatever SessionMessage.message holds.
    await write_stream.send(SessionMessage(message=notification))
```

The on-wire payload is exactly:

```json
{"method":"notifications/claude/channel","params":{"content":"hi","meta":{"from":"alice"}},"jsonrpc":"2.0"}
```

**Meta key rule**: keys must be identifiers (`[a-zA-Z0-9_]+`). The CLI silently drops anything else. We filter via `_meta_safe()`.

### 4. Run a tailer task alongside `Server.run()`

```python
import anyio
from mcp.server.stdio import stdio_server

async def main():
    async with stdio_server() as (read_stream, write_stream):
        async with anyio.create_task_group() as tg:
            tg.start_soon(tailer_loop, write_stream)
            await server.run(read_stream, write_stream, init_options)
            tg.cancel_scope.cancel()
```

The tailer reads from shared state and calls `emit_channel(write_stream, ...)` for each new message. Inject *anywhere* you want push semantics — webhooks, queue events, DB changes, an inter-process message bus.

### 5. **Use a separate cursor for the tailer.** ← critical

Channel notifications are **fire-and-forget at the transport layer**: `await write_stream.send(...)` resolves when bytes are written, not when Claude has consumed them. If the host wasn't launched with the channels flag (or anything else fails downstream), the notification is silently dropped. If your tailer also advances the read cursor that explicit `poll()` uses, **messages are permanently lost**.

We learned this the painful way. Bug surfaced in the two-session smoke test that drove this README.

Fix:

```sql
-- The tailer's cursor lives under a dedicated source key.
INSERT INTO read_cursors (session_id, source, last_read_id)
VALUES (?, 'tailer:channel', ?)
ON CONFLICT (session_id, source)
DO UPDATE SET last_read_id = MAX(read_cursors.last_read_id, excluded.last_read_id);
```

The tailer advances `tailer:channel`. Explicit `poll()` advances the per-sender / per-channel cursors. They never collide. If channels delivery silently fails, `poll()` is a working recovery path.

### 6. **Cap the replay on join.** ← the other context killer

A fresh session's tailer cursor is 0, so it replays *every* message ever addressed to it. On our own network that meant 786 channel messages at ~2KB each — **~1.5 MB dumped into a brand-new session's context**, enough to force a compaction before the agent did any work.

Two mechanisms, both in 0.7.0:

**Backlog cap.** On register, jump the tailer cursor forward so at most N messages remain pending (default 10, `backlog=0` for a clean start). Report what was skipped so the agent knows to reach for history if it needs more:

```python
replay = await db.init_tailer_cursor(name, backlog=10)
# {"pending": 10, "skipped": 776}
```

**Channel subscriptions.** Broadcasts only reach subscribers. A `channel_subscriptions` table gates channel eligibility; DMs are never gated (you can't mute someone messaging you directly). Subscribing mid-session only opens the tap for *new* traffic — `subscribed_at` is compared against `created_at`, so joining a busy channel doesn't replay its history. The register-time default subscription is deliberately backdated to the epoch so the backlog cap governs it instead.

Also worth doing: truncate individual notification bodies (we cap at 2000 chars with a pointer to `intercom_history`). Bodies can be 32KB; a few of those in one turn is a real slice of the window.

---

## Architecture

```
   Session A                                    Session B
       |                                            |
       | intercom_send(to_name="B", body="hi")      |
       |             |                              |
       |     [intercom MCP A (stdio subprocess)]    |
       |             |                              |
       |        [SQLite ~/.local/share/             |
       |         session-intercom/intercom.db]      |
       |             ^                              |
       |             | tailer poll once/sec         |
       |             |                              |
       |     [intercom MCP B (stdio subprocess)]    |
       |             |                              |
       |             | notifications/claude/channel |
       |             v                              |
       |     [Claude Code B]                        |
       |             |                              |
       |             +---> <channel source="session-intercom"
       |                     from="A" message_id="42">hi</channel>
       |                   arrives in B's next turn
```

- Each Claude Code session spawns its own MCP subprocess via stdio
- All subprocesses share a single SQLite database (WAL mode, FK on)
- Each subprocess runs a background tailer task that polls SQLite once per second for messages addressed to its registered session
- The tailer emits `notifications/claude/channel` on its own write stream — the same stream the host CLI is reading
- The CLI delivers each notification as a `<channel>` tag at the start of the next turn (or wakes an idle session)

---

## Quick start (user perspective)

### Install via plugin

```
/plugin marketplace add struktured-labs/claudemarketplace
/plugin install session-intercom@struktured-labs
```

### Launch Claude Code with channels enabled

The Channels API is in research preview. Custom channels aren't on Anthropic's allowlist, so use the dev flag:

```bash
claude --dangerously-load-development-channels plugin:session-intercom@struktured-labs
```

**Gotcha**: the flag has two syntax forms and the wrong one fails silently:

```bash
# Plugin-installed channel (what we ship):
claude --dangerously-load-development-channels plugin:session-intercom@struktured-labs

# Bare .mcp.json entry (NOT what we ship):
claude --dangerously-load-development-channels server:session-intercom
```

Using `server:session-intercom` for a plugin-installed channel will leave delivery broken with no error. We hit this in production.

### Register and send

```python
# In any Claude Code session:
intercom_register(name="my-session-name")

# Send a DM:
intercom_send(to_name="recipient-name", body="Your message here")

# Broadcast:
intercom_broadcast(body="deploy is green", channel="general")
```

After register, all other intercom tools default to this session's name. No per-call identity argument needed.

---

## MCP tool surface

| Tool | Purpose |
|------|---------|
| `intercom_register(name, backlog=10)` | Register, set session identity, auto-subscribe to `general`, and cap replayed history |
| `intercom_send(to_name, body)` | Direct message — your own name is implicit |
| `intercom_broadcast(body, channel="general")` | Broadcast to a channel |
| `intercom_poll()` | Drain via per-sender cursors — **independent of channel notification cursors**; works as recovery if channels silently fail |
| `intercom_list_sessions()` | Discover registered sessions |
| `intercom_history(...)` | Read-only message history with pagination |
| `intercom_subscribe(channel)` | Start receiving a broadcast channel (new traffic only) |
| `intercom_unsubscribe(channel)` | Stop receiving a channel — cuts context noise. DMs unaffected |
| `intercom_list_channels()` | List channels, each flagged with your `subscribed` state |
| `intercom_create_channel(channel_name)` | Create a new broadcast channel |
| `intercom_cleanup(ttl_minutes=...)` | Remove sessions inactive for 2+ weeks (default) |

---

## SQLite schema

If you want to fork to a different storage backend (Redis, Postgres, FoundationDB), this is what you need to replicate. The shape is small.

```sql
CREATE TABLE sessions (
    id TEXT PRIMARY KEY,
    name TEXT NOT NULL UNIQUE,
    created_at TEXT NOT NULL,
    last_heartbeat TEXT NOT NULL,
    metadata TEXT,
    team_name TEXT
);

CREATE TABLE channels (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    name TEXT NOT NULL UNIQUE,
    created_at TEXT NOT NULL,
    description TEXT
);

CREATE TABLE messages (
    id INTEGER PRIMARY KEY AUTOINCREMENT,   -- monotonic ordering matters
    sender_id TEXT NOT NULL,
    recipient_id TEXT,                       -- NULL for broadcasts
    channel TEXT,                            -- NULL for DMs
    body TEXT NOT NULL,
    thread_id INTEGER,
    created_at TEXT NOT NULL,
    FOREIGN KEY (sender_id) REFERENCES sessions(id),
    FOREIGN KEY (recipient_id) REFERENCES sessions(id),
    FOREIGN KEY (thread_id) REFERENCES messages(id)
);

CREATE TABLE channel_subscriptions (
    session_id TEXT NOT NULL,
    channel TEXT NOT NULL,
    subscribed_at TEXT NOT NULL,             -- gates replay: only messages
                                              -- created at/after this land
    PRIMARY KEY (session_id, channel),
    FOREIGN KEY (session_id) REFERENCES sessions(id)
);

CREATE TABLE read_cursors (
    session_id TEXT NOT NULL,
    source TEXT NOT NULL,                    -- '<sender_id>' for DMs,
                                              -- 'ch:<channel>' for channels,
                                              -- 'tailer:channel' for the tailer
    last_read_id INTEGER NOT NULL DEFAULT 0,
    PRIMARY KEY (session_id, source),
    FOREIGN KEY (session_id) REFERENCES sessions(id)
);
```

**Source-key convention** in `read_cursors`:
- `<sender_id>` (uuid) → per-sender DM read cursor (advanced by `intercom_poll`)
- `ch:<channel_name>` → per-channel read cursor (advanced by `intercom_poll`)
- `tailer:channel` → tailer's own cursor (advanced by `db.fetch_for_channel_tailer`)

These three never collide. Keeping the tailer cursor disjoint is what guarantees recovery on silent channel-delivery failure.

---

## Gotchas we discovered (in priority order)

1. **`FastMCP` drops `experimental` capabilities silently.** Always use the low-level `mcp.server.Server`. There is no error; just no deliveries.

2. **Launch flag syntax: `plugin:` vs `server:`.** Plugin-installed channels need `plugin:<name>@<marketplace>`. Bare `.mcp.json` entries need `server:<name>`. Wrong form = silent failure.

3. **Cap the replay on join or you will blow up fresh sessions.** Tailer cursor starts at 0 = full history replay. We measured 786 messages / ~1.5 MB on our own network. Backlog cap + channel subscriptions + per-message truncation.

4. **Tailer must not share cursors with `poll`.** If it does, a failed notification eats the cursor and the message vanishes. We split the cursor in 0.6.1.

5. **Meta keys must be identifiers.** Keys with hyphens, dots, or other non-alphanum-underscore characters are silently dropped by the CLI. Filter at emit time.

6. **Notifications are fire-and-forget at the transport.** `await write_stream.send(...)` resolves when bytes are written, not when Claude has read them. Don't add ack semantics — there aren't any. Build the recovery path you'd need if delivery failed (we did, via the cursor split).

7. **`uvx` caches builds.** A restart doesn't pick up new code if the build is cached. Use `uvx --refresh` or `uvx cache prune` after pushing fixes.

8. **The host has to be launched with `CLAUDE_CODE_DEBUG=1`** if you want to inspect what the CLI does with your notifications. Otherwise `~/.claude/debug/<session-id>.txt` doesn't exist.

9. **Claude Desktop does not support channels** as of 2026-06. CLI only. Desktop's idle sessions can't be woken by external events yet.

10. **The MCP Python SDK breaks across majors, and it breaks you at *runtime*.** Going 1.x → 2.0 moved handler registration (decorators → `add_request_handler`), turned `JSONRPCMessage` from a wrapper model into a bare type union (so the old constructor call raises `TypeError` mid-delivery), and renamed `Tool(inputSchema=)` to `input_schema=`. None of that is caught by importing the module. Pin deliberately, and keep a CI job that does a real stdio handshake against a *fresh* dependency resolution — cached envs keep working and hide the break.

11. **Notifications wake idle sessions.** This is a real behavior change from pre-channels MCP, where servers were passive. With channels, an MCP server can drive a session into action without the user typing anything. Treat this as the powerful primitive it is.

---

## Layout

```
src/session_intercom/
  server.py        — low-level MCP server: tool handlers + channel tailer task
  db/              — SQLite layer
    _common.py     — connection, schema, validators, constants
    sessions.py    — register / heartbeat / list / lookup
    messages.py    — send / broadcast / poll / history / fetch_for_channel_tailer
    channels.py    — list / create
    subscriptions.py — per-session channel subscribe/unsubscribe
    cleanup.py     — TTL-based stale-session sweep
  models.py        — Session, Message, Channel dataclasses
```

## Running tests

```bash
uv run --extra dev pytest tests/ -v
```

49 tests cover: idempotent registration, heartbeat refresh, FK-safe cleanup, cursor-split semantics, backlog capping, subscription gating, MCP tool dispatch, SDK handler registration, the JSON-RPC wire format we emit, and meta-key sanitization.

## Migration from 0.5.x (file-inbox era)

If you're on the pre-channels file-inbox path:

- Drop `team_name=` from `intercom_register` calls — argument is gone
- Drop any handling of `delivery_health`, `inbox_file_ready`, `next_step`, `recovery`, `binding_mismatch`, `unread_in_file_inbox` response fields — all gone
- Delete any `TeamCreate` / `TeamDelete` calls used for intercom setup — not needed
- Drop `intercom_diagnose` calls — tool removed
- Add `--dangerously-load-development-channels plugin:session-intercom@struktured-labs` to your Claude Code launch command

The DB schema is forward-compatible. Existing message history is preserved.

## Requirements

- Python >= 3.11
- `mcp` >= 2.0.0
- Claude Code v2.1.80 or later (channels support)
- `uv` for managing the venv and running

## Upstream

- **Source**: https://github.com/struktured-labs/session-intercom
- **Plugin**: [`struktured-labs/claudemarketplace`](https://github.com/struktured-labs/claudemarketplace), `plugins/session-intercom/`
- **Channels API docs**: https://code.claude.com/docs/en/channels-reference
- **Tracking issue that drove channels into the CLI**: https://github.com/anthropics/claude-code/issues/33679

## License

MIT
