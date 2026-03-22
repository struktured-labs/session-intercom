"""Bridge configuration — reads from ~/.config/konsolai/bridge.conf."""

from __future__ import annotations

import json
import secrets
from pathlib import Path
from typing import Optional

from pydantic import Field
from pydantic_settings import BaseSettings


_DEFAULT_CONFIG_DIR = Path.home() / ".config" / "konsolai"
_DEFAULT_CONFIG_FILE = _DEFAULT_CONFIG_DIR / "bridge.conf"
_DEFAULT_DATA_DIR = Path.home() / ".local" / "share" / "konsolai"
_DEFAULT_SESSIONS_FILE = _DEFAULT_DATA_DIR / "sessions.json"
_DEFAULT_SOCKET_DIR = Path.home() / ".konsolai" / "sessions"


class BridgeConfig(BaseSettings):
    """Bridge service configuration."""

    # Network
    host: str = Field(default="127.0.0.1", description="Bind address")
    port: int = Field(default=8472, description="Bind port")

    # Authentication
    bearer_token: str = Field(default="", description="Bearer token for API auth")

    # Paths
    sessions_file: Path = Field(default=_DEFAULT_SESSIONS_FILE)
    folders_file: Path = Field(default=_DEFAULT_DATA_DIR / "session_folders.json")
    socket_dir: Path = Field(default=_DEFAULT_SOCKET_DIR)

    # Android Auto / CarPlay
    vehicle_session_limit: int = Field(
        default=5,
        description="Max sessions shown on vehicle displays (safety constraint)",
    )
    voice_command_timeout_s: float = Field(
        default=10.0,
        description="Timeout for voice command acknowledgement",
    )
    tts_max_chars: int = Field(
        default=500,
        description="Max characters for text-to-speech responses",
    )

    model_config = {"env_prefix": "KONSOLAI_BRIDGE_"}

    @classmethod
    def load(cls, path: Optional[Path] = None) -> "BridgeConfig":
        """Load config from JSON file, creating defaults if missing."""
        path = path or _DEFAULT_CONFIG_FILE
        if path.exists():
            data = json.loads(path.read_text())
            return cls(**data)
        # Generate default config with a random token
        cfg = cls(bearer_token=secrets.token_urlsafe(32))
        cfg.save(path)
        return cfg

    def save(self, path: Optional[Path] = None) -> None:
        """Persist current config to disk."""
        path = path or _DEFAULT_CONFIG_FILE
        path.parent.mkdir(parents=True, exist_ok=True)
        data = self.model_dump(mode="json")
        # Convert Path objects to strings for JSON serialization
        for key, val in data.items():
            if isinstance(val, Path):
                data[key] = str(val)
        path.write_text(json.dumps(data, indent=2) + "\n")
        path.chmod(0o600)
