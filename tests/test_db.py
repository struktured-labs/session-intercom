import pytest

from session_intercom import db


@pytest.mark.asyncio
async def test_init_db():
    await db.init_db()
    assert db.DB_PATH.exists()


@pytest.mark.asyncio
async def test_register_and_list():
    await db.init_db()
    s = await db.register_session("alice")
    assert s.name == "alice"
    assert s.id

    sessions = await db.list_sessions(include_stale=True)
    assert len(sessions) == 1
    assert sessions[0].name == "alice"


@pytest.mark.asyncio
async def test_register_duplicate_reclaims():
    await db.init_db()
    s1 = await db.register_session("alice")
    s2 = await db.register_session("alice")
    assert s1.id == s2.id


@pytest.mark.asyncio
async def test_register_idempotent(monkeypatch):
    await db.init_db()
    s1 = await db.register_session("alice")
    # Re-registering the same name reclaims the session (idempotent)
    s2 = await db.register_session("alice")
    assert s2.id == s1.id
    assert s2.name == "alice"
    # Heartbeat should be refreshed
    assert s2.last_heartbeat >= s1.last_heartbeat


@pytest.mark.asyncio
async def test_invalid_name():
    await db.init_db()
    with pytest.raises(ValueError, match="Invalid session name"):
        await db.register_session("")
    with pytest.raises(ValueError, match="Invalid session name"):
        await db.register_session("has spaces")
    with pytest.raises(ValueError, match="Invalid session name"):
        await db.register_session("-starts-with-hyphen")


@pytest.mark.asyncio
async def test_send_and_poll_dm():
    await db.init_db()
    await db.register_session("alice")
    await db.register_session("bob")

    msg = await db.send_message("alice", "bob", "Hello Bob!")
    assert msg.id
    assert msg.sender_name == "alice"
    assert msg.body == "Hello Bob!"

    # Bob polls and gets the message
    messages, remaining = await db.poll_messages("bob")
    assert len(messages) == 1
    assert messages[0].body == "Hello Bob!"
    assert messages[0].sender_name == "alice"
    assert remaining == 0

    # Poll again — no new messages (cursor advanced)
    messages, _ = await db.poll_messages("bob")
    assert len(messages) == 0


@pytest.mark.asyncio
async def test_broadcast_and_poll():
    await db.init_db()
    await db.register_session("alice")
    await db.register_session("bob")

    msg = await db.broadcast_message("alice", "Hello everyone!", "general")
    assert msg.channel == "general"

    # Bob sees it
    messages, _ = await db.poll_messages("bob")
    assert len(messages) == 1
    assert messages[0].body == "Hello everyone!"
    assert messages[0].channel == "general"

    # Alice doesn't see her own broadcast
    messages, _ = await db.poll_messages("alice")
    assert len(messages) == 0


@pytest.mark.asyncio
async def test_threading():
    await db.init_db()
    await db.register_session("alice")
    await db.register_session("bob")

    root = await db.send_message("alice", "bob", "Start a thread")
    reply = await db.send_message("bob", "alice", "Reply in thread", thread_id=root.id)
    assert reply.thread_id == root.id

    # Get thread via history
    history = await db.get_history("alice", thread_id=root.id)
    assert len(history) == 2
    assert history[0].id == root.id
    assert history[1].id == reply.id


@pytest.mark.asyncio
async def test_heartbeat():
    await db.init_db()
    s = await db.register_session("alice")
    old_hb = s.last_heartbeat

    updated = await db.heartbeat("alice")
    assert updated.last_heartbeat >= old_hb


@pytest.mark.asyncio
async def test_channels():
    await db.init_db()

    # Default channel exists
    channels = await db.list_channels()
    assert any(c.name == "general" for c in channels)

    # Create new channel
    ch = await db.create_channel("rom-hacking", "ROM hacking discussion")
    assert ch.name == "rom-hacking"

    channels = await db.list_channels()
    assert len(channels) == 2


