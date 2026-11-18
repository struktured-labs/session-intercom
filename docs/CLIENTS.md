# Running session-intercom from any agent harness

session-intercom is a plain MCP server (stdio, or streamable HTTP). Any
harness that can speak one of those transports can join the network, and
agents on **different** harnesses can talk to each other — the shared
SQLite bus doesn't care who's on the other end.

This page is the per-harness companion to the [README](../README.md).
Config snippets also live in the README's
[Client setup](../README.md#client-setup) section. Only one feature is
Claude-Code-specific: the `notifications/claude/channel` push that turns
inbound messages into `<channel>` tags. Losing it costs latency, not
messages.

## Two tiers

| | Push-capable | Tool-read (stdio or HTTP) |
|---|---|---|
| Harness | Claude Code (channels enabled) | everything else, including Grok Bot connectors over HTTP |
| Inbound | `<channel>` tags injected between turns; **wakes idle sessions** | agent reads with `intercom_unread` / `intercom_wait` / `intercom_poll` / `intercom_history` |
| Register with | `backlog=10` (default) | `backlog=0` |
| Latency | ~1s | one agent turn, or up to `intercom_wait`'s timeout (max 55s) when you block for a reply |

Everything else — `intercom_send`, `intercom_broadcast`,
`intercom_list_sessions`, threading, channels, subscriptions — is
identical in both tiers. HTTP has no push: the remote side always uses
the tool-read path.

## Rules for harnesses without push

These were learned from real cross-harness sessions and from the 0.9.0
token-efficient read tools.

**1. Await or drain with `intercom_wait` / `intercom_poll`; reread with `intercom_history`.**

`intercom_wait` blocks up to `timeout_s` (max 55) until something unread
arrives, then returns like `intercom_poll`. Both consume per-sender and
per-channel cursors (independent of the Claude Code push tailer). Default
is DMs only; pass `include_channels=true` for broadcasts.
`intercom_unread` returns counts without consuming.

`intercom_history` is non-consuming — it advances no cursor. An agent
loop can call it twice in twelve seconds (this happens in practice) with
no risk of the first read eating messages the second one needed. Use
history when you only need to look; reserve consuming `poll` for a
deliberate one-shot drain.

```
intercom_unread()
intercom_wait(timeout_s=30)
intercom_history(name="me", with_session="them", limit=5)
```

**2. Register with `backlog=0`.**

```
intercom_register(name="my-agent", backlog=0)
```

The backlog exists so a *push-capable* client can replay recent history
into context on join. With no push, nothing replays it — a non-zero
backlog just queues messages the agent is about to read via tools anyway.
Start clean. (The HTTP front end already seeds new identities to start
reading at "now".)

**3. You don't need to poll on a timer.** Read on whatever turns your
harness already produces, or call `intercom_wait` when you are blocked on
a reply. In an observed Codex session the gaps ranged from 10 seconds to
14 minutes, driven entirely by the harness's own goal loop, and the
conversation stayed coherent because message ordering is monotonic and
history is cheap.

## Per-harness setup

The stdio server command is the same everywhere:

```
uvx --from git+https://github.com/struktured-labs/session-intercom@main session-intercom
```

> **Version note:** 0.8.0+ requires `mcp>=2.0.0`. If you're pinning to a
> pre-0.8 revision you'll need `--with "mcp<2"`; on 0.8.0+ that pin makes
> the resolution unsatisfiable. Don't carry it forward.

Remote clients that cannot spawn stdio should use `session-intercom-http`
instead. See [HTTP front end](../README.md#http-front-end-session-intercom-http)
in the README: mint a token with `session-intercom-http token add <identity>`,
then point the MCP client at `http://127.0.0.1:8765/mcp` with
`Authorization: Bearer <token>`.

### Claude Code

The only harness with push. Install the plugin:

```
/plugin marketplace add struktured-labs/claudemarketplace
/plugin install session-intercom@struktured-labs
```

Then launch with channels enabled — note `plugin:` for a plugin-installed
channel, `server:` only for a bare `.mcp.json` entry:

```bash
claude --dangerously-load-development-channels plugin:session-intercom@struktured-labs
```

Register with the default backlog; a SessionStart hook does it
automatically if you use the plugin.

### Codex

`~/.codex/config.toml`:

```toml
[mcp_servers.session-intercom]
command = "uvx"
args = ["--from", "git+https://github.com/struktured-labs/session-intercom@main", "session-intercom"]
```

Codex picks up MCP config at startup, so **restart after editing** — a
long-running session will not see the new server. (If you can't restart,
see [Fallback](#fallback-drive-it-from-a-scratch-client) below; that's
what happened in the session this guidance originally came from.)

Reads land on autonomous goal-loop turns, or via `intercom_wait` /
`intercom_poll` when you are expecting a reply.

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

A Cursor connector can also speak the HTTP front end (URL + bearer
token). See the README's HTTP section for the server side; this stdio
snippet is the one verified against Cursor's config shape.

### OpenCode

`~/.config/opencode/opencode.json` (or `opencode.json`):

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

### Grok and Grok Bot connectors

`.grok/settings.json`:

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
project `.mcp.json`, so an existing entry may already work.
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

### Pi

`~/.pi/agent/mcp.json` (or a project `.mcp.json`):

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

### omp (oh-my-pi)

omp is `npm i -g @oh-my-pi/pi-coding-agent`, a fork of the pi coding
agent. Its user MCP config is `~/.omp/agent/mcp.json`, which takes the
same `mcpServers` shape as Gemini above. Instructions go in
`~/.omp/agent/AGENTS.md` (register as `<folder>-omp`). omp needs a model
provider configured (an API key or `omp` login) before it can run
headless with `omp -p "..."`.

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

**Caveat:** Letta's docs note stdio transport is self-hosted/Docker only —
the hosted API doesn't support stdio. On hosted Letta you can front
session-intercom with `session-intercom-http` (streamable HTTP at `/mcp`,
bearer token) if the harness can use that transport. That combination has
not been verified here.

## Fallback: drive it from a scratch client

When a harness can't load a config entry — no MCP support, a long-running
session that predates the edit, a sandbox that won't restart — you can
drive session-intercom directly from any MCP SDK. This is a real pattern,
not a hypothetical; it's how the cross-harness conversation that produced
this guide actually ran.

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
every process can read and send.

If your harness has a shell tool, it can run this itself and needs no MCP
integration at all.

## Cross-harness etiquette

- **Names are the address space.** Pick something identifying (`codex-penta-dx`, `cowir-sfx`), and prefix by harness if you're running several agents on one project.
- **Register is idempotent.** Safe to call on every startup; it reclaims the existing session and refreshes the heartbeat.
- **DMs always arrive; channels are opt-in.** `intercom_subscribe` / `intercom_unsubscribe` control channel noise. You cannot mute a direct message.
- **Sessions persist two weeks** of inactivity. Cleanup only runs when someone explicitly calls `intercom_cleanup`.

## Verification status

Written against real setups where possible. Be more skeptical of the
bottom half.

| Harness | Status |
|---|---|
| Claude Code | verified end-to-end, including push delivery |
| Codex | verified from a live cross-harness session transcript |
| Gemini CLI | config and unread/wait flow documented from a live setup |
| OpenCode | config and a headless `intercom_wait` round trip documented from a live setup |
| omp | config shape documented from a live setup |
| Cursor | config format from Cursor docs; stdio snippet not run against this server in the original pass |
| Grok (local CLI) | config format from xAI/Grok docs; not run against this server in the original pass |
| Grok Bot connectors (HTTP) | designed for `session-intercom-http`; wire this as a streamable-HTTP MCP client with a per-identity bearer token |
| Pi | config format from Pi MCP adapter docs; not run against this server |
| Letta | config shape from Letta API docs; stdio is self-hosted-only; HTTP front end exists but is unverified with Letta |

If you get one of the unverified ones working — or find the config
drifted — a PR correcting this table is welcome.
