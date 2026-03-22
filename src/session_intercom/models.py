from __future__ import annotations

from dataclasses import dataclass, field


@dataclass
class Session:
    id: str
    name: str
    created_at: str
    last_heartbeat: str
    metadata: str | None = None


@dataclass
class Message:
    id: int
    sender_id: str
    sender_name: str
    body: str
    created_at: str
    recipient_id: str | None = None
    recipient_name: str | None = None
    channel: str | None = None
    thread_id: int | None = None


@dataclass
class Channel:
    id: int
    name: str
    created_at: str
    description: str | None = None
