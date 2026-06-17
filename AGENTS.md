# Working on session-intercom (for agents modifying this repo)

This file is for agents (or people) **modifying** session-intercom itself. If you're an agent **using** intercom for inter-session messaging, see [`AGENT_SETUP.md`](./AGENT_SETUP.md) instead.

## What this is

`session-intercom` is an MCP server that gives independent Claude Code sessions a shared SQLite-backed message bus. Each session's MCP subprocess pushes inbound messages into its host as `notifications/claude/channel` events (the [Channels API](https://code.claude.com/docs/en/channels-reference)), which Claude Code injects as `<channel>` tags between turns. State is shared across processes via `~/.local/share/session-intercom/intercom.db`.

## Architecture in one paragraph

`server.py` uses the **low-level** `mcp.server.Server` (not FastMCP — its high-level wrapper drops the `experimental` capability needed for channels). It declares `experimental: {"claude/channel": {}}` and registers handlers for `intercom_*` tools via `@server.list_tools()` / `@server.call_tool()`. After `intercom_register`, a background `_tailer_loop` task in the same anyio task group polls the DB once per second for messages addressed to the registered session, formats each into a `notifications/claude/channel` JSON-RPC frame, and writes it directly to the same stdio write stream Claude Code is reading. The `db/` package handles SQLite: `_common` (connection + schema), `sessions`, `messages`, `channels`, `cleanup`.

## Dev loop

```bash
uv sync --extra dev          # install deps including pyright + ruff
uv run pytest                # 32 tests must pass
uv run pyright               # 0 errors required
uv run ruff check . && uv run ruff format --check .
```

CI runs all four gates on every PR (matrix: Python 3.11 + 3.12 for tests; 3.11 only for pyright/ruff).

## Constants worth knowing

- `STALE_MINUTES = CLEANUP_MINUTES = 20160` (2 weeks). Sessions are durable.
- `MAX_BODY_SIZE = 32768` — message body cap.
- DB path: `~/.local/share/session-intercom/intercom.db` (WAL mode, FK on).

## Things to keep in mind

- **Low-level Server is non-negotiable.** FastMCP's high-level wrapper silently drops the `experimental.claude/channel` capability. If you switch back to FastMCP, the CLI never registers a channel listener and delivery silently breaks.
- **The tailer must NOT use the typed `SendNotificationT` path.** The SDK doesn't have a typed model for `notifications/claude/channel`, so we hand-construct `JSONRPCNotification` and write to the stream directly. See `_emit_channel`.
- **`meta` keys must be identifiers.** The CLI silently drops keys with hyphens or other non-alphanum-underscore characters. `_meta_safe` filters this — don't bypass it.
- **Registration is idempotent.** `register_session` reclaims existing rows, refreshing heartbeat and metadata. Don't add "name already taken" errors back.
- **Heartbeat refreshes on every `send` / `broadcast` / `poll`.** Active sessions stay alive automatically.
- **Cleanup is explicit only.** It runs only on `intercom_cleanup()`. Resist the urge to add opportunistic cleanup — it caused production session deletion in the past (see commit `7bd180a` and earlier).
- **The tailer advances read cursors when it emits.** If a notification fails to reach Claude (server crash, transport closed), the message is gone. This is the docs' explicit semantics for channel notifications ("not acknowledged"). Use `intercom_poll` for a re-drainable path if delivery semantics matter for a specific case.
- **Tests share a tmp DB via `tests/conftest.py`** — it monkeypatches `db._common.DB_PATH`. New modules that read DB-related constants should pull them from `db._common`, not from `db` (the package facade re-exports them through `__getattr__`).

## Related repos

- **Plugin**: [`struktured-labs/claudemarketplace`](https://github.com/struktured-labs/claudemarketplace) — `plugins/session-intercom/` ships the MCP config and the skill. Bump the plugin's `version` whenever API or setup shape changes.
- **Plugin → MCP fetch**: the plugin's `.mcp.json` runs `uvx --from git+https://github.com/struktured-labs/session-intercom@main session-intercom`.

## Channels-API research-preview gotchas

- Custom channels require launching Claude Code with `--dangerously-load-development-channels server:session-intercom` (or `plugin:session-intercom@struktured-labs`). The vanilla `--channels` flag only accepts Anthropic-allowlisted plugins right now.
- Confirm the capability is propagating by reading `~/.claude/debug/<session-id>.txt` — a missing capability shows up as `Channel notifications skipped: server did not declare claude/channel capability`.
- Notifications are fire-and-forget at the transport layer. `await write_stream.send(...)` resolves when bytes are written, not when Claude has read them. Don't add ack semantics — just emit.

## Releases

There's no release pipeline yet. Bumps to `pyproject.toml` `version` and `server.py` `__version__` are documentation; downstream `uvx` installs always fetch from `@main` HEAD.
