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
        rows = await conn.execute_fetchall(
            "SELECT last_heartbeat FROM sessions WHERE name = 'alice'"
        )
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


# --- Channel-tailer cursor split (0.6.1 regression guard) ---


@pytest.mark.asyncio
async def test_tailer_cursor_independent_from_poll_cursor():
    """fetch_for_channel_tailer must NOT advance the per-sender cursor that
    poll_messages uses. If channel delivery silently fails, poll must still
    work as a recovery path."""
    await db.init_db()
    await db.register_session("alice")
    await db.register_session("bob")
    await db.send_message("alice", "bob", "hello via channel + poll")

    # Tailer drains its own cursor — simulating "tailer emitted, channel
    # notification may or may not have actually landed in Claude's context".
    tailed = await db.fetch_for_channel_tailer("bob")
    assert len(tailed) == 1
    assert tailed[0].body == "hello via channel + poll"

    # Tailer cursor is now past msg 1, but the per-sender cursor used by
    # intercom_poll is untouched. So poll still finds the message.
    polled, _ = await db.poll_messages("bob")
    assert len(polled) == 1, (
        "poll must still see the message — its cursor was not advanced by the tailer"
    )
    assert polled[0].body == "hello via channel + poll"

    # After explicit poll(mark_read=True), poll is drained.
    polled2, _ = await db.poll_messages("bob")
    assert polled2 == []

    # Tailer is ALSO not seeing it again — its cursor was advanced during the
    # tailer fetch above.
    tailed2 = await db.fetch_for_channel_tailer("bob")
    assert tailed2 == []


@pytest.mark.asyncio
async def test_tailer_cursor_advances_monotonically():
    """Successive fetch_for_channel_tailer calls must not redeliver messages."""
    await db.init_db()
    await db.register_session("alice")
    await db.register_session("bob")
    await db.send_message("alice", "bob", "m1")
    await db.send_message("alice", "bob", "m2")

    first = await db.fetch_for_channel_tailer("bob")
    assert [m.body for m in first] == ["m1", "m2"]

    # Nothing new — cursor sits at m2's id.
    second = await db.fetch_for_channel_tailer("bob")
    assert second == []

    # New send → tailer picks it up, others already delivered are not.
    await db.send_message("alice", "bob", "m3")
    third = await db.fetch_for_channel_tailer("bob")
    assert [m.body for m in third] == ["m3"]


@pytest.mark.asyncio
async def test_tailer_picks_up_channel_broadcasts():
    """Broadcasts (to channels, not DMs) should also flow through the tailer."""
    await db.init_db()
    await db.register_session("alice")
    await db.register_session("bob")
    await db.broadcast_message("alice", "general announcement", "general")

    tailed = await db.fetch_for_channel_tailer("bob")
    assert len(tailed) == 1
    assert tailed[0].body == "general announcement"
    assert tailed[0].channel == "general"


@pytest.mark.asyncio
async def test_tailer_does_not_pick_up_own_broadcasts():
    """A session shouldn't see its own broadcast echoed back via the tailer."""
    await db.init_db()
    await db.register_session("alice")
    await db.broadcast_message("alice", "my own message", "general")

    tailed = await db.fetch_for_channel_tailer("alice")
    assert tailed == []


# --- Backlog cap + channel subscriptions (0.7.0) ---


@pytest.mark.asyncio
async def test_register_auto_subscribes_to_general():
    await db.init_db()
    await db.register_session("alice")
    assert await db.list_subscriptions("alice") == ["general"]


@pytest.mark.asyncio
async def test_unsubscribe_survives_reregistration():
    """A deliberate unsubscribe must not be undone by re-registering."""
    await db.init_db()
    await db.register_session("alice")
    await db.create_channel("noisy")
    await db.subscribe("alice", "noisy")
    await db.unsubscribe("alice", "general")
    assert await db.list_subscriptions("alice") == ["noisy"]

    await db.register_session("alice")
    assert await db.list_subscriptions("alice") == ["noisy"], "re-register must not re-add general"


