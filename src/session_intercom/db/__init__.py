"""Public db package facade.

Re-exports the public API so callers can keep using `from .. import db` and
`db.register_session(...)` exactly as before. Internal modules are organized
by concern (sessions / messages / channels / cleanup) on top of
the shared connection + schema in `_common`.
"""

from __future__ import annotations

from typing import TYPE_CHECKING

from ._common import (
    get_connection,
    init_db,
    validate_body,
    validate_name,
)

# Static type hints for the constants proxied via __getattr__ — keeps
# pyright/IDEs aware of them without freezing the binding at import time.
if TYPE_CHECKING:
    from ._common import (
        CLEANUP_MINUTES,
        DB_DIR,
        DB_PATH,
        DEFAULT_BACKLOG,
        DEFAULT_CHANNEL,
        MAX_BODY_SIZE,
        MAX_NOTIFICATION_CHARS,
        NAME_RE,
        STALE_MINUTES,
    )
from .channels import create_channel, list_channels
from .cleanup import cleanup_sessions
from .messages import (
    broadcast_message,
    fetch_for_channel_tailer,
    get_history,
    init_tailer_cursor,
    poll_messages,
    send_message,
)
from .sessions import (
    get_session_by_name,
    heartbeat,
    list_sessions,
    register_session,
)
from .subscriptions import (
    list_subscriptions,
    subscribe,
    unsubscribe,
)

# Mutable constants live in _common; proxying them via __getattr__ keeps
# `db.DB_PATH` always in sync with `_common.DB_PATH`. Without this, tests
# that monkeypatch _common.DB_PATH wouldn't affect callers reading db.DB_PATH.
_PROXIED_CONSTANTS = frozenset(
    {
        "CLEANUP_MINUTES",
        "DB_DIR",
        "DB_PATH",
        "DEFAULT_BACKLOG",
        "DEFAULT_CHANNEL",
        "MAX_BODY_SIZE",
        "MAX_NOTIFICATION_CHARS",
        "NAME_RE",
        "STALE_MINUTES",
    }
)


def __getattr__(name: str):
    if name in _PROXIED_CONSTANTS:
        from . import _common

        return getattr(_common, name)
    raise AttributeError(f"module 'session_intercom.db' has no attribute {name!r}")


__all__ = [
    # constants (resolved via __getattr__)
    "CLEANUP_MINUTES",
    "DB_DIR",
    "DB_PATH",
    "DEFAULT_BACKLOG",
    "DEFAULT_CHANNEL",
    "MAX_BODY_SIZE",
    "MAX_NOTIFICATION_CHARS",
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
    "fetch_for_channel_tailer",
    "init_tailer_cursor",
    # subscriptions
    "list_subscriptions",
    "subscribe",
    "unsubscribe",
    # channels
    "list_channels",
    "create_channel",
    # cleanup
    "cleanup_sessions",
]
