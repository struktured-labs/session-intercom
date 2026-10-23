"""Streamable-HTTP front end for session-intercom (multi-token, multi-identity).

For remote agents that cannot spawn a local stdio subprocess (e.g. a cloud
bot). Every bearer token is bound to exactly one intercom identity; requests
act as that identity (from_name/name cannot impersonate another session).
Shares the same SQLite DB as the stdio servers. There is no push channel over
HTTP: the remote side reads its inbox with intercom_poll / intercom_wait.

Tokens come from:
  * a token map file (JSON {token: identity}, owner-only), re-read on change,
    so `token add` / `token revoke` take effect without a restart;
  * optionally the legacy single pair INTERCOM_HTTP_TOKEN + INTERCOM_HTTP_IDENTITY.

Env:
  INTERCOM_HTTP_TOKENS_FILE  token map (default ~/.local/share/session-intercom-http/tokens.json)
  INTERCOM_HTTP_IDENTITY     legacy identity for INTERCOM_HTTP_TOKEN (optional)
  INTERCOM_HTTP_TOKEN        legacy bearer token (optional, >=24 chars)
  INTERCOM_HTTP_HOST         bind host (default 127.0.0.1)
  INTERCOM_HTTP_PORT         bind port (default 8765)

CLI:
  session-intercom-http                      serve
  session-intercom-http token add <identity> mint a token (prints it once)
  session-intercom-http token list           identities + token fingerprints
  session-intercom-http token revoke <identity|token-prefix>
"""

from __future__ import annotations

import argparse
import asyncio
import contextlib
import hashlib
import hmac
import json
import logging
import os
import secrets
import sys
from pathlib import Path

import uvicorn
from mcp.server.streamable_http_manager import StreamableHTTPSessionManager
from starlette.applications import Starlette
from starlette.responses import JSONResponse
from starlette.routing import Route

from . import db, server

logger = logging.getLogger("session-intercom.http")

DEFAULT_TOKENS_FILE = Path.home() / ".local/share/session-intercom-http/tokens.json"
MIN_TOKEN_LEN = 24


def tokens_path() -> Path:
    return Path(os.environ.get("INTERCOM_HTTP_TOKENS_FILE") or DEFAULT_TOKENS_FILE)


def load_tokens(path: Path) -> dict[str, str]:
    try:
        data = json.loads(path.read_text())
    except FileNotFoundError:
        return {}
    if not isinstance(data, dict):
        raise ValueError(f"{path}: expected a JSON object {{token: identity}}")
    return {str(k): str(v) for k, v in data.items() if len(str(k)) >= MIN_TOKEN_LEN}


