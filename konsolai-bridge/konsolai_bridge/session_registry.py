"""Session registry — reads Konsolai session state from disk and tmux.

Mirrors ClaudeSessionRegistry from the C++ side.  Reads:
  - ~/.local/share/konsolai/sessions.json   (persisted session metadata)
  - tmux list-sessions                       (live sessions)
  - ~/.konsolai/sessions/*.sock              (hook sockets)
"""

from __future__ import annotations

import json
import logging
import uuid
from datetime import datetime, timezone
from pathlib import Path
from typing import Optional

from .models import (
    ClaudeState,
    SessionDetail,
    SessionFolderInfo,
    SessionSummary,
    TokenUsage,
    YoloSettings,
)
from .tmux import TmuxManager, TmuxSessionInfo

logger = logging.getLogger(__name__)


def _parse_state(raw: str) -> ClaudeState:
    """Convert a C++ state string to our enum."""
    mapping = {
        "NotRunning": ClaudeState.NOT_RUNNING,
        "Starting": ClaudeState.STARTING,
        "Idle": ClaudeState.IDLE,
        "Working": ClaudeState.WORKING,
        "WaitingInput": ClaudeState.WAITING_INPUT,
        "WaitingForInput": ClaudeState.WAITING_INPUT,
        "Error": ClaudeState.ERROR,
    }
    return mapping.get(raw, ClaudeState.NOT_RUNNING)


