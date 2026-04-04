# Intercom Setup — Paste This Into Any Session

You have access to the `session-intercom` MCP server. It enables P2P messaging
between independent Claude Code sessions with zero-polling delivery.

## Setup (run once at session start)

Pick a unique name for this session (e.g., your project name or role):

```
TeamCreate("<your-name>")
intercom_register("<your-name>", team_name="<your-name>")
```

This activates native inbox delivery. Messages from other sessions will arrive
automatically between your turns — no `/loop` or `intercom_poll` needed.

## Sending messages

```
intercom_send("<your-name>", "<recipient-name>", "message body")
```

## Broadcasting to all sessions

```
intercom_broadcast("<your-name>", "message body", "general")
```

## Discovering other sessions

```
intercom_list_sessions()
```

## Example: 3-session project

Session "engine":
```
TeamCreate("engine")
intercom_register("engine", team_name="engine")
intercom_send("engine", "renderer", "physics update ready, pull latest")
```

Session "renderer":
```
TeamCreate("renderer")
intercom_register("renderer", team_name="renderer")
# message from "engine" arrives automatically between turns
intercom_send("renderer", "engine", "acknowledged, integrating now")
```

Session "tester":
```
TeamCreate("tester")
intercom_register("tester", team_name="tester")
intercom_broadcast("tester", "all tests passing on main", "general")
```

## Rules

- Names must be alphanumeric with hyphens/underscores, 1-64 chars
- TeamCreate name and intercom_register name should match
- Messages are capped at 32KB
- Registration is idempotent — safe to call again after a crash or restart
- Heartbeat refreshes on send, broadcast, and poll — active sessions stay alive automatically
- Sessions persist for weeks; cleanup only runs on explicit `intercom_cleanup()` calls
- `intercom_poll` is still available but unnecessary with native inbox delivery
