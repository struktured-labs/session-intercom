"""Public db package facade.

Re-exports the public API so callers can keep using `from .. import db` and
`db.register_session(...)` exactly as before. Internal modules are organized
by concern (sessions / messages / channels / diagnose / cleanup) on top of
the shared connection + schema in `_common`.
"""

from __future__ import annotations

from ._common import (
    CLEANUP_MINUTES,
    DB_DIR,
    DB_PATH,
    MAX_BODY_SIZE,
    NAME_RE,
    STALE_MINUTES,
    get_connection,
    init_db,
    validate_body,
    validate_name,
)
from .channels import create_channel, list_channels
from .cleanup import cleanup_sessions
from .diagnose import diagnose_session
from .messages import (
    broadcast_message,
    get_history,
    poll_messages,
    send_message,
)
from .sessions import (
    get_session_by_name,
    heartbeat,
    list_sessions,
    register_session,
)

__all__ = [
    # constants
    "CLEANUP_MINUTES",
    "DB_DIR",
    "DB_PATH",
    "MAX_BODY_SIZE",
    "NAME_RE",
    "STALE_MINUTES",
    # connection
    "get_connection",
    "init_db",
    "validate_body",
    "validate_name",
    # sessions
    "register_session",
    "heartbeat",
    "list_sessions",
    "get_session_by_name",
    # messages
    "send_message",
    "broadcast_message",
    "poll_messages",
    "get_history",
    # channels
    "list_channels",
    "create_channel",
    # diagnose
    "diagnose_session",
    # cleanup
    "cleanup_sessions",
]
