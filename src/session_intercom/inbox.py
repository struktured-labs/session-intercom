"""Bridge to Claude Code's native Agent Teams inbox files.

When a session registers with a team_name, intercom will also write messages
to ~/.claude/teams/{team_name}/inboxes/team-lead.json so the CLI's built-in
InboxPoller picks them up without any polling from the session itself.

Each independent session creates its own personal team (via TeamCreate) and
becomes the sole "team-lead". Intercom routes messages to the right inbox file.
"""

from __future__ import annotations

import fcntl
import json
import logging
from datetime import datetime, timezone
from pathlib import Path

logger = logging.getLogger("session-intercom.inbox")

CLAUDE_TEAMS_DIR = Path.home() / ".claude" / "teams"


def _inbox_path(team_name: str) -> Path:
    """Return the inbox file path for a team's lead agent."""
    return CLAUDE_TEAMS_DIR / team_name / "inboxes" / "team-lead.json"


def write_to_inbox(
    team_name: str,
    from_name: str,
    body: str,
    *,
    summary: str | None = None,
    color: str | None = None,
) -> bool:
    """Write a message to a native Claude Code team inbox file.

    Uses flock for safe concurrent access (matches CLI behavior).
    Returns True if written successfully, False on any error.
    """
    inbox = _inbox_path(team_name)

    if not inbox.parent.exists():
        logger.warning("Inbox dir %s does not exist — session may not have created team yet", inbox.parent)
        return False

    timestamp = datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%S.%fZ")

    new_message = {
        "from": from_name,
        "text": body,
        "timestamp": timestamp,
        "read": False,
        "summary": summary,
        "color": color,
    }

    try:
        # Use flock for atomic read-modify-write (matches CLI's locking)
        with open(inbox, "a+") as f:
            fcntl.flock(f, fcntl.LOCK_EX)
            try:
                f.seek(0)
                content = f.read()
                messages = json.loads(content) if content.strip() else []
                messages.append(new_message)
                f.seek(0)
                f.truncate()
                f.write(json.dumps(messages, indent=2))
            finally:
                fcntl.flock(f, fcntl.LOCK_UN)

        logger.info("Wrote inbox message to %s from %s", team_name, from_name)
        return True

    except Exception:
        logger.exception("Failed to write to inbox %s", inbox)
        return False


def inbox_exists(team_name: str) -> bool:
    """Check if a team's inbox directory exists (team was created by CLI)."""
    return _inbox_path(team_name).parent.exists()


def ensure_inbox(team_name: str) -> bool:
    """Create the inboxes dir and empty inbox file if the team config exists but inboxes don't.

    TeamCreate creates config.json but not the inboxes dir — that normally
    happens when the first teammate is spawned. We create it eagerly so the
    InboxPoller has something to watch immediately.

    Returns True if inbox is ready, False if team config doesn't exist.
    """
    team_dir = CLAUDE_TEAMS_DIR / team_name
    config = team_dir / "config.json"
    if not config.exists():
        return False

    inbox = _inbox_path(team_name)
    inbox.parent.mkdir(parents=True, exist_ok=True)
    if not inbox.exists():
        inbox.write_text("[]")
        logger.info("Created inbox file for team %s", team_name)
    return True
