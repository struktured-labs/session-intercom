"""Tests for native Claude Code inbox bridge."""

import json
from pathlib import Path

import pytest

from session_intercom import db
from session_intercom.inbox import inbox_exists, write_to_inbox


@pytest.fixture
def teams_dir(tmp_path: Path, monkeypatch: pytest.MonkeyPatch):
    """Create a fake ~/.claude/teams/ structure."""
    import session_intercom.inbox as inbox_mod

    monkeypatch.setattr(inbox_mod, "CLAUDE_TEAMS_DIR", tmp_path)
    return tmp_path


def _create_team(teams_dir: Path, team_name: str) -> Path:
    """Simulate what TeamCreate does: create the team dir + inboxes dir."""
    inbox_dir = teams_dir / team_name / "inboxes"
    inbox_dir.mkdir(parents=True)
    # Write minimal team config
    config = teams_dir / team_name / "config.json"
    config.write_text(json.dumps({
        "name": team_name,
        "members": [{"name": "team-lead", "agentId": f"team-lead@{team_name}"}],
    }))
    return inbox_dir


def test_inbox_exists_false(teams_dir):
    assert not inbox_exists("nonexistent")


def test_inbox_exists_true(teams_dir):
    _create_team(teams_dir, "my-session")
    assert inbox_exists("my-session")


def test_write_to_inbox_creates_message(teams_dir):
    _create_team(teams_dir, "bob")

    result = write_to_inbox("bob", "alice", "Hello from intercom!")
    assert result is True

    inbox_file = teams_dir / "bob" / "inboxes" / "team-lead.json"
    assert inbox_file.exists()

    messages = json.loads(inbox_file.read_text())
    assert len(messages) == 1
    assert messages[0]["from"] == "alice"
    assert messages[0]["text"] == "Hello from intercom!"
    assert messages[0]["read"] is False
    assert messages[0]["timestamp"]


def test_write_to_inbox_appends(teams_dir):
    _create_team(teams_dir, "bob")

    write_to_inbox("bob", "alice", "First")
    write_to_inbox("bob", "charlie", "Second")

    inbox_file = teams_dir / "bob" / "inboxes" / "team-lead.json"
    messages = json.loads(inbox_file.read_text())
    assert len(messages) == 2
    assert messages[0]["from"] == "alice"
    assert messages[1]["from"] == "charlie"


def test_write_to_inbox_no_team_dir(teams_dir):
    result = write_to_inbox("nonexistent", "alice", "Hello")
    assert result is False


def test_write_with_summary_and_color(teams_dir):
    _create_team(teams_dir, "bob")

    write_to_inbox("bob", "alice", "Important!", summary="urgent DM", color="red")

    inbox_file = teams_dir / "bob" / "inboxes" / "team-lead.json"
    messages = json.loads(inbox_file.read_text())
    assert messages[0]["summary"] == "urgent DM"
    assert messages[0]["color"] == "red"


@pytest.mark.asyncio
async def test_send_message_bridges_to_inbox(tmp_path, monkeypatch, teams_dir):
    """Integration: intercom_send writes to both SQLite and native inbox."""
    monkeypatch.setattr(db, "DB_DIR", tmp_path)
    monkeypatch.setattr(db, "DB_PATH", tmp_path / "test.db")
    await db.init_db()

    # Create team for bob (simulating TeamCreate)
    _create_team(teams_dir, "bob-team")

    # Register sessions — bob has a team_name
    await db.register_session("alice")
    await db.register_session("bob", team_name="bob-team")

    # Send message
    msg = await db.send_message("alice", "bob", "Hello via intercom!")
    assert msg.id

    # Verify native inbox got the message
    inbox_file = teams_dir / "bob-team" / "inboxes" / "team-lead.json"
    assert inbox_file.exists()
    messages = json.loads(inbox_file.read_text())
    assert len(messages) == 1
    assert messages[0]["from"] == "alice"
    assert messages[0]["text"] == "Hello via intercom!"
    assert messages[0]["read"] is False


@pytest.mark.asyncio
async def test_send_no_bridge_without_team(tmp_path, monkeypatch, teams_dir):
    """Messages to sessions without team_name should NOT write to inbox."""
    monkeypatch.setattr(db, "DB_DIR", tmp_path)
    monkeypatch.setattr(db, "DB_PATH", tmp_path / "test.db")
    await db.init_db()

    await db.register_session("alice")
    await db.register_session("bob")  # No team_name

    await db.send_message("alice", "bob", "Hello!")

    # No inbox dir should exist
    inbox_file = teams_dir / "bob" / "inboxes" / "team-lead.json"
    assert not inbox_file.exists()


@pytest.mark.asyncio
async def test_broadcast_bridges_to_all_team_inboxes(tmp_path, monkeypatch, teams_dir):
    """Broadcast should write to all sessions with team_names except sender."""
    monkeypatch.setattr(db, "DB_DIR", tmp_path)
    monkeypatch.setattr(db, "DB_PATH", tmp_path / "test.db")
    await db.init_db()

    _create_team(teams_dir, "alice-team")
    _create_team(teams_dir, "bob-team")
    _create_team(teams_dir, "charlie-team")

    await db.register_session("alice", team_name="alice-team")
    await db.register_session("bob", team_name="bob-team")
    await db.register_session("charlie", team_name="charlie-team")

    await db.broadcast_message("alice", "Attention all!", "general")

    # Bob and Charlie should get inbox messages, Alice should not
    bob_inbox = teams_dir / "bob-team" / "inboxes" / "team-lead.json"
    charlie_inbox = teams_dir / "charlie-team" / "inboxes" / "team-lead.json"
    alice_inbox = teams_dir / "alice-team" / "inboxes" / "team-lead.json"

    assert bob_inbox.exists()
    assert charlie_inbox.exists()
    assert not alice_inbox.exists()

    bob_msgs = json.loads(bob_inbox.read_text())
    assert len(bob_msgs) == 1
    assert bob_msgs[0]["from"] == "alice"
    assert bob_msgs[0]["text"] == "Attention all!"


@pytest.mark.asyncio
async def test_register_with_team_name(tmp_path, monkeypatch):
    """team_name is stored and returned in session data."""
    monkeypatch.setattr(db, "DB_DIR", tmp_path)
    monkeypatch.setattr(db, "DB_PATH", tmp_path / "test.db")
    await db.init_db()

    session = await db.register_session("alice", team_name="alice-team")
    assert session.team_name == "alice-team"

    sessions = await db.list_sessions(include_stale=True)
    assert sessions[0].team_name == "alice-team"
