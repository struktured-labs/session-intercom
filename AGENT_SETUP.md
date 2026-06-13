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

After registering, your own name is implicit — you don't pass `from_name`:

```
intercom_send(to_name="<recipient-name>", body="message body")
```

## Broadcasting to all sessions

```
intercom_broadcast(body="message body", channel="general")
```

## Discovering other sessions

```
intercom_list_sessions()
```

## Example: 3-session project

Session "engine":
```
TeamCreate(team_name="engine")
intercom_register(name="engine", team_name="engine")
intercom_send(to_name="renderer", body="physics update ready, pull latest")
```

Session "renderer":
```
TeamCreate(team_name="renderer")
intercom_register(name="renderer", team_name="renderer")
# message from "engine" arrives automatically between turns
intercom_send(to_name="engine", body="acknowledged, integrating now")
```

Session "tester":
```
TeamCreate(team_name="tester")
intercom_register(name="tester", team_name="tester")
intercom_broadcast(body="all tests passing on main", channel="general")
```

## Rules

- Names must be alphanumeric with hyphens/underscores, 1-64 chars
- TeamCreate name and intercom_register name should match
- Messages are capped at 32KB
- Registration is idempotent — safe to call again after a crash or restart
- Heartbeat refreshes on send, broadcast, and poll — active sessions stay alive automatically
- Sessions persist for weeks; cleanup only runs on explicit `intercom_cleanup()` calls
- `intercom_poll` is still available but unnecessary with native inbox delivery
- `intercom_register` reports a `delivery_health` field — `likely_ok`, `likely_broken`, `polling_only`, or `no_inbox`. `likely_broken` fires in two cases: (1) the team config's `leadSessionId` doesn't match this conversation's `CLAUDE_CODE_SESSION_ID` (the response includes a `binding_mismatch` field), or (2) unread messages are sitting in the file inbox (the response includes `unread_in_file_inbox`). Either way the response includes copy-pastable recovery steps (TeamDelete → TeamCreate → re-register). No Claude restart needed.
- If you skipped registering with a `team_name` and want to add native delivery later, just re-register with `team_name` set — registration is idempotent.
