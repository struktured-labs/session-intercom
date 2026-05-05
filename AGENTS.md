# Working on session-intercom (for agents modifying this repo)

This file is for agents (or people) **modifying** session-intercom itself. If you're an agent **using** intercom for inter-session messaging, see [`AGENT_SETUP.md`](./AGENT_SETUP.md) instead.

## What this is

`session-intercom` is an MCP server that gives independent Claude Code sessions a shared SQLite-backed message bus, with a bridge into Claude's native `InboxPoller` for zero-polling delivery. The MCP runs as a stdio subprocess per Claude session and shares state via `~/.local/share/session-intercom/intercom.db`.

## Architecture in one paragraph

`server.py` defines FastMCP tools (the agent-facing surface). It tracks a per-process "current session" so agents don't need to repeat their own name on every call. Tools delegate to the `db/` package — `_common` (connection + schema), `sessions`, `messages`, `channels`, `diagnose`, `cleanup`. The `inbox.py` module is a separate concern: it writes to `~/.claude/teams/<name>/inboxes/team-lead.json` so the CLI's `InboxPoller` picks up messages without the agent calling `intercom_poll`. The MCP can write the file but **cannot** fix a stale in-process `InboxPoller` binding — see the `intercom_diagnose` tool and the `delivery_health` field on `intercom_register` for the recovery flow.

## Dev loop

```bash
uv sync --extra dev          # install deps including pyright
uv run pytest                # 49/49 must pass
uv run pyright               # 0 errors required
```

CI runs both on every PR (matrix: Python 3.11 + 3.12 for tests; 3.11 only for pyright). Branch protection isn't enforced yet, but PRs should land green.

## Constants worth knowing

- `STALE_MINUTES = CLEANUP_MINUTES = 20160` (2 weeks). Sessions are durable. Don't lower these without a strong reason — long-lived agent fleets rely on this.
- `MAX_BODY_SIZE = 32768` — message body cap, enforced server-side.
- DB path: `~/.local/share/session-intercom/intercom.db` (WAL mode, FK on).

## Things to keep in mind

- **Registration is idempotent.** `register_session` reclaims existing rows, refreshing heartbeat and metadata. Don't add "name already taken" errors back.
- **Heartbeat refreshes on every `send`/`broadcast`/`poll`.** Active sessions stay alive automatically; there's no need for the agent to ping `intercom_heartbeat` (which has been removed).
- **Cleanup is explicit only.** It runs only on `intercom_cleanup()`. Resist the urge to add opportunistic cleanup to other tools — it caused production session deletion in the past (see commit `7bd180a` and earlier).
- **`delivery_health` is the source of truth for native delivery state.** Don't reintroduce blanket caveats like "delivery may silently fail" on every register response — the field branches on actual inbox state.
- **The MCP can't fix in-process state.** If the CLI's `InboxPoller` is bound to a stale `leadSessionId`, the recovery requires `TeamDelete` + `TeamCreate` + `intercom_register` from the broken session. No Claude restart needed. See `intercom_diagnose`'s `likely_broken` verdict.
- **Tests share a tmp DB via `tests/conftest.py`** — it monkeypatches `db._common.DB_PATH`. New modules that read DB-related constants should pull them from `db._common`, not from `db` (the package facade re-exports them as fresh bindings).

## Related repos

- **Plugin**: [`struktured-labs/claudemarketplace`](https://github.com/struktured-labs/claudemarketplace) — `plugins/session-intercom/` ships the MCP config + slash command + skill. Bump the plugin's `version` when shipping API changes that the slash command or skill needs to know about.
- **Plugin → MCP fetch**: the plugin's `.mcp.json` runs `uvx --from git+https://github.com/struktured-labs/session-intercom@main session-intercom`. Tagged releases would be safer; `@main` is the current convention.

## Releases

There's no release pipeline yet. Bumps to `pyproject.toml` `version` are documentation; downstream `uvx` installs always fetch from `@main` HEAD.