@pytest.mark.asyncio
async def test_tailer_skips_unsubscribed_channels():
    await db.init_db()
    await db.register_session("alice")
    await db.register_session("bob")
    await db.create_channel("noisy")

    await db.broadcast_message("alice", "general msg", "general")
    await db.broadcast_message("alice", "noisy msg", "noisy")

    # bob is only subscribed to general
    tailed = await db.fetch_for_channel_tailer("bob")
    assert [m.body for m in tailed] == ["general msg"]

    # subscribing mid-session only opens the tap for NEW traffic — the earlier
    # "noisy msg" stays out of context (still readable via intercom_history)
    await db.subscribe("bob", "noisy")
    await db.broadcast_message("alice", "noisy msg 2", "noisy")
    tailed2 = await db.fetch_for_channel_tailer("bob")
    assert [m.body for m in tailed2] == ["noisy msg 2"]


@pytest.mark.asyncio
async def test_tailer_always_delivers_dms_regardless_of_subscriptions():
    """DMs are not subscribable — you can't mute someone messaging you directly."""
    await db.init_db()
    await db.register_session("alice")
    await db.register_session("bob")
    await db.unsubscribe("bob", "general")

    await db.send_message("alice", "bob", "direct message")
    tailed = await db.fetch_for_channel_tailer("bob")
    assert [m.body for m in tailed] == ["direct message"]


@pytest.mark.asyncio
async def test_backlog_cap_skips_old_messages():
    """The whole point: a fresh session must not replay a huge channel history."""
    await db.init_db()
    await db.register_session("alice")
    for i in range(25):
        await db.broadcast_message("alice", f"msg{i}", "general")

    # bob registers fresh with a backlog of 5
    await db.register_session("bob")
    replay = await db.init_tailer_cursor("bob", backlog=5)
    assert replay == {"pending": 5, "skipped": 20}

    tailed = await db.fetch_for_channel_tailer("bob", limit=100)
    assert [m.body for m in tailed] == ["msg20", "msg21", "msg22", "msg23", "msg24"]


@pytest.mark.asyncio
async def test_backlog_zero_starts_clean():
    await db.init_db()
    await db.register_session("alice")
    for i in range(10):
        await db.broadcast_message("alice", f"msg{i}", "general")

    await db.register_session("bob")
    replay = await db.init_tailer_cursor("bob", backlog=0)
    assert replay == {"pending": 0, "skipped": 10}
    assert await db.fetch_for_channel_tailer("bob", limit=100) == []

    # ...but new traffic still arrives
    await db.broadcast_message("alice", "after registration", "general")
    tailed = await db.fetch_for_channel_tailer("bob", limit=100)
    assert [m.body for m in tailed] == ["after registration"]


@pytest.mark.asyncio
async def test_backlog_noop_when_under_cap():
    await db.init_db()
    await db.register_session("alice")
    await db.register_session("bob")
    await db.broadcast_message("alice", "only one", "general")

    replay = await db.init_tailer_cursor("bob", backlog=10)
    assert replay == {"pending": 1, "skipped": 0}
    assert len(await db.fetch_for_channel_tailer("bob")) == 1


@pytest.mark.asyncio
async def test_backlog_does_not_touch_poll_cursor():
    """Skipped-by-backlog messages must still be recoverable via intercom_poll."""
    await db.init_db()
    await db.register_session("alice")
    await db.register_session("bob")
    for i in range(10):
        await db.send_message("alice", "bob", f"dm{i}")

    await db.init_tailer_cursor("bob", backlog=2)
    # tailer only replays the last 2...
    assert len(await db.fetch_for_channel_tailer("bob", limit=100)) == 2
    # ...but poll still sees all 10, because its cursors are untouched
    polled, _ = await db.poll_messages("bob", limit=100)
    assert len(polled) == 10


@pytest.mark.asyncio
async def test_subscribe_to_missing_channel_errors():
    await db.init_db()
    await db.register_session("alice")
    with pytest.raises(ValueError, match="not found"):
        await db.subscribe("alice", "nonexistent")
