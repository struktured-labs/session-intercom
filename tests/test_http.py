"""Multi-token HTTP front end: token->identity binding, no spoofing, CLI."""

import json
import os

import pytest
from starlette.testclient import TestClient

from session_intercom import db, http, server

H = {"Accept": "application/json, text/event-stream", "Content-Type": "application/json"}
TOK_A = "a" * 32
TOK_B = "b" * 32
LEGACY = "L" * 32


@pytest.fixture(autouse=True)
def reset_state():
    server._current_session = None
    server._locked_identity = None
    yield
    server._current_session = None
    server._locked_identity = None


@pytest.fixture
def tokfile(tmp_path, monkeypatch):
    p = tmp_path / "tokens.json"
    http.save_tokens(p, {TOK_A: "bot-a", TOK_B: "bot-b"})
    monkeypatch.setenv("INTERCOM_HTTP_TOKENS_FILE", str(p))
    return p


def _call(client, token, tool, args, rid=1):
    r = client.post(
        "/mcp",
        headers={**H, "Authorization": f"Bearer {token}"},
        json={
            "jsonrpc": "2.0",
            "id": rid,
            "method": "tools/call",
            "params": {"name": tool, "arguments": args},
        },
    )
    return r


def _result(r):
    assert r.status_code == 200, r.text
    body = r.json()
    return body["result"]["content"][0]["text"]


@pytest.fixture
def client(tokfile):
    store = http.TokenStore(tokfile, {LEGACY: "legacy-bot"})
    with TestClient(http.build_app(store)) as c:
        yield c


def test_unauthorized(client):
    assert _call(client, "nope" * 10, "intercom_unread", {}).status_code == 401
    r = client.post("/mcp", headers=H, json={})
    assert r.status_code == 401


def test_each_token_acts_as_its_identity(client):
    out = json.loads(
        _result(_call(client, TOK_A, "intercom_send", {"to_name": "bot-b", "body": "hi"}))
    )
    assert out["from"] == "bot-a"
    out = json.loads(
        _result(_call(client, LEGACY, "intercom_send", {"to_name": "bot-b", "body": "yo"}))
    )
    assert out["from"] == "legacy-bot"
    text = _result(_call(client, TOK_B, "intercom_poll", {}))
    assert "hi" in text and "yo" in text and "bot-a" in text


def test_cannot_spoof(client):
    t = _result(
        _call(
            client, TOK_A, "intercom_send", {"to_name": "bot-b", "body": "x", "from_name": "bot-b"}
        )
    )
    assert "locked to identity 'bot-a'" in t
    t = _result(_call(client, TOK_A, "intercom_register", {"name": "bot-b"}))
    assert "locked to identity 'bot-a'" in t


def test_hot_reload_add_and_revoke(client, tokfile):
    new = "c" * 32
    assert _call(client, new, "intercom_unread", {}).status_code == 401
    toks = http.load_tokens(tokfile)
    toks[new] = "bot-c"
    http.save_tokens(tokfile, toks)
    os.utime(tokfile, (1, 1))  # force mtime change even within same tick
    assert _call(client, new, "intercom_unread", {}).status_code == 200
    toks.pop(new)
    http.save_tokens(tokfile, toks)
    os.utime(tokfile, (2, 2))
    assert _call(client, new, "intercom_unread", {}).status_code == 401


async def test_existing_identity_keeps_history():
    await db.init_db()
    await db.register_session("old-bot")
    await db.register_session("peer")
    await db.send_message("peer", "old-bot", "before", None)
    await http.ensure_identity("old-bot")
    msgs, _ = await db.poll_messages("old-bot", True, 50, None, include_channels=False)
    assert [m.body for m in msgs] == ["before"]  # reclaimed, unread DM preserved


def test_cli_add_list_revoke(tokfile, capsys):
    assert http._token_cli(["add", "bot-new"]) == 0
    tok = capsys.readouterr().out.strip()
    assert len(tok) >= 24 and http.load_tokens(tokfile)[tok] == "bot-new"
    assert oct(tokfile.stat().st_mode & 0o777) == "0o600"
    http._token_cli(["list"])
    out = capsys.readouterr().out
    assert "bot-new" in out and tok not in out and http.fingerprint(tok) in out
    assert http._token_cli(["revoke", http.fingerprint(tok)]) == 0
    assert tok not in http.load_tokens(tokfile)
    assert http._token_cli(["revoke", "bot-a"]) == 0
    assert "bot-a" not in http.load_tokens(tokfile).values()
    assert http._token_cli(["revoke", "nobody"]) == 1
