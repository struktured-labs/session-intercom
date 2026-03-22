"""Pydantic models for the bridge API — shared between REST, WebSocket, and vehicle protocols."""

from __future__ import annotations

from datetime import datetime
from enum import Enum
from typing import Any, Optional

from pydantic import BaseModel, Field


# ---------------------------------------------------------------------------
# Claude state enum (mirrors ClaudeProcess::State in C++)
# ---------------------------------------------------------------------------

class ClaudeState(str, Enum):
    NOT_RUNNING = "not_running"
    STARTING = "starting"
    IDLE = "idle"
    WORKING = "working"
    WAITING_INPUT = "waiting_input"
    ERROR = "error"


# ---------------------------------------------------------------------------
# Session models
# ---------------------------------------------------------------------------

class TokenUsage(BaseModel):
    input_tokens: int = 0
    output_tokens: int = 0
    cache_read_tokens: int = 0
    cache_creation_tokens: int = 0

    @property
    def total_tokens(self) -> int:
        return (
            self.input_tokens
            + self.output_tokens
            + self.cache_read_tokens
            + self.cache_creation_tokens
        )

    @property
    def estimated_cost_usd(self) -> float:
        return (
            self.input_tokens * 3.0
            + self.output_tokens * 15.0
            + self.cache_creation_tokens * 0.30
            + self.cache_read_tokens * 0.30
        ) / 1_000_000


class YoloSettings(BaseModel):
    yolo: bool = False
    double_yolo: bool = False
    triple_yolo: bool = False


class SessionSummary(BaseModel):
    """Compact session info for list views and vehicle displays."""
    name: str
    session_id: str
    profile: str
    state: ClaudeState = ClaudeState.NOT_RUNNING
    needs_attention: bool = False
    token_usage: TokenUsage = Field(default_factory=TokenUsage)
    yolo: YoloSettings = Field(default_factory=YoloSettings)
    created_at: Optional[datetime] = None
    last_activity: Optional[datetime] = None
    folder_id: Optional[str] = None


class SessionDetail(SessionSummary):
    """Full session info including working directory and model."""
    working_dir: str = ""
    model: str = "default"
    auto_continue_prompt: str = ""
    approval_count: int = 0


class TranscriptMessage(BaseModel):
    """A single message in the conversation transcript."""
    role: str  # "user", "assistant", "system", "tool"
    content: str
    timestamp: Optional[datetime] = None
    tool_name: Optional[str] = None


class Transcript(BaseModel):
    """Parsed conversation transcript."""
    session_name: str
    messages: list[TranscriptMessage] = []
    raw: str = ""


# ---------------------------------------------------------------------------
# Session folder (group) models
# ---------------------------------------------------------------------------

class SessionFolderInfo(BaseModel):
    """A named grouping folder for sessions."""
    folder_id: str
    name: str
    color: str = ""  # hex color string, e.g. "#ff5500"
    created_at: Optional[datetime] = None
    sort_order: int = 0
    member_count: int = 0


class SessionFolderCreate(BaseModel):
    name: str = Field(..., min_length=1, max_length=100)
    color: str = ""


class SessionFolderUpdate(BaseModel):
    name: Optional[str] = None
    color: Optional[str] = None
    sort_order: Optional[int] = None


# ---------------------------------------------------------------------------
# Request / Response models
# ---------------------------------------------------------------------------

class PromptRequest(BaseModel):
    text: str = Field(..., min_length=1, max_length=100_000)


class YoloUpdateRequest(BaseModel):
    yolo: Optional[bool] = None
    double_yolo: Optional[bool] = None
    triple_yolo: Optional[bool] = None


class NewSessionRequest(BaseModel):
    profile: str = "Default"
    working_dir: str = ""
    model: str = "default"


class VoiceCommandRequest(BaseModel):
    """Voice command from Android Auto or CarPlay."""
    text: str = Field(..., min_length=1)
    source: str = Field(default="android_auto", description="android_auto or carplay")
    session_name: Optional[str] = None


class VoiceCommandResponse(BaseModel):
    """Response for voice commands — designed for TTS readback."""
    success: bool
    spoken_response: str = Field(
        ..., max_length=500, description="Short text suitable for TTS playback"
    )
    action_taken: str = ""
    session_name: Optional[str] = None


# ---------------------------------------------------------------------------
# WebSocket event models
# ---------------------------------------------------------------------------

class WSEventType(str, Enum):
    STATE_CHANGED = "state_changed"
    PERMISSION_REQUESTED = "permission_requested"
    TASK_STARTED = "task_started"
    TASK_FINISHED = "task_finished"
    NOTIFICATION = "notification"
    TRANSCRIPT_UPDATE = "transcript_update"
    TOKEN_USAGE_UPDATED = "token_usage_updated"
    SESSION_CREATED = "session_created"
    SESSION_REMOVED = "session_removed"


class WSEvent(BaseModel):
    """WebSocket event envelope."""
    type: WSEventType
    session_name: str
    timestamp: datetime = Field(default_factory=datetime.utcnow)
    data: dict[str, Any] = {}


# ---------------------------------------------------------------------------
# Vehicle display models (Android Auto / CarPlay)
# ---------------------------------------------------------------------------

class VehicleSessionCard(BaseModel):
    """Simplified session info for vehicle head-unit display.

    Constrained for safety: short text, large touch targets,
    limited to essential info.
    """
    name: str = Field(..., max_length=30)
    state: ClaudeState
    state_label: str = Field(..., max_length=20, description="Human-readable state")
    needs_attention: bool = False
    attention_reason: str = Field(default="", max_length=50)
    cost_label: str = Field(default="", max_length=15, description="e.g. '$0.42'")


class VehicleDashboard(BaseModel):
    """Top-level vehicle display data."""
    sessions: list[VehicleSessionCard] = []
    total_active: int = 0
    total_needing_attention: int = 0
    summary_text: str = Field(
        default="", max_length=100, description="Spoken/displayed summary"
    )
