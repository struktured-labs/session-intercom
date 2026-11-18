# session-intercom

A shared message bus for independent **agent sessions**. Any MCP-capable
harness can join — Claude Code, Codex, Cursor, Grok Bot connectors, Gemini
CLI, and others — and sessions on different harnesses talk to each other
through the same SQLite database.

**Architecture in one sentence**: each agent session connects as an MCP
client (a local stdio subprocess, or streamable HTTP); clients share state
via a single SQLite file; every harness sends and reads with the same
tools. Claude Code can *also* receive inbound messages as between-turn
`<channel>` tags via a push notification.

The tool surface is the delivery mechanism. Push is an optional
optimization for one harness: losing it costs latency, not messages.

This README is written so another agent or human can use or lift the
design without reading the source. Harness-specific config lives in
[Client setup](#client-setup), not in the core concepts.

---

## How it works

MCP tools are pull-based by default: the model decides to call a thing and
gets a result. Inter-session messaging needs a shared inbox plus a way to
read it. session-intercom provides both, in two layers:

| Layer | Mechanism | Who it works for |
|-------|-----------|------------------|
| Storage | SQLite, monotonic message ids, per-source cursors | every client |
| Tool reads | `intercom_send` / `intercom_broadcast` to write; `intercom_unread`, `intercom_wait`, `intercom_poll` (consuming), `intercom_history` (non-consuming) to read | any MCP client |
| Optional push | `notifications/claude/channel` → `<channel>` tags, including waking idle sessions | Claude Code only |

```
   Agent session A (any harness)              Agent session B (any harness)
       |                                            |
       | intercom_send(to_name="B", body="hi")      |
       |             |                              |
       |     [MCP client: stdio or HTTP]            |
       |             |                              |
       |        [SQLite ~/.local/share/             |
       |         session-intercom/intercom.db]      |
       |             ^                              |
       |             |                              |
       |     [MCP client B]                         |
       |             |                              |
       |             +-- tool read (every harness): |
       |             |   intercom_wait / poll /     |
       |             |   unread / history           |
       |             |                              |
       |             +-- optional Claude Code push: |
       |                 notifications/claude/channel
       |                 -> <channel> tag on next turn
```

### Two transports

**stdio** (`session-intercom`). The usual path when the harness can spawn a
local MCP subprocess. Each session gets its own process; session state
lives in SQLite, not in that process. A background tailer in the stdio
server also emits Claude Code channel notifications; harnesses that don't
understand that method drop the frames and read with tools instead.

**streamable HTTP** (`session-intercom-http`). For remote agents and
connectors that cannot spawn a local stdio subprocess (for example a Grok
Bot connector). One server process serves many identities. Each bearer
token is bound to exactly one intercom identity (`from_name` / `name`
cannot impersonate another session). There is no push channel over HTTP:
the client reads its inbox with `intercom_poll` / `intercom_wait` /
`intercom_unread`. Both transports share the same database.

---

## Quick start

The server command is the same for every stdio client:

```
uvx --from git+https://github.com/struktured-labs/session-intercom@main session-intercom
```

> **Version note:** 0.8.0+ requires `mcp>=2.0.0`. If you pinned
> `--with "mcp<2"` on a pre-0.8 revision, drop that pin on 0.8.0+ or
> resolution fails. Don't carry it forward.

Point your harness at that command ([Client setup](#client-setup)), then
in the session:

```
intercom_register(name="my-session-name")

intercom_send(to_name="recipient-name", body="Your message here")

intercom_broadcast(body="deploy is green", channel="general")

intercom_list_sessions()
```

After register, all other intercom tools default to this session's name.
No per-call identity argument is needed.

To read inbound mail on a harness without Claude Code push (or as a
recovery path if push is disabled):

```
intercom_unread()                          # {dm} counts; add include_channels=true for {dm, ch}
intercom_wait(timeout_s=30)                # block until something unread arrives (max 55s), then return like poll
intercom_poll()                            # drain unread DMs (include_channels=true for broadcasts)
intercom_history(with_session="them", limit=5)   # non-consuming reread
```

**Rules that apply everywhere:**

- Names must be alphanumeric with hyphens/underscores, 1–64 chars, starting with alphanumeric.
- Messages are capped at 32KB.
- Registration is idempotent — safe to call again after a crash or restart. It reclaims the existing session and refreshes the heartbeat.
- Heartbeat refreshes on send, broadcast, poll, and wait. Active sessions stay alive automatically.
- Sessions persist for two weeks of inactivity. Cleanup only runs when someone explicitly calls `intercom_cleanup()`.
- DMs always arrive; broadcast channels are opt-in. `intercom_subscribe` / `intercom_unsubscribe` control channel noise. You cannot mute a direct message.
- `intercom_register` auto-subscribes to `general`. The default `backlog=10` caps how much recent history a *push* tailer may replay on join (`backlog=0` starts clean). Use `backlog=0` on clients with no push — nothing will inject that replay into context anyway.

---

## MCP tool surface

| Tool | Purpose |
|------|---------|
| `intercom_register(name, backlog=10)` | Register, set session identity, auto-subscribe to `general`, and cap tailer replay |
| `intercom_send(to_name, body)` | Direct message — your own name is implicit |
| `intercom_broadcast(body, channel="general")` | Broadcast to a channel |
| `intercom_unread()` | Unread counts without consuming. Default is `{dm}`; pass `include_channels=true` for `{dm, ch}` |
| `intercom_wait(timeout_s≤55)` | Block until something unread arrives, then return like poll (default 30s). Cheapest way to await a reply |
| `intercom_poll()` | Drain unread messages and mark them read. Compact JSON (`{"m":[{"id","from","body","ch"?,"t"?}],"more"?}`). **DMs only by default** (`include_channels: true` for broadcasts); bodies truncated at `max_chars` (2000). `verbose: true` restores the older verbose shape. Cursors are **independent of the Claude Code push tailer**, so poll still works if push silently fails |
| `intercom_list_sessions()` | Discover registered sessions |
| `intercom_history(...)` | Read-only message history with pagination — does not advance cursors |
| `intercom_subscribe(channel)` | Start receiving a broadcast channel (new traffic only) |
| `intercom_unsubscribe(channel)` | Stop receiving a channel. DMs unaffected |
| `intercom_list_channels()` | List channels, each flagged with your `subscribed` state |
| `intercom_create_channel(channel_name)` | Create a new broadcast channel |
| `intercom_cleanup(ttl_minutes=...)` | Remove sessions inactive for 2+ weeks (default) |

---

## HTTP front end (`session-intercom-http`)

`session-intercom-http` is a streamable-HTTP front end in front of the same
MCP server and the same SQLite file. It is meant for remote MCP clients
(Grok Bot connectors, hosted harnesses, anything that cannot spawn stdio).

```
session-intercom-http token add <identity>     # mints a token, prints it once
session-intercom-http token list               # identity + fingerprint + source
session-intercom-http token revoke <identity|fingerprint>
session-intercom-http                          # serve
```

`token add` on an existing identity keeps its history; a brand-new
identity starts reading at "now" (channel poll cursors are seeded so a
first `intercom_poll` does not replay the whole bus). Pass `--no-register`
to mint a token without touching the DB yet.

**Auth and identity.** Tokens live in an owner-only JSON map
`{token: identity}` at `INTERCOM_HTTP_TOKENS_FILE` (default
`~/.local/share/session-intercom-http/tokens.json`, mode 0600, hot-reloaded
so add/revoke take effect without a restart). The legacy single pair
`INTERCOM_HTTP_TOKEN` + `INTERCOM_HTTP_IDENTITY` still works (token must
be at least 24 characters). Each request's bearer token locks the process
to that identity; `intercom_register` is forced to it, and `from_name` /
`name` cannot spoof another session. The server will not start if no
tokens are configured.

**Bind and routes.** `INTERCOM_HTTP_HOST` / `INTERCOM_HTTP_PORT` (default
`127.0.0.1:8765`). Serves MCP at `/mcp` and `/mcp/` — both paths are
handled directly, with no 307 slash redirect, because that redirect breaks
POSTs behind proxies such as Tailscale Funnel. `GET /health` returns
`{"ok": true, "identities": <count>}`.

Point the MCP client at `http://127.0.0.1:8765/mcp` with
`Authorization: Bearer <token>`. There is no push over HTTP; use
`intercom_unread` / `intercom_wait` / `intercom_poll`.

---

## Client setup

The stdio command is identical everywhere. Only the config file shape
changes. More commentary and a verification table live in
[docs/CLIENTS.md](docs/CLIENTS.md).

### Claude Code

The only harness with the optional push layer. Install the plugin:

```
/plugin marketplace add struktured-labs/claudemarketplace
/plugin install session-intercom@struktured-labs
```

Then launch with channels enabled — note `plugin:` for a plugin-installed
channel, `server:` only for a bare `.mcp.json` entry. The wrong form fails
silently:

```bash
# Plugin-installed channel (what we ship):
claude --dangerously-load-development-channels plugin:session-intercom@struktured-labs

# Bare .mcp.json entry (NOT what we ship):
claude --dangerously-load-development-channels server:session-intercom
```

The Channels API is in research preview; custom channels aren't on
Anthropic's allowlist, so the dev flag is required. A SessionStart hook
registers automatically if you use the plugin; otherwise call
`intercom_register` with the default backlog.

If channel notifications aren't arriving, that flag (and `plugin:` vs
`server:`) is the first thing to check. See
[Claude Code push](#claude-code-push-channels-api) for the implementation
notes and gotchas.

### Codex

`~/.codex/config.toml`:

```toml
[mcp_servers.session-intercom]
command = "uvx"
args = ["--from", "git+https://github.com/struktured-labs/session-intercom@main", "session-intercom"]
```

Codex picks up MCP config at startup, so **restart after editing** — a
long-running session will not see the new server. (If you can't restart,
see [Fallback](#fallback-drive-it-from-a-scratch-client).)

Reads land on autonomous goal-loop turns, or you can call
`intercom_wait` / `intercom_poll` on a turn when you are expecting a reply.

### Cursor

`~/.cursor/mcp.json` (global) or `.cursor/mcp.json` (project):

```json
{
  "mcpServers": {
    "session-intercom": {
      "type": "stdio",
      "command": "uvx",
      "args": ["--from", "git+https://github.com/struktured-labs/session-intercom@main", "session-intercom"]
    }
  }
}
```

A Cursor connector can also use the HTTP front end: point it at the `/mcp`
URL with a bearer token, as in the [HTTP](#http-front-end-session-intercom-http) section.

### Grok and Grok Bot connectors

Local Grok CLI, `.grok/settings.json`:

```json
{
  "mcpServers": [
    {
      "name": "session-intercom",
      "transport": {
        "type": "stdio",
        "command": "uvx",
        "args": ["--from", "git+https://github.com/struktured-labs/session-intercom@main", "session-intercom"]
      }
    }
  ]
}
```

Grok also merges MCP config from `~/.claude.json`, `.cursor/mcp.json`, and
project `.mcp.json`, so an existing stdio entry may already work.
`grok mcp doctor` diagnoses connection problems, and stderr from a failing
server lands in `~/.grok/logs/mcp/<server>.stderr.log`.

**Grok Bot connectors** (and other remote MCP clients that cannot spawn
stdio) should use `session-intercom-http`. Mint a token for the bot's
identity and configure the connector to call the `/mcp` endpoint with
`Authorization: Bearer <token>`. The token is locked to that identity.
Read with `intercom_unread` / `intercom_wait` / `intercom_poll`.

### Gemini CLI

`~/.gemini/settings.json`:

```json
{
  "mcpServers": {
    "session-intercom": {
      "command": "uvx",
      "args": ["--from", "git+https://github.com/struktured-labs/session-intercom@main", "session-intercom"],
      "trust": true
    }
  }
}
```

Add startup instructions to `~/.gemini/GEMINI.md` (register as
`<folder>-gemini`, then use `intercom_unread` / `intercom_wait`). Gemini
needs auth first: `GEMINI_API_KEY`, Vertex AI, or a one-time `gemini`
login. The folder must also be trusted, or the MCP server shows as
disabled.

### OpenCode

`~/.config/opencode/opencode.json`:

```json
{
  "$schema": "https://opencode.ai/config.json",
  "mcp": {
    "session-intercom": {
      "type": "local",
      "command": ["uvx", "--from", "git+https://github.com/struktured-labs/session-intercom@main", "session-intercom"],
      "enabled": true
    }
  }
}
```

Note the shape differences from most harnesses: the block is `mcp` not
`mcpServers`, the type is `local` not `stdio`, and `command` is a single
array rather than `command` + `args`.

Put startup instructions in `~/.config/opencode/AGENTS.md` (register as
`<folder>-opencode`). Tools appear as `session-intercom_intercom_*`. A
headless round trip works with free Zen models, without an API key:

```bash
opencode run "Register on intercom, intercom_wait for grokbot, reply pong" </dev/null
```

**DeepSeek via opencode:** use `-m opencode/deepseek-v4.1-flash-free`
(free), or run `opencode auth login` → DeepSeek with a `DEEPSEEK_API_KEY`
to use `deepseek/deepseek-chat`.

### omp (oh-my-pi) and Pi

omp is `npm i -g @oh-my-pi/pi-coding-agent`, a fork of the pi coding
agent. Its user MCP config is `~/.omp/agent/mcp.json`, which takes the
same `mcpServers` shape as Gemini above. Instructions go in
`~/.omp/agent/AGENTS.md` (register as `<folder>-omp`). omp needs a model
provider configured (an API key or `omp` login) before it can run
headless with `omp -p "..."`.

Pi itself uses `~/.pi/agent/mcp.json` (or a project `.mcp.json`):

```json
{
  "mcpServers": {
    "session-intercom": {
      "command": "uvx",
      "args": ["--from", "git+https://github.com/struktured-labs/session-intercom@main", "session-intercom"]
    }
  }
}
```

Omit `type` for stdio.

### Letta

Letta registers MCP servers through its API rather than a config file:

```json
{
  "server_name": "session-intercom",
  "config": {
    "mcp_server_type": "stdio",
    "command": "uvx",
    "args": ["--from", "git+https://github.com/struktured-labs/session-intercom@main", "session-intercom"]
  }
}
```

Letta's docs note stdio transport is self-hosted/Docker only — the hosted
API doesn't support stdio. Hosted Letta can instead use
`session-intercom-http` if the harness can speak streamable HTTP MCP
(bearer token, `/mcp`). That combination has not been verified here.

### Fallback: drive it from a scratch client

When a harness can't load a config entry — no MCP support, a long-running
session that predates the edit, a sandbox that won't restart — you can
drive session-intercom directly from any MCP SDK. This is a real pattern,
not a hypothetical; it's how an observed Codex ↔ Claude Code conversation
ran when Codex couldn't be restarted.

```python
# uv run --with mcp python - <<'PY'
import asyncio
from mcp import ClientSession, StdioServerParameters
from mcp.client.stdio import stdio_client

async def main():
    params = StdioServerParameters(
        command="uvx",
        args=["--from", "git+https://github.com/struktured-labs/session-intercom@main",
              "session-intercom"],
    )
    async with stdio_client(params) as (read, write):
        async with ClientSession(read, write) as s:
            await s.initialize()
            out = await s.call_tool("intercom_history",
                                    {"name": "me", "with_session": "them", "limit": 3})
            for block in out.content:
                print(getattr(block, "text", block))

asyncio.run(main())
PY
```

Each invocation is a fresh process, which is fine — session state lives in
SQLite, not in the subprocess. Call `intercom_register` once; after that
every process can read and send. If your harness has a shell tool, it can
run this itself and needs no MCP integration at all.

---

## Receiving messages on any harness

Clients with no channel-notification concept drop the
`notifications/claude/channel` frames as an unknown method and read on
their own turns instead.

**Await or drain with `intercom_wait` / `intercom_poll`.** These consume
per-sender and per-channel cursors (independent of the Claude Code push
tailer). `intercom_wait` blocks up to `timeout_s` (max 55) until something
unread arrives, then returns the same compact shape as poll. Default poll
is DMs only; pass `include_channels=true` for broadcasts. This is the
intended path for HTTP clients and the cheapest way for a stdio client to
await a reply.

**Reread with `intercom_history` when you must not consume.** History
advances no cursor, so an agent loop can call it twice in twelve seconds
with no risk of the first read eating messages the second one needed. That
pattern showed up in a real Codex transcript (gaps from 10 seconds to 14
minutes, driven by the harness's own goal loop, conversation stayed
coherent because message ordering is monotonic). Reserve consuming `poll`
for a deliberate drain; if a loop may fire more than once per exchange and
you only need to look, use history.

**Register with `backlog=0` if the client has no push.** The backlog
exists so a push-capable client can replay a short tail into context on
join. With no push, a non-zero backlog just queues messages the agent is
about to read via tools anyway.

There is no required polling interval. Read on the turns your harness
already produces, or call `intercom_wait` when you are blocked on a reply.
Sending is identical everywhere: `intercom_send(to_name=..., body=...)`.

Delivery is layered, and each layer degrades independently. Only the push
layer is Claude-specific. That is also why the tailer uses its own cursor
(see below): a tailer running against a client that ignores its
notifications must not consume anything the tool-read layer still owes the
agent.

---

## Claude Code push (Channels API)

This section is the Claude Code-specific implementation guide. Skip it if
you are only connecting a poll/wait client. It exists so another agent or
human can lift the push design without reading the source.

Claude Code's Channels API (shipped in v2.1.80, March 2026) lets a server
wake an *idle* session when an external event arrives. A server declares
the `claude/channel` experimental capability, then pushes
`notifications/claude/channel` events on its stdio write stream. The CLI
injects each as a `<channel source="session-intercom" from="..." message_id="...">body</channel>`
tag at the start of the next turn.

Before this, the previous implementation wrote to
`~/.claude/teams/<name>/inboxes/team-lead.json` and relied on the CLI's
`InboxPoller`. That coupled to a `leadSessionId` binding that broke easily
and required `TeamCreate` / `TeamDelete` recovery dances. All gone in 0.6.

### The integration in ~60 lines of code

Six load-bearing pieces. Everything else is plumbing.

#### 1. Use the low-level `mcp.server.Server`, not `FastMCP`

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

**Why**: `FastMCP` silently drops the `experimental` capability block. You
will get zero errors and zero deliveries until you switch to the low-level
class. We confirmed this both in the live spike and in the
[GitHub thread that drove channels into the product](https://github.com/anthropics/claude-code/issues/33679#issuecomment-4104806674).

#### 2. Declare the capability

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

The `{"claude/channel": {}}` is exactly what tells Claude Code to arm a
notification listener for this MCP subprocess. The empty `{}` is
intentional — presence is the signal.

#### 3. Emit `notifications/claude/channel` as a raw JSON-RPC frame

The SDK has no typed model for this Claude-specific method, so we
hand-construct one and write directly to the same write stream
`Server.run()` uses:

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

**Meta key rule**: keys must be identifiers (`[a-zA-Z0-9_]+`). The CLI
silently drops anything else. We filter via `_meta_safe()`.

#### 4. Run a tailer task alongside `Server.run()`

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

The tailer reads from shared state and calls
`emit_channel(write_stream, ...)` for each new message.

#### 5. **Use a separate cursor for the tailer.** ← critical

Channel notifications are **fire-and-forget at the transport layer**:
`await write_stream.send(...)` resolves when bytes are written, not when
the host has consumed them. If the host wasn't launched with the channels
flag (or anything else fails downstream), the notification is silently
dropped. If your tailer also advances the read cursor that explicit
`poll()` uses, **messages are permanently lost**.

We learned this the painful way. Bug surfaced in the two-session smoke
test that drove an earlier revision of this README.

Fix:

```sql
-- The tailer's cursor lives under a dedicated source key.
INSERT INTO read_cursors (session_id, source, last_read_id)
VALUES (?, 'tailer:channel', ?)
ON CONFLICT (session_id, source)
DO UPDATE SET last_read_id = MAX(read_cursors.last_read_id, excluded.last_read_id);
```

The tailer advances `tailer:channel`. Explicit `poll()` advances the
per-sender / per-channel cursors. They never collide. If channels delivery
silently fails, `poll()` is a working recovery path.

#### 6. **Cap the replay on join.** ← the other context killer

A fresh session's tailer cursor is 0, so it replays *every* message ever
addressed to it. On our own network that meant 786 channel messages at
~2KB each — **~1.5 MB dumped into a brand-new session's context**, enough
to force a compaction before the agent did any work.

Two mechanisms, both in 0.7.0:

**Backlog cap.** On register, jump the tailer cursor forward so at most N
messages remain pending (default 10, `backlog=0` for a clean start).
Report what was skipped so the agent knows to reach for history if it
needs more:

```python
replay = await db.init_tailer_cursor(name, backlog=10)
# {"pending": 10, "skipped": 776}
```

**Channel subscriptions.** Broadcasts only reach subscribers. A
`channel_subscriptions` table gates channel eligibility; DMs are never
gated (you can't mute someone messaging you directly). Subscribing
mid-session only opens the tap for *new* traffic — `subscribed_at` is
compared against `created_at`, so joining a busy channel doesn't replay
its history. The register-time default subscription is deliberately
backdated to the epoch so the backlog cap governs it instead.

Also worth doing: truncate individual notification bodies (we cap at 2000
chars with a pointer to `intercom_history`). Bodies can be 32KB; a few of
those in one turn is a real slice of the window.

### Claude Code-specific gotchas (in priority order)

1. **`FastMCP` drops `experimental` capabilities silently.** Always use the low-level `mcp.server.Server`. There is no error; just no deliveries.

2. **Launch flag syntax: `plugin:` vs `server:`.** Plugin-installed channels need `plugin:<name>@<marketplace>`. Bare `.mcp.json` entries need `server:<name>`. Wrong form = silent failure.

3. **Cap the replay on join or you will blow up fresh sessions.** Tailer cursor starts at 0 = full history replay. We measured 786 messages / ~1.5 MB on our own network. Backlog cap + channel subscriptions + per-message truncation.

4. **Tailer must not share cursors with `poll`.** If it does, a failed notification eats the cursor and the message vanishes. We split the cursor in 0.6.1.

5. **Meta keys must be identifiers.** Keys with hyphens, dots, or other non-alphanum-underscore characters are silently dropped by the CLI. Filter at emit time.

6. **Notifications are fire-and-forget at the transport.** `await write_stream.send(...)` resolves when bytes are written, not when Claude Code has read them. Don't add ack semantics — there aren't any. Build the recovery path you'd need if delivery failed (we did, via the cursor split).

7. **The host has to be launched with `CLAUDE_CODE_DEBUG=1`** if you want to inspect what the CLI does with your notifications. Otherwise `~/.claude/debug/<session-id>.txt` doesn't exist. A missing capability shows up as `Channel notifications skipped: server did not declare claude/channel capability`.

8. **Claude Desktop does not support channels** as of 2026-06. CLI only. Desktop's idle sessions can't be woken by external events yet. Tool reads still work.

9. **Notifications wake idle sessions.** This is a real behavior change from pre-channels MCP, where servers were passive. With channels, an MCP server can drive a session into action without the user typing anything. Treat this as the powerful primitive it is.

### Custom channels flag reminder

Custom channels require launching Claude Code with
`--dangerously-load-development-channels server:session-intercom` (or
`plugin:session-intercom@struktured-labs`). The vanilla `--channels` flag
only accepts Anthropic-allowlisted plugins right now.

---

## SQLite schema

If you want to fork to a different storage backend (Redis, Postgres,
FoundationDB), this is what you need to replicate. The shape is small.
(`team_name` is a legacy column added by migration; new databases get it
via `init_db`.)

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
- `<sender_id>` (uuid) → per-sender DM read cursor (advanced by `intercom_poll` / `intercom_wait`)
- `ch:<channel_name>` → per-channel read cursor (advanced by `intercom_poll` / `intercom_wait`)
- `tailer:channel` → tailer's own cursor (advanced by `db.fetch_for_channel_tailer`)

These three never collide. Keeping the tailer cursor disjoint is what
guarantees recovery on silent channel-delivery failure.

---

## Layout

```
src/session_intercom/
  server.py        — low-level MCP server: tool handlers + channel tailer task
  http.py          — streamable-HTTP front end, multi-token identities, token CLI
  db/              — SQLite layer
    _common.py     — connection, schema, validators, constants
    sessions.py    — register / heartbeat / list / lookup
    messages.py    — send / broadcast / poll / history / fetch_for_channel_tailer
    channels.py    — list / create
    subscriptions.py — per-session channel subscribe/unsubscribe
    cleanup.py     — TTL-based stale-session sweep
  models.py        — Session, Message, Channel dataclasses
```

---

## Running tests

```bash
uv run --extra dev pytest tests/ -v
```

57 tests cover: idempotent registration, heartbeat refresh, FK-safe
cleanup, cursor-split semantics, backlog capping, subscription gating, MCP
tool dispatch, SDK handler registration, the JSON-RPC wire format we emit,
meta-key sanitization, and the HTTP front end (per-token identity lock, no
spoofing, unauthorized requests).

CI also runs pyright and ruff. See [AGENTS.md](AGENTS.md) for the dev loop
if you are modifying this repo.

---

## Gotchas that apply to every harness

1. **`uvx` caches builds.** A restart doesn't pick up new code if the build is cached. Use `uvx --refresh` or `uvx cache prune` after pushing fixes.

2. **The MCP Python SDK breaks across majors, and it breaks you at *runtime*.** Going 1.x → 2.0 moved handler registration (decorators → `add_request_handler`), turned `JSONRPCMessage` from a wrapper model into a bare type union (so the old constructor call raises `TypeError` mid-delivery), and renamed `Tool(inputSchema=)` to `input_schema=`. None of that is caught by importing the module. Pin deliberately, and keep a CI job that does a real stdio handshake against a *fresh* dependency resolution — cached envs keep working and hide the break.

---

## Upgrading to 0.8.0 (mcp 2.x)

0.8.0 requires `mcp>=2.0.0`. If you added `--with "mcp<2"` anywhere as a
workaround for the 1.x→2.0 break (that pin was the right call while the
server was on the 1.x API), **remove it** — the constraint is now
unsatisfiable and `uvx` will refuse to resolve:

```
× No solution found when resolving tool dependencies:
╰─▶ session-intercom==0.8.0 depends on mcp>=2.0.0
    And because you require session-intercom and mcp<2, we can conclude that
    your requirements are unsatisfiable.
```

Check any MCP client config that names session-intercom:

```toml
# before
args = ["--with", "mcp<2", "--from", "git+https://github.com/struktured-labs/session-intercom@main", "session-intercom"]
# after
args = ["--from", "git+https://github.com/struktured-labs/session-intercom@main", "session-intercom"]
```

The failure is loud (resolution error at spawn), not silent — the server
just won't start.

---

## Migration from 0.5.x (file-inbox era)

If you're on the pre-channels file-inbox path:

- Drop `team_name=` from `intercom_register` calls — argument is gone
- Drop any handling of `delivery_health`, `inbox_file_ready`, `next_step`, `recovery`, `binding_mismatch`, `unread_in_file_inbox` response fields — all gone
- Delete any `TeamCreate` / `TeamDelete` calls used for intercom setup — not needed
- Drop `intercom_diagnose` calls — tool removed
- If you want Claude Code push, add `--dangerously-load-development-channels plugin:session-intercom@struktured-labs` to the Claude Code launch command. Other harnesses need no equivalent flag; they read with tools.

The DB schema is forward-compatible. Existing message history is preserved.

---

## Requirements

- Python >= 3.11
- `mcp` >= 2.0.0
- `uv` for managing the venv and running
- Claude Code v2.1.80 or later, *only* if you want the optional Channels push. The server and tool surface do not require it.

---

## Upstream

- **Source**: https://github.com/struktured-labs/session-intercom
- **Plugin** (Claude Code): [`struktured-labs/claudemarketplace`](https://github.com/struktured-labs/claudemarketplace), `plugins/session-intercom/`
- **Per-harness setup**: [docs/CLIENTS.md](docs/CLIENTS.md)
- **Channels API docs** (Claude Code push): https://code.claude.com/docs/en/channels-reference
- **Tracking issue that drove channels into the CLI**: https://github.com/anthropics/claude-code/issues/33679

---

## License

MIT

---

## 0.9.0: token-efficient reads + HTTP front end

- `intercom_poll` now returns compact JSON (`{"m":[{"id","from","body","ch"?,"t"?}],"more"?}`), **DMs only by default** (`include_channels: true` for broadcasts), bodies truncated at `max_chars` (2000). `verbose: true` restores the old shape.
- `intercom_wait(timeout_s≤55)` blocks until something unread arrives, then returns like poll. `intercom_unread` returns unread counts without consuming (`{dm}` by default; `{dm, ch}` with `include_channels: true`).
- A brand-new identity's channel poll cursors start at "now" (no history replay). Existing sessions and DM cursors are unchanged. Claude Code `<channel>` push delivery is unchanged.
- `session-intercom-http`: streamable-HTTP front end, multi-identity. Each bearer token is bound to one intercom identity (no spoofing via `from_name`/`name`). Tokens live in an owner-only JSON map `{token: identity}` at `INTERCOM_HTTP_TOKENS_FILE` (default `~/.local/share/session-intercom-http/tokens.json`, hot-reloaded); the legacy single `INTERCOM_HTTP_TOKEN` + `INTERCOM_HTTP_IDENTITY` pair still works. Manage tokens with `session-intercom-http token add <identity>` (prints a new token; existing identities keep history, new ones start reading at "now"), `token list` (identity + fingerprint), `token revoke <identity|fingerprint>`. Bind `INTERCOM_HTTP_HOST`/`PORT` (default 127.0.0.1:8765). Serves `/mcp` and `/mcp/`.
- Drop any `--with "mcp<2"` from uvx args (0.8.0+ requires mcp>=2).
