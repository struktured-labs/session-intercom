"""Smoke tests for the MCP server's per-process session state."""

import json

import pytest

from session_intercom import db, server


@pytest.fixture(autouse=True)
def clear_state():
    """Reset module-level session state between tests."""
    server._current_session = None
    yield
    server._current_session = None


def test_resolve_name_explicit_override():
    server._current_session = "alice"
    assert server._resolve_name("bob") == "bob"


def test_resolve_name_uses_state():
    server._current_session = "alice"
    assert server._resolve_name(None) == "alice"


def test_resolve_name_errors_when_no_state():
    with pytest.raises(ValueError, match="No session name available"):
        server._resolve_name(None)


@pytest.mark.asyncio
async def test_register_sets_current_session():
    await db.init_db()
    assert server._current_session is None
    out = await server.intercom_register("alice")
    assert json.loads(out)["status"] == "registered"
    assert server._current_session == "alice"


@pytest.mark.asyncio
async def test_send_uses_registered_name_implicitly():
    await db.init_db()
    await server.intercom_register("alice")
    await server.intercom_register("bob")
    # bob is now current, but we want alice to send → re-register as alice
    await server.intercom_register("alice")
    # No from_name passed; should send as alice (the most recent register)
    out = await server.intercom_send(to_name="bob", body="hi")
    parsed = json.loads(out)
    assert parsed["status"] == "sent"
    assert parsed["from"] == "alice"
    assert parsed["to"] == "bob"


@pytest.mark.asyncio
async def test_send_explicit_from_name_overrides_state():
    await db.init_db()
    await server.intercom_register("alice")
    await server.intercom_register("bob")  # bob is current
    out = await server.intercom_send(to_name="alice", body="ping", from_name="bob")
    parsed = json.loads(out)
    assert parsed["from"] == "bob"


@pytest.mark.asyncio
async def test_send_without_register_returns_helpful_error():
    await db.init_db()
    out = await server.intercom_send(to_name="someone", body="hi")
    parsed = json.loads(out)
    assert "error" in parsed
    assert "No session name available" in parsed["error"]


@pytest.mark.asyncio
async def test_poll_uses_registered_name_implicitly():
    await db.init_db()
    await server.intercom_register("alice")
    await server.intercom_register("bob")
    await server.intercom_register("alice")  # alice current
    await server.intercom_send(to_name="alice", body="msg", from_name="bob")
    out = await server.intercom_poll()
    parsed = json.loads(out)
    assert parsed["count"] == 1
    assert parsed["messages"][0]["body"] == "msg"


@pytest.mark.asyncio
async def test_diagnose_uses_registered_name_implicitly():
    await db.init_db()
    await server.intercom_register("alice")
    out = await server.intercom_diagnose()
    parsed = json.loads(out)
    assert parsed["session_name"] == "alice"
    assert "verdict" in parsed


@pytest.mark.asyncio
async def test_cleanup_default_uses_constant():
    """Default ttl should match the durability story (2 weeks), not 30 minutes."""
    await db.init_db()
    out = await server.intercom_cleanup()
    parsed = json.loads(out)
    assert parsed["ttl_minutes"] == db.CLEANUP_MINUTES
    assert parsed["ttl_minutes"] >= 20000  # ~2 weeks
