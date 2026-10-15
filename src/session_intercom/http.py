"""Streamable-HTTP front end for session-intercom (single fixed identity).

For remote agents that cannot spawn a local stdio subprocess (e.g. a cloud
bot). One process == one intercom identity, set via INTERCOM_HTTP_IDENTITY and
auto-registered at startup, so every tool defaults from_name/name to it.
Shares the same SQLite DB as the stdio servers. There is no push channel over
HTTP: the remote side reads its inbox with intercom_poll.

Env:
  INTERCOM_HTTP_IDENTITY  session name to act as (required)
  INTERCOM_HTTP_TOKEN     required bearer token (Authorization: Bearer <tok>)
  INTERCOM_HTTP_HOST      bind host (default 127.0.0.1)
  INTERCOM_HTTP_PORT      bind port (default 8765)
"""

from __future__ import annotations

import contextlib
import hmac
import json
import logging
import os

import uvicorn
from mcp.server.streamable_http_manager import StreamableHTTPSessionManager
from starlette.applications import Starlette
from starlette.responses import JSONResponse
from starlette.routing import Route

from . import db, server

logger = logging.getLogger("session-intercom.http")


async def _seed_channel_cursors(identity: str) -> None:
    """Start intercom_poll at "now" for channel broadcasts this identity has
    never read, instead of replaying the full channel history (thousands of
    messages). Existing cursors and DM cursors are untouched."""
    from .db._common import get_connection

    conn = await get_connection()
    try:
        await conn.execute(
            """
            INSERT OR IGNORE INTO read_cursors (session_id, source, last_read_id)
            SELECT s.id, 'ch:' || m.channel, MAX(m.id)
            FROM sessions s, messages m
            WHERE s.name = ? AND m.channel IS NOT NULL
            GROUP BY m.channel
            """,
            (identity,),
        )
        await conn.commit()
    finally:
        await conn.close()


def build_app(identity: str, token: str):
    manager = StreamableHTTPSessionManager(app=server.server, json_response=True, stateless=True)
    expected = f"Bearer {token}".encode()

    async def mcp_asgi(scope, receive, send):
        headers = dict(scope.get("headers") or [])
        got = headers.get(b"authorization", b"")
        if not hmac.compare_digest(got, expected):
            await JSONResponse({"error": "unauthorized"}, status_code=401)(scope, receive, send)
            return
        await manager.handle_request(scope, receive, send)

    async def health(_request):
        return JSONResponse({"ok": True, "identity": identity})

    @contextlib.asynccontextmanager
    async def lifespan(_app):
        await db.init_db()
        res = await server.call_tool("intercom_register", {"name": identity, "backlog": 0})
        server._locked_identity = identity
        await _seed_channel_cursors(identity)
        logger.info("registered %s: %s", identity, json.loads(res[0].text).get("status"))
        async with manager.run():
            yield

    inner = Starlette(routes=[Route("/health", health)], lifespan=lifespan)

    async def app(scope, receive, send):
        # Serve MCP at /mcp and /mcp/ alike (no 307 slash redirect, which
        # breaks POSTs behind proxies like Tailscale Funnel).
        if scope["type"] == "http" and scope["path"].rstrip("/") == "/mcp":
            await mcp_asgi(scope, receive, send)
        else:
            await inner(scope, receive, send)

    return app


def main() -> None:
    logging.basicConfig(level=logging.INFO)
    identity = os.environ["INTERCOM_HTTP_IDENTITY"]
    token = os.environ.get("INTERCOM_HTTP_TOKEN", "")
    if len(token) < 24:
        raise SystemExit("INTERCOM_HTTP_TOKEN must be set (>=24 chars)")
    uvicorn.run(
        build_app(identity, token),
        host=os.environ.get("INTERCOM_HTTP_HOST", "127.0.0.1"),
        port=int(os.environ.get("INTERCOM_HTTP_PORT", "8765")),
    )


if __name__ == "__main__":
    main()