@pytest.mark.asyncio
async def test_history_dm_conversation():
    await db.init_db()
    await db.register_session("alice")
    await db.register_session("bob")

    await db.send_message("alice", "bob", "msg1")
    await db.send_message("bob", "alice", "msg2")
    await db.send_message("alice", "bob", "msg3")

    history = await db.get_history("alice", with_session="bob")
    assert len(history) == 3
    assert [m.body for m in history] == ["msg1", "msg2", "msg3"]


@pytest.mark.asyncio
async def test_history_pagination():
    await db.init_db()
    await db.register_session("alice")
    await db.register_session("bob")

    for i in range(5):
        await db.send_message("alice", "bob", f"msg{i}")

    # Get last 3
    history = await db.get_history("bob", with_session="alice", limit=3)
    assert len(history) == 3
    assert history[-1].body == "msg4"

    # Paginate backwards
    history2 = await db.get_history("bob", with_session="alice", limit=3, before_id=history[0].id)
    assert len(history2) == 2
    assert history2[0].body == "msg0"


@pytest.mark.asyncio
async def test_cleanup():
    await db.init_db()
    s = await db.register_session("stale-session")
    conn = await db.get_connection()
    try:
        await conn.execute(
            "UPDATE sessions SET last_heartbeat = datetime('now', '-21 days') WHERE id = ?",
            (s.id,),
        )
        await conn.commit()
    finally:
        await conn.close()

    removed = await db.cleanup_sessions(ttl_minutes=30)
    assert "stale-session" in removed

    sessions = await db.list_sessions(include_stale=True)
    assert len(sessions) == 0


@pytest.mark.asyncio
async def test_message_body_limit():
    await db.init_db()
    await db.register_session("alice")
    await db.register_session("bob")

    big_body = "x" * 40000
    with pytest.raises(ValueError, match="byte limit"):
        await db.send_message("alice", "bob", big_body)


@pytest.mark.asyncio
async def test_poll_updates_heartbeat():
    await db.init_db()
    s = await db.register_session("alice")
    old_hb = s.last_heartbeat

    await db.poll_messages("alice")

    conn = await db.get_connection()
    try:
        rows = await conn.execute_fetchall("SELECT last_heartbeat FROM sessions WHERE name = 'alice'")
        new_hb = rows[0]["last_heartbeat"]
        assert new_hb >= old_hb
    finally:
        await conn.close()


@pytest.mark.asyncio
async def test_broadcast_nonexistent_channel():
    await db.init_db()
    await db.register_session("alice")
    with pytest.raises(ValueError, match="not found"):
        await db.broadcast_message("alice", "hello", channel="nonexistent")


@pytest.mark.asyncio
async def test_cleanup_with_thread_replies():
    """Regression: cleanup must not FK-fail when stale session's messages have thread replies."""
    await db.init_db()
    alice = await db.register_session("alice")
    await db.register_session("bob")

    # Alice sends a message, Bob replies in a thread
    msg = await db.send_message("alice", "bob", "hello")
    await db.send_message("bob", "alice", "reply", thread_id=msg.id)

    # Make Alice stale
    conn = await db.get_connection()
    try:
        await conn.execute(
            "UPDATE sessions SET last_heartbeat = datetime('now', '-21 days') WHERE id = ?",
            (alice.id,),
        )
        await conn.commit()
    finally:
        await conn.close()

    # Cleanup should NOT raise FK constraint error
    removed = await db.cleanup_sessions(ttl_minutes=30)
    assert "alice" in removed


@pytest.mark.asyncio
async def test_register_idempotent_preserves_messages():
    """Re-registering keeps the same session ID, so messages and threads are preserved."""
    await db.init_db()
    alice = await db.register_session("alice")
    await db.register_session("bob")

    msg = await db.send_message("alice", "bob", "original")
    await db.send_message("bob", "alice", "threaded reply", thread_id=msg.id)

    # Re-register alice — should reclaim, not replace
    new_alice = await db.register_session("alice")
    assert new_alice.name == "alice"
    assert new_alice.id == alice.id

    # Messages should still be intact
    history = await db.get_history("alice", with_session="bob")
    assert len(history) == 2
