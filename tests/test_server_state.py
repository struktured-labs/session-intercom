"""Smoke tests for the MCP server's per-process session state."""

import json
from pathlib import Path

import pytest

from session_intercom import db, server


@pytest.fixture(autouse=True)
def clear_state():
    """Reset module-level session state between tests."""
    server._current_session = None
    yield
    server._current_session = None


@pytest.fixture
def fake_teams_dir(tmp_path: Path, monkeypatch: pytest.MonkeyPatch):
    """Redirect ~/.claude/teams to a tmp dir for tests that touch inbox files."""
    import session_intercom.inbox as inbox_mod

    monkeypatch.setattr(inbox_mod, "CLAUDE_TEAMS_DIR", tmp_path)
    return tmp_path


def _create_team_dir(fake_teams_dir: Path, team_name: str) -> Path:
    """Simulate TeamCreate: write config.json + create inboxes dir."""
    team_dir = fake_teams_dir / team_name
    (team_dir / "inboxes").mkdir(parents=True, exist_ok=True)
    (team_dir / "config.json").write_text(json.dumps({"name": team_name}))
    return team_dir


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


# --- Register delivery-health detection ---


@pytest.mark.asyncio
async def test_register_no_team_reports_polling_only():
    """Registration without team_name signals polling-only, no false delivery promise."""
    await db.init_db()
    out = await server.intercom_register("alice")
    parsed = json.loads(out)
    assert parsed["status"] == "registered"
    assert parsed["delivery_health"] == "polling_only"
    assert "tip" in parsed


@pytest.mark.asyncio
async def test_register_with_team_no_inbox_signals_no_inbox(fake_teams_dir):
    """When team config doesn't exist, delivery_health is 'no_inbox' with TeamCreate hint."""
    await db.init_db()
    out = await server.intercom_register("alice", team_name="alice")
    parsed = json.loads(out)
    assert parsed["status"] == "registered"
    assert parsed["delivery_health"] == "no_inbox"
    assert parsed["inbox_file_ready"] is False
    assert "TeamCreate" in parsed["next_step"]


@pytest.mark.asyncio
async def test_register_with_clean_team_reports_likely_ok(fake_teams_dir):
    """Fresh team with empty inbox → delivery_health is 'likely_ok'."""
    _create_team_dir(fake_teams_dir, "alice")
    await db.init_db()
    out = await server.intercom_register("alice", team_name="alice")
    parsed = json.loads(out)
    assert parsed["status"] == "registered"
    assert parsed["delivery_health"] == "likely_ok"
    assert parsed["inbox_file_ready"] is True
    # No misleading delivery_caveat — only emit recovery when actually broken.
    assert "delivery_caveat" not in parsed
    assert "recovery" not in parsed


@pytest.mark.asyncio
async def test_register_with_unread_inbox_reports_likely_broken(fake_teams_dir):
    """Inbox with unread messages → 'likely_broken' + actionable recovery steps."""
    _create_team_dir(fake_teams_dir, "alice")
    # Simulate unread messages from a previous conversation that the new
    # session's InboxPoller never picked up.
    inbox_file = fake_teams_dir / "alice" / "inboxes" / "team-lead.json"
    inbox_file.write_text(
        json.dumps(
            [
                {"from": "bob", "text": "hi", "timestamp": "2024-01-01T00:00:00Z", "read": False},
                {
                    "from": "carol",
                    "text": "u up",
                    "timestamp": "2024-01-01T00:01:00Z",
                    "read": False,
                },
            ]
        )
    )

    await db.init_db()
    out = await server.intercom_register("alice", team_name="alice")
    parsed = json.loads(out)
    assert parsed["status"] == "registered"
    assert parsed["delivery_health"] == "likely_broken"
    assert parsed["unread_in_file_inbox"] == 2
    # Recovery steps are concrete and copy-pastable.
    assert "TeamDelete" in parsed["recovery"]
    assert "TeamCreate(team_name='alice')" in parsed["recovery"]
    assert "intercom_register(name='alice', team_name='alice')" in parsed["recovery"]
    assert "intercom_poll" in parsed["recovery"]


# --- leadSessionId binding mismatch detection ---


