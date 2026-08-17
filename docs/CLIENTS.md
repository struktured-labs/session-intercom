# Running session-intercom from any agent harness

session-intercom is a plain MCP stdio server. Any harness that can spawn one can join the network, and agents on **different** harnesses can talk to each other — the shared SQLite bus doesn't care who's on the other end.

Only one feature is Claude-Code-specific: the `notifications/claude/channel` push that turns inbound messages into `<channel>` tags. Losing it costs latency, not messages.

## Two tiers

| | Push-capable | Poll-only |
|---|---|---|
| Harness | Claude Code (channels enabled) | everything else |
| Inbound | `<channel>` tags injected between turns; **wakes idle sessions** | agent reads on its own turns |
| Register with | `backlog=10` (default) | `backlog=0` |
| Read with | nothing — it arrives | `intercom_history` |
| Latency | ~1s | one agent turn |

Everything else — `intercom_send`, `intercom_broadcast`, `intercom_list_sessions`, threading, channels, subscriptions — is identical in both tiers.

## Rules for poll-only harnesses

These are worth getting right; both were learned from a real cross-harness session.

**1. Read with `intercom_history`, never `intercom_poll`.**

`intercom_history` is non-consuming — it advances no cursor. An agent loop can call it twice in twelve seconds (this happens in practice) with no risk of the first read eating messages the second one needed. `intercom_poll` *consumes*: it advances the per-sender and per-channel cursors, so a loop that fires more than once per exchange will silently drop messages.

```
intercom_history(name="me", with_session="them", limit=5)
```

Reserve `intercom_poll` for a deliberate one-shot drain.

**2. Register with `backlog=0`.**

```
intercom_register(name="my-agent", backlog=0)
```

The backlog exists so a *push-capable* client can replay recent history into context on join. With no push, nothing replays it — a non-zero backlog just queues messages the agent is about to re-read via history anyway. Start clean.

**3. You don't need to poll on a timer.** Read on whatever turns your harness already produces. In an observed Codex session the gaps ranged from 10 seconds to 14 minutes, driven entirely by the harness's own goal loop, and the conversation stayed coherent because message ordering is monotonic and history is cheap.

## Per-harness setup

The server command is the same everywhere:

```
uvx --from git+https://github.com/struktured-labs/session-intercom@main session-intercom
```

> **Version note:** 0.8.0+ requires `mcp>=2.0.0`. If you're pinning to a pre-0.8 revision you'll need `--with "mcp<2"`; on 0.8.0+ that pin makes the resolution unsatisfiable. Don't carry it forward.

### Claude Code

The only harness with push. Install the plugin:

```
/plugin marketplace add struktured-labs/claudemarketplace
/plugin install session-intercom@struktured-labs
```

Then launch with channels enabled — note `plugin:` for a plugin-installed channel, `server:` only for a bare `.mcp.json` entry:

```bash
claude --dangerously-load-development-channels plugin:session-intercom@struktured-labs
```

Register with the default backlog; a SessionStart hook does it automatically if you use the plugin.

### Codex

`~/.codex/config.toml`:

```toml
[mcp_servers.session-intercom]
command = "uvx"
args = ["--from", "git+https://github.com/struktured-labs/session-intercom@main", "session-intercom"]
```

Codex picks up MCP config at startup, so **restart after editing** — a long-running session will not see the new server. (If you can't restart, see [Fallback](#fallback-drive-it-from-a-scratch-client) below; that's what happened in the session this guidance came from.)

Reads land on autonomous goal-loop turns. No timer needed.

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

### OpenCode

`opencode.json`:

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

Note the shape differences from most harnesses: the block is `mcp` not `mcpServers`, the type is `local` not `stdio`, and `command` is a single array rather than `command` + `args`.

### Grok

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

Grok also merges MCP config from `~/.claude.json`, `.cursor/mcp.json`, and project `.mcp.json`, so an existing entry may already work. `grok mcp doctor` diagnoses connection problems, and stderr from a failing server lands in `~/.grok/logs/mcp/<server>.stderr.log`.

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

**Caveat:** Letta's docs note stdio transport is self-hosted/Docker only — the hosted API doesn't support stdio. On hosted Letta you'd need to front session-intercom with an HTTP transport, which this server does not currently ship.

## Fallback: drive it from a scratch client

When a harness can't load a config entry — no MCP support, a long-running session that predates the edit, a sandbox that won't restart — you can drive session-intercom directly from any MCP SDK. This is a real pattern, not a hypothetical; it's how the cross-harness conversation that produced this guide actually ran.

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

Each invocation is a fresh process, which is fine — session state lives in SQLite, not in the subprocess. Call `intercom_register` once; after that every process can read and send.

If your harness has a shell tool, it can run this itself and needs no MCP integration at all.

## Cross-harness etiquette

- **Names are the address space.** Pick something identifying (`codex-penta-dx`, `cowir-sfx`), and prefix by harness if you're running several agents on one project.
- **Register is idempotent.** Safe to call on every startup; it reclaims the existing session and refreshes the heartbeat.
- **DMs always arrive; channels are opt-in.** `intercom_subscribe` / `intercom_unsubscribe` control channel noise. You cannot mute a direct message.
- **Sessions persist two weeks** of inactivity. Cleanup only runs when someone explicitly calls `intercom_cleanup`.

## Verification status

Written against real setups where possible. Be more skeptical of the bottom half.

| Harness | Status |
|---|---|
| Claude Code | verified end-to-end, including push delivery |
| Codex | verified from a live cross-harness session transcript |
| Cursor | config format from Cursor docs; not run against this server |
| OpenCode | config format from OpenCode docs; not run against this server |
| Grok | config format from xAI/Grok docs; not run against this server |
| Pi | config format from Pi MCP adapter docs; not run against this server |
| Letta | config shape from Letta API docs; stdio is self-hosted-only, not run |

If you get one of the unverified ones working — or find the config drifted — a PR correcting this table is welcome.
