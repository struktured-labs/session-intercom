# Intercom Setup — Paste This Into Any Session

You have access to the `session-intercom` MCP server. It enables P2P messaging between independent Claude Code sessions over the Channels API — messages arrive between turns as `<channel source="session-intercom" ...>` tags without any polling.

## Setup (one line)

Pick a unique name for this session (e.g., your project name or role):

```
intercom_register(name="<your-name>")
```

That's it. No `TeamCreate`, no `team_name` argument. After register, all other intercom tools default to this name.

The host must have been launched with channels enabled:

```bash
claude --dangerously-load-development-channels server:session-intercom
```

If channel notifications aren't arriving, that flag is the first thing to check.

## Sending messages

```
intercom_send(to_name="<recipient-name>", body="message body")
```

The recipient sees it as `<channel source="session-intercom" from="<your-name>" message_id="42">message body</channel>` on their next turn.

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
intercom_register(name="engine")
intercom_send(to_name="renderer", body="physics update ready, pull latest")
```

Session "renderer":
```
intercom_register(name="renderer")
# message from "engine" arrives automatically between turns as a <channel> tag
intercom_send(to_name="engine", body="acknowledged, integrating now")
```

Session "tester":
```
intercom_register(name="tester")
intercom_broadcast(body="all tests passing on main", channel="general")
```

## Rules

- Names must be alphanumeric with hyphens/underscores, 1-64 chars
- Messages are capped at 32KB
- Registration is idempotent — safe to call again after a crash or restart
- Heartbeat refreshes on send, broadcast, and poll — active sessions stay alive automatically
- Sessions persist for weeks; cleanup only runs on explicit `intercom_cleanup()` calls
- `intercom_poll` is still available but unnecessary — messages arrive as `<channel>` tags automatically
- If channel notifications aren't arriving, verify the host was launched with `--dangerously-load-development-channels server:session-intercom`