def _create_team_dir_with_lead(fake_teams_dir: Path, team_name: str, lead_session_id: str) -> Path:
    """Simulate TeamCreate with a specific leadSessionId baked in."""
    team_dir = fake_teams_dir / team_name
    (team_dir / "inboxes").mkdir(parents=True, exist_ok=True)
    (team_dir / "config.json").write_text(
        json.dumps({"name": team_name, "leadSessionId": lead_session_id})
    )
    return team_dir


@pytest.mark.asyncio
async def test_register_detects_stale_lead_session_id(
    fake_teams_dir, monkeypatch: pytest.MonkeyPatch
):
    """Empty inbox + stale leadSessionId still reports likely_broken.

    The original heuristic only fired when unread > 0, missing the common
    fresh-setup case: new conversation reuses an existing team whose
    leadSessionId is still bound to a different (dead or live) conversation.
    """
    _create_team_dir_with_lead(fake_teams_dir, "alice", "old-conversation-sid")
    monkeypatch.setenv("CLAUDE_CODE_SESSION_ID", "this-conversation-sid")

    await db.init_db()
    out = await server.intercom_register("alice", team_name="alice")
    parsed = json.loads(out)

    assert parsed["delivery_health"] == "likely_broken"
    assert parsed["binding_mismatch"] == {
        "team_lead_session_id": "old-conversation-sid",
        "this_session_id": "this-conversation-sid",
    }
    # Inbox is empty so this field should not appear.
    assert "unread_in_file_inbox" not in parsed
    # Recovery still gives the standard TeamDelete → TeamCreate → re-register loop.
    assert "TeamDelete" in parsed["recovery"]
    assert "TeamCreate(team_name='alice')" in parsed["recovery"]


@pytest.mark.asyncio
async def test_register_matching_session_id_reports_likely_ok(
    fake_teams_dir, monkeypatch: pytest.MonkeyPatch
):
    """Matching session IDs → likely_ok, no binding_mismatch flag."""
    _create_team_dir_with_lead(fake_teams_dir, "alice", "same-sid")
    monkeypatch.setenv("CLAUDE_CODE_SESSION_ID", "same-sid")

    await db.init_db()
    out = await server.intercom_register("alice", team_name="alice")
    parsed = json.loads(out)

    assert parsed["delivery_health"] == "likely_ok"
    assert "binding_mismatch" not in parsed
    assert "recovery" not in parsed


@pytest.mark.asyncio
async def test_register_without_env_var_falls_back_to_unread_heuristic(
    fake_teams_dir, monkeypatch: pytest.MonkeyPatch
):
    """Without CLAUDE_CODE_SESSION_ID, mismatch can't be detected; rely on unread count.

    Empty inbox + no env var → likely_ok, since we have no other signal.
    """
    _create_team_dir_with_lead(fake_teams_dir, "alice", "some-sid")
    monkeypatch.delenv("CLAUDE_CODE_SESSION_ID", raising=False)

    await db.init_db()
    out = await server.intercom_register("alice", team_name="alice")
    parsed = json.loads(out)

    assert parsed["delivery_health"] == "likely_ok"
    assert "binding_mismatch" not in parsed


@pytest.mark.asyncio
async def test_register_binding_mismatch_and_unread_both_surface(
    fake_teams_dir, monkeypatch: pytest.MonkeyPatch
):
    """Both signals present → mismatch detail wins in the cause text, unread still reported."""
    _create_team_dir_with_lead(fake_teams_dir, "alice", "old-sid")
    monkeypatch.setenv("CLAUDE_CODE_SESSION_ID", "new-sid")
    inbox_file = fake_teams_dir / "alice" / "inboxes" / "team-lead.json"
    inbox_file.write_text(
        json.dumps(
            [{"from": "bob", "text": "hi", "timestamp": "2024-01-01T00:00:00Z", "read": False}]
        )
    )

    await db.init_db()
    out = await server.intercom_register("alice", team_name="alice")
    parsed = json.loads(out)

    assert parsed["delivery_health"] == "likely_broken"
    assert parsed["unread_in_file_inbox"] == 1
    assert parsed["binding_mismatch"]["team_lead_session_id"] == "old-sid"
    # When both signals fire, the cause text leads with the mismatch (more diagnostic).
    assert "leadSessionId" in parsed["recovery"]