class SessionRegistry:
    """Aggregates session info from multiple sources."""

    def __init__(
        self,
        sessions_file: Path,
        socket_dir: Path,
        tmux: TmuxManager,
        folders_file: Path | None = None,
    ) -> None:
        self._sessions_file = sessions_file
        self._socket_dir = socket_dir
        self._tmux = tmux
        self._folders_file = folders_file or sessions_file.parent / "session_folders.json"

        # In-memory state overlay (updated by hook events)
        self._state_cache: dict[str, ClaudeState] = {}

    def update_state(self, session_name: str, state: ClaudeState) -> None:
        """Update cached state for a session (called from hook events)."""
        self._state_cache[session_name] = state

    async def resolve_session_id(self, session_id: str) -> str | None:
        """Resolve a short 8-hex session ID to the full session name.

        Searches live tmux sessions and persisted data.  Returns None if
        no match is found.
        """
        # Check state cache first (fast path)
        for name in self._state_cache:
            if name.endswith(f"-{session_id}"):
                return name
        # Check live tmux sessions
        live = await self._tmux.list_sessions()
        for info in live:
            if info.session_id == session_id:
                return info.name
        # Check persisted data
        persisted = self._read_persisted()
        for name, meta in persisted.items():
            if meta.get("sessionId") == session_id or name.endswith(f"-{session_id}"):
                return name
        return None

    async def list_sessions(self) -> list[SessionSummary]:
        """Return summaries of all known sessions."""
        persisted = self._read_persisted()
        live = await self._tmux.list_sessions()
        live_names = {s.name for s in live}

        results: list[SessionSummary] = []
        # Merge persisted data with live tmux sessions
        seen: set[str] = set()
        for info in live:
            seen.add(info.name)
            meta = persisted.get(info.name, {})
            state = self._state_cache.get(info.name, _parse_state(meta.get("state", "")))
            needs_attention = state in (ClaudeState.WAITING_INPUT, ClaudeState.ERROR)
            results.append(SessionSummary(
                name=info.name,
                session_id=info.session_id,
                profile=info.profile,
                state=state,
                needs_attention=needs_attention,
                token_usage=self._extract_tokens(meta),
                yolo=self._extract_yolo(meta),
                created_at=datetime.fromtimestamp(info.created, tz=timezone.utc) if info.created else None,
                last_activity=self._parse_dt(meta.get("lastActivity")),
                folder_id=meta.get("folderId"),
            ))
        # Include persisted sessions not currently in tmux (detached/dead)
        for name, meta in persisted.items():
            if name not in seen:
                results.append(SessionSummary(
                    name=name,
                    session_id=meta.get("sessionId", ""),
                    profile=meta.get("profile", ""),
                    state=ClaudeState.NOT_RUNNING,
                    needs_attention=False,
                    token_usage=self._extract_tokens(meta),
                    yolo=self._extract_yolo(meta),
                    created_at=self._parse_dt(meta.get("createdAt")),
                    last_activity=self._parse_dt(meta.get("lastActivity")),
                    folder_id=meta.get("folderId"),
                ))
        # Sort: needs-attention first, then by last activity
        results.sort(key=lambda s: (not s.needs_attention, s.last_activity or datetime.min.replace(tzinfo=timezone.utc)), reverse=False)
        return results

    async def get_session(self, name: str) -> Optional[SessionDetail]:
        """Return full detail for a single session."""
        persisted = self._read_persisted()
        meta = persisted.get(name, {})
        exists = await self._tmux.session_exists(name)
        if not exists and not meta:
            return None
        state = self._state_cache.get(name, _parse_state(meta.get("state", "")))
        if not exists:
            state = ClaudeState.NOT_RUNNING

        # Parse session name for profile/id
        profile = meta.get("profile", "")
        session_id = meta.get("sessionId", "")
        if not profile and "-" in name:
            parts = name.split("-")
            if len(parts) >= 3:
                profile = "-".join(parts[1:-1])
                session_id = parts[-1]

        return SessionDetail(
            name=name,
            session_id=session_id,
            profile=profile,
            state=state,
            needs_attention=state in (ClaudeState.WAITING_INPUT, ClaudeState.ERROR),
            token_usage=self._extract_tokens(meta),
            yolo=self._extract_yolo(meta),
            created_at=self._parse_dt(meta.get("createdAt")),
            last_activity=self._parse_dt(meta.get("lastActivity")),
            working_dir=meta.get("workingDir", ""),
            model=meta.get("model", "default"),
            auto_continue_prompt=meta.get("autoContinuePrompt", ""),
            approval_count=meta.get("approvalCount", 0),
            folder_id=meta.get("folderId"),
        )

    # ------------------------------------------------------------------
    # Session folders (groups)
    # ------------------------------------------------------------------

    def _read_folders(self) -> list[dict]:
        """Read the session_folders.json file."""
        if not self._folders_file.exists():
            return []
        try:
            data = json.loads(self._folders_file.read_text())
            if isinstance(data, list):
                return data
        except Exception:
            logger.warning("Failed to read session_folders.json", exc_info=True)
        return []

    def _write_folders(self, folders: list[dict]) -> None:
        """Write the session_folders.json file."""
        self._folders_file.parent.mkdir(parents=True, exist_ok=True)
        self._folders_file.write_text(json.dumps(folders, indent=2, default=str) + "\n")

    def _count_members(self, folder_id: str) -> int:
        """Count how many sessions belong to a folder."""
        persisted = self._read_persisted()
        return sum(1 for meta in persisted.values() if meta.get("folderId") == folder_id)

    def list_folders(self) -> list[SessionFolderInfo]:
        """Return all session folders."""
        raw = self._read_folders()
        results: list[SessionFolderInfo] = []
        for f in raw:
            fid = f.get("folderId", "")
            results.append(SessionFolderInfo(
                folder_id=fid,
                name=f.get("name", ""),
                color=f.get("color", ""),
                created_at=self._parse_dt(f.get("createdAt")),
                sort_order=f.get("sortOrder", 0),
                member_count=self._count_members(fid),
            ))
        results.sort(key=lambda f: f.sort_order)
        return results

    def create_folder(self, name: str, color: str = "") -> SessionFolderInfo:
        """Create a new session folder."""
        folders = self._read_folders()
        folder_id = uuid.uuid4().hex[:12]
        now = datetime.now(tz=timezone.utc)
        max_order = max((f.get("sortOrder", 0) for f in folders), default=0)
        entry = {
            "folderId": folder_id,
            "name": name,
            "color": color,
            "createdAt": now.isoformat(),
            "sortOrder": max_order + 1,
        }
        folders.append(entry)
        self._write_folders(folders)
        return SessionFolderInfo(
            folder_id=folder_id,
            name=name,
            color=color,
            created_at=now,
            sort_order=max_order + 1,
            member_count=0,
        )

    def update_folder(
        self,
        folder_id: str,
        name: str | None = None,
        color: str | None = None,
        sort_order: int | None = None,
    ) -> SessionFolderInfo | None:
        """Update an existing session folder. Returns None if not found."""
        folders = self._read_folders()
        for f in folders:
            if f.get("folderId") == folder_id:
                if name is not None:
                    f["name"] = name
                if color is not None:
                    f["color"] = color
                if sort_order is not None:
                    f["sortOrder"] = sort_order
                self._write_folders(folders)
                return SessionFolderInfo(
                    folder_id=folder_id,
                    name=f.get("name", ""),
                    color=f.get("color", ""),
                    created_at=self._parse_dt(f.get("createdAt")),
                    sort_order=f.get("sortOrder", 0),
                    member_count=self._count_members(folder_id),
                )
        return None

    def delete_folder(self, folder_id: str) -> bool:
        """Delete a folder and clear folderId from all its member sessions.

        Returns True if the folder was found and deleted.
        """
        folders = self._read_folders()
        new_folders = [f for f in folders if f.get("folderId") != folder_id]
        if len(new_folders) == len(folders):
            return False
        self._write_folders(new_folders)
        # Clear folderId from sessions
        self._clear_folder_from_sessions(folder_id)
        return True

    def move_session_to_folder(self, session_id: str, folder_id: str) -> bool:
        """Assign a session to a folder. Returns False if session not found."""
        return self._set_session_folder(session_id, folder_id)

    def remove_session_from_folder(self, session_id: str) -> bool:
        """Remove a session from its folder. Returns False if session not found."""
        return self._set_session_folder(session_id, None)

    def _set_session_folder(self, session_name: str, folder_id: str | None) -> bool:
        """Set or clear the folderId for a session in sessions.json."""
        if not self._sessions_file.exists():
            return False
        try:
            raw = self._sessions_file.read_text()
            data = json.loads(raw)
        except Exception:
            return False

        found = False
        if isinstance(data, list):
            for entry in data:
                if entry.get("name") == session_name:
                    if folder_id is not None:
                        entry["folderId"] = folder_id
                    else:
                        entry.pop("folderId", None)
                    found = True
                    break
        elif isinstance(data, dict):
            if session_name in data:
                if folder_id is not None:
                    data[session_name]["folderId"] = folder_id
                else:
                    data[session_name].pop("folderId", None)
                found = True

        if found:
            self._sessions_file.write_text(json.dumps(data, indent=2, default=str) + "\n")
        return found

    def _clear_folder_from_sessions(self, folder_id: str) -> None:
        """Remove a specific folderId from all sessions."""
        if not self._sessions_file.exists():
            return
        try:
            raw = self._sessions_file.read_text()
            data = json.loads(raw)
        except Exception:
            return

        changed = False
        if isinstance(data, list):
            for entry in data:
                if entry.get("folderId") == folder_id:
                    entry.pop("folderId", None)
                    changed = True
        elif isinstance(data, dict):
            for meta in data.values():
                if isinstance(meta, dict) and meta.get("folderId") == folder_id:
                    meta.pop("folderId", None)
                    changed = True

        if changed:
            self._sessions_file.write_text(json.dumps(data, indent=2, default=str) + "\n")

    # ------------------------------------------------------------------
    # Internal helpers
    # ------------------------------------------------------------------

    def _read_persisted(self) -> dict[str, dict]:
        """Read the sessions.json file."""
        if not self._sessions_file.exists():
            return {}
        try:
            data = json.loads(self._sessions_file.read_text())
            if isinstance(data, list):
                return {s["name"]: s for s in data if "name" in s}
            if isinstance(data, dict):
                return data
        except Exception:
            logger.warning("Failed to read sessions.json", exc_info=True)
        return {}

    @staticmethod
    def _extract_tokens(meta: dict) -> TokenUsage:
        tok = meta.get("tokenUsage", {})
        return TokenUsage(
            input_tokens=tok.get("inputTokens", 0),
            output_tokens=tok.get("outputTokens", 0),
            cache_read_tokens=tok.get("cacheReadTokens", 0),
            cache_creation_tokens=tok.get("cacheCreationTokens", 0),
        )

    @staticmethod
    def _extract_yolo(meta: dict) -> YoloSettings:
        return YoloSettings(
            yolo=meta.get("yoloMode", False),
            double_yolo=meta.get("doubleYoloMode", False),
            triple_yolo=meta.get("tripleYoloMode", False),
        )

    @staticmethod
    def _parse_dt(val: Optional[str]) -> Optional[datetime]:
        if not val:
            return None
        try:
            return datetime.fromisoformat(val)
        except (ValueError, TypeError):
            return None