def save_tokens(path: Path, tokens: dict[str, str]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True, mode=0o700)
    tmp = path.with_suffix(".tmp")
    fd = os.open(tmp, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
    with os.fdopen(fd, "w") as f:
        json.dump(tokens, f, indent=2, sort_keys=True)
    os.chmod(tmp, 0o600)
    os.replace(tmp, path)


def fingerprint(token: str) -> str:
    return hashlib.sha256(token.encode()).hexdigest()[:10]


class TokenStore:
    """Token -> identity map, reloaded when the file's mtime changes."""

    def __init__(self, path: Path, legacy: dict[str, str] | None = None):
        self.path = path
        self.legacy = dict(legacy or {})
        self._mtime: float | None = -1.0
        self._tokens: dict[str, str] = {}

    def _refresh(self) -> None:
        try:
            mtime: float | None = self.path.stat().st_mtime
        except FileNotFoundError:
            mtime = None
        if mtime != self._mtime:
            try:
                self._tokens = load_tokens(self.path)
            except (ValueError, json.JSONDecodeError) as e:
                logger.error("ignoring bad token file: %s", e)
                self._tokens = {}
            self._mtime = mtime

    def all(self) -> dict[str, str]:
        self._refresh()
        return {**self.legacy, **self._tokens}

    def identity_for(self, token: str) -> str | None:
        found = None
        for tok, ident in self.all().items():
            # constant-time compare against every token
            if hmac.compare_digest(tok.encode(), token.encode()):
                found = ident
        return found


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


async def ensure_identity(identity: str) -> str:
    """Register (or reclaim, keeping history/cursors) an identity; new ones
    start reading at "now"."""
    await db.init_db()
    await db.register_session(identity, None, None)
    await db.init_tailer_cursor(identity, 0)
    await _seed_channel_cursors(identity)
    return identity


def _bearer(scope) -> str:
    headers = dict(scope.get("headers") or [])
    got = headers.get(b"authorization", b"").decode("latin-1")
    if got[:7].lower() != "bearer ":
        return ""
    return got[7:].strip()


def build_app(store: TokenStore):
    manager = StreamableHTTPSessionManager(app=server.server, json_response=True, stateless=True)
    ready: set[str] = set()

    async def mcp_asgi(scope, receive, send):
        token = _bearer(scope)
        identity = store.identity_for(token) if len(token) >= MIN_TOKEN_LEN else None
        if identity is None:
            await JSONResponse({"error": "unauthorized"}, status_code=401)(scope, receive, send)
            return
        if identity not in ready:
            await ensure_identity(identity)
            ready.add(identity)
        reset = server._request_identity.set(identity)
        try:
            await manager.handle_request(scope, receive, send)
        finally:
            server._request_identity.reset(reset)

    async def health(_request):
        return JSONResponse({"ok": True, "identities": len(set(store.all().values()))})

    @contextlib.asynccontextmanager
    async def lifespan(_app):
        await db.init_db()
        for ident in sorted(set(store.all().values())):
            await ensure_identity(ident)
            ready.add(ident)
            logger.info("registered %s", ident)
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


def _legacy_tokens() -> dict[str, str]:
    token = os.environ.get("INTERCOM_HTTP_TOKEN", "")
    identity = os.environ.get("INTERCOM_HTTP_IDENTITY", "")
    if token and identity:
        if len(token) < MIN_TOKEN_LEN:
            raise SystemExit(f"INTERCOM_HTTP_TOKEN must be >= {MIN_TOKEN_LEN} chars")
        return {token: identity}
    return {}


def _token_cli(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(prog="session-intercom-http token")
    sub = ap.add_subparsers(dest="cmd", required=True)
    a = sub.add_parser("add", help="mint a token for an identity (new or existing)")
    a.add_argument("identity")
    a.add_argument("--no-register", action="store_true", help="don't touch the DB now")
    sub.add_parser("list")
    r = sub.add_parser("revoke", help="revoke all tokens of an identity, or one by fingerprint")
    r.add_argument("target")
    args = ap.parse_args(argv)
    path = tokens_path()
    tokens = load_tokens(path)

    if args.cmd == "add":
        from .db import validate_name

        validate_name(args.identity)
        tok = secrets.token_urlsafe(32)
        tokens[tok] = args.identity
        save_tokens(path, tokens)
        if not args.no_register:
            asyncio.run(ensure_identity(args.identity))
        print(tok)
        print(f"# identity={args.identity} fingerprint={fingerprint(tok)}", file=sys.stderr)
        return 0
    if args.cmd == "list":
        legacy = _legacy_tokens()
        for tok, ident in sorted({**legacy, **tokens}.items(), key=lambda kv: kv[1]):
            src = "env" if tok in legacy and tok not in tokens else "file"
            print(f"{ident}\t{fingerprint(tok)}\t{src}")
        return 0
    # revoke
    keep = {t: i for t, i in tokens.items() if i != args.target and fingerprint(t) != args.target}
    n = len(tokens) - len(keep)
    if n == 0:
        print(f"no token matched {args.target!r}", file=sys.stderr)
        return 1
    save_tokens(path, keep)
    print(f"revoked {n} token(s)")
    return 0


def main(argv: list[str] | None = None) -> None:
    argv = sys.argv[1:] if argv is None else argv
    if argv and argv[0] == "token":
        raise SystemExit(_token_cli(argv[1:]))
    logging.basicConfig(level=logging.INFO)
    store = TokenStore(tokens_path(), _legacy_tokens())
    if not store.all():
        raise SystemExit("no tokens configured: run `session-intercom-http token add <identity>`")
    uvicorn.run(
        build_app(store),
        host=os.environ.get("INTERCOM_HTTP_HOST", "127.0.0.1"),
        port=int(os.environ.get("INTERCOM_HTTP_PORT", "8765")),
    )


if __name__ == "__main__":
    main()
