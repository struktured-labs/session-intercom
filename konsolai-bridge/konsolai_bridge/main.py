"""Konsolai Bridge — FastAPI application entry point."""

from __future__ import annotations

import argparse
import logging
from contextlib import asynccontextmanager
from pathlib import Path
from typing import Any

from fastapi import FastAPI

from .config import BridgeConfig
from .hook_listener import HookListener
from .models import ClaudeState, WSEventType
from .routes import groups, sessions, setup, vehicle, websocket
from .session_registry import SessionRegistry
from .tmux import TmuxManager

logger = logging.getLogger("konsolai_bridge")


def _make_hook_handler(app: FastAPI):
    """Create a hook event handler closed over a specific app instance.

    This avoids a module-level ``_app`` global.  The returned coroutine
    resolves the short 8-hex session ID to the full session name via the
    registry before updating state or emitting WebSocket events.
    """

    async def hook_event_handler(
        session_id: str,
        event: dict[str, Any],
    ) -> None:
        event_type = event.get("type", "")
        registry: SessionRegistry = app.state.registry

        # Resolve short hex id -> full session name
        session_name = await registry.resolve_session_id(session_id)
        if not session_name:
            session_name = session_id  # fallback: raw id

        if event_type == "Stop":
            registry.update_state(session_name, ClaudeState.IDLE)
            await websocket.emit_event(
                WSEventType.STATE_CHANGED,
                session_name,
                {"state": "idle"},
            )

        elif event_type == "PreToolUse":
            tool = event.get("tool", "")
            registry.update_state(session_name, ClaudeState.WORKING)
            await websocket.emit_event(
                WSEventType.STATE_CHANGED,
                session_name,
                {"state": "working", "tool": tool},
            )

        elif event_type == "PostToolUse":
            tool = event.get("tool", "")
            await websocket.emit_event(
                WSEventType.STATE_CHANGED,
                session_name,
                {"state": "working", "tool": tool, "phase": "post"},
            )

        elif event_type == "Notification":
            message = event.get("message", "")
            await websocket.emit_event(
                WSEventType.NOTIFICATION,
                session_name,
                {"message": message},
            )

        # Generic forwarding for any event type
        await websocket.emit_event(
            WSEventType.NOTIFICATION,
            session_name,
            {"hook_event": event_type, "raw": event},
        )

    return hook_event_handler


@asynccontextmanager
async def lifespan(app: FastAPI):
    """Application lifespan — start/stop background services."""
    config: BridgeConfig = app.state.config
    tmux = TmuxManager()
    registry = SessionRegistry(config.sessions_file, config.socket_dir, tmux, config.folders_file)
    handler = _make_hook_handler(app)
    hook_listener = HookListener(config.socket_dir, handler)

    app.state.tmux = tmux
    app.state.registry = registry
    app.state.hook_listener = hook_listener

    await hook_listener.start()
    logger.info(
        "Konsolai Bridge started on %s:%d",
        config.host,
        config.port,
    )
    yield
    await hook_listener.stop()
    logger.info("Konsolai Bridge stopped")


def create_app(config: BridgeConfig | None = None) -> FastAPI:
    """Create and configure the FastAPI application."""
    if config is None:
        config = BridgeConfig.load()

    app = FastAPI(
        title="Konsolai Bridge",
        description="REST/WebSocket bridge for remote Konsolai control — Android Auto & CarPlay",
        version="0.1.0",
        lifespan=lifespan,
    )
    app.state.config = config

    # Mount route modules
    app.include_router(sessions.router)
    app.include_router(groups.router)
    app.include_router(websocket.router)
    app.include_router(vehicle.router)
    app.include_router(setup.router)

    return app


def cli_main() -> None:
    """CLI entry point — parse args and run the server."""
    parser = argparse.ArgumentParser(description="Konsolai Bridge Service")
    parser.add_argument("--host", default=None, help="Bind address")
    parser.add_argument("--port", type=int, default=None, help="Bind port")
    parser.add_argument("--config", type=Path, default=None, help="Config file path")
    parser.add_argument("--log-level", default="INFO", help="Log level")
    args = parser.parse_args()

    logging.basicConfig(
        level=getattr(logging, args.log_level.upper(), logging.INFO),
        format="%(asctime)s %(name)s %(levelname)s %(message)s",
    )

    config = BridgeConfig.load(args.config)
    if args.host:
        config.host = args.host
    if args.port:
        config.port = args.port

    app = create_app(config)

    import uvicorn
    uvicorn.run(
        app,
        host=config.host,
        port=config.port,
        log_level=args.log_level.lower(),
    )


if __name__ == "__main__":
    cli_main()
