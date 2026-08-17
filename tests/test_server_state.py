"""Smoke tests for the MCP server's per-process session state and tool dispatch."""

import json

import pytest

from session_intercom import db, server


@pytest.fixture(autouse=True)
def clear_state():
    """Reset module-level session state between tests."""
    server._current_session = None
    # Re-create the tailer ready event so each test starts with a clean state.
    import anyio

    server._tailer_ready = anyio.Event()
    yield
    server._current_session = None


def _payload(text_content_list):
    """Parse the JSON body out of the TextContent the call_tool handler returns."""
    assert len(text_content_list) == 1
    return json.loads(text_content_list[0].text)


# --- _resolve_name primitive ---


def test_resolve_name_explicit_override():
    server._current_session = "alice"
    assert server._resolve_name("bob") == "bob"


def test_resolve_name_uses_state():
    server._current_session = "alice"
    assert server._resolve_name(None) == "alice"


def test_resolve_name_errors_when_no_state():
    with pytest.raises(ValueError, match="No session name available"):
        server._resolve_name(None)


# --- Registration sets current session + arms the tailer ---


@pytest.mark.asyncio
async def test_register_sets_current_session_and_arms_tailer():
    await db.init_db()
    assert server._current_session is None
    assert not server._tailer_ready.is_set()

    result = _payload(await server.call_tool("intercom_register", {"name": "alice"}))

    assert result["status"] == "registered"
    assert server._current_session == "alice"
    assert server._tailer_ready.is_set()


@pytest.mark.asyncio
async def test_register_response_has_no_inbox_fields():
    """The mbox-era fields (delivery_health, inbox_file_ready, recovery) are gone."""
    await db.init_db()
    result = _payload(await server.call_tool("intercom_register", {"name": "alice"}))

    for legacy in (
        "delivery_health",
        "inbox_file_ready",
        "recovery",
        "next_step",
        "tip",
        "delivery_caveat",
        "binding_mismatch",
        "unread_in_file_inbox",
    ):
        assert legacy not in result, (
            f"{legacy} should not be in the response — it was an mbox-era field"
        )


# --- Implicit identity (from_name / name optional after register) ---


@pytest.mark.asyncio
async def test_send_uses_registered_name_implicitly():
    await db.init_db()
    await server.call_tool("intercom_register", {"name": "alice"})
    await server.call_tool("intercom_register", {"name": "bob"})
    await server.call_tool("intercom_register", {"name": "alice"})  # alice is current

    result = _payload(await server.call_tool("intercom_send", {"to_name": "bob", "body": "hi"}))

    assert result["status"] == "sent"
    assert result["from"] == "alice"
    assert result["to"] == "bob"


@pytest.mark.asyncio
async def test_send_explicit_from_name_overrides_state():
    await db.init_db()
    await server.call_tool("intercom_register", {"name": "alice"})
    await server.call_tool("intercom_register", {"name": "bob"})  # bob is current

    result = _payload(
        await server.call_tool(
            "intercom_send", {"to_name": "alice", "body": "ping", "from_name": "bob"}
        )
    )

    assert result["from"] == "bob"


@pytest.mark.asyncio
async def test_send_without_register_returns_helpful_error():
    await db.init_db()
    result = _payload(await server.call_tool("intercom_send", {"to_name": "someone", "body": "hi"}))
    assert "error" in result
    assert "No session name available" in result["error"]


@pytest.mark.asyncio
async def test_poll_uses_registered_name_implicitly():
    await db.init_db()
    await server.call_tool("intercom_register", {"name": "alice"})
    await server.call_tool("intercom_register", {"name": "bob"})
    await server.call_tool("intercom_register", {"name": "alice"})
    await server.call_tool("intercom_send", {"to_name": "alice", "body": "msg", "from_name": "bob"})

    result = _payload(await server.call_tool("intercom_poll", {}))

    assert result["count"] == 1
    assert result["messages"][0]["body"] == "msg"


# --- Cleanup uses the durable constant by default ---


@pytest.mark.asyncio
async def test_cleanup_default_uses_constant():
    """Default ttl should match the durability story (2 weeks)."""
    await db.init_db()
    result = _payload(await server.call_tool("intercom_cleanup", {}))
    assert result["ttl_minutes"] == db.CLEANUP_MINUTES
    assert result["ttl_minutes"] >= 20000  # ~2 weeks


# --- Tool surface no longer advertises diagnose / heartbeat ---


@pytest.mark.asyncio
async def test_tool_list_dropped_diagnose_and_heartbeat():
    # list_tools is registered via a decorator that statically declares one
    # arg in its overload, but the actual returned function is no-arg. Cast to
    # silence pyright.
    tools = await server.list_tools()  # type: ignore[call-arg]
    names = {t.name for t in tools}
    assert "intercom_diagnose" not in names, "intercom_diagnose was mbox-only and should be gone"
    assert "intercom_heartbeat" not in names, "intercom_heartbeat has been vestigial since v0.4"


# --- Channels capability is declared in the experimental block ---


def test_server_declares_channels_capability():
    """The capability the CLI looks for is experimental['claude/channel'] = {}."""
    from mcp.server.lowlevel import NotificationOptions

    caps = server.server.get_capabilities(
        notification_options=NotificationOptions(),
        experimental_capabilities={"claude/channel": {}},
    )
    assert caps.experimental == {"claude/channel": {}}


# --- Channel notification wire format ---


@pytest.mark.asyncio
async def test_emit_channel_writes_correct_jsonrpc_notification():
    """_emit_channel must write a method='notifications/claude/channel' notification with
    content + meta to the supplied write stream."""
    captured: list = []

    class FakeStream:
        async def send(self, msg):
            captured.append(msg)

    await server._emit_channel(
        FakeStream(), "hello from alice", {"from": "alice", "message_id": "42"}
    )

    assert len(captured) == 1
    payload = captured[0].message.model_dump(by_alias=True, exclude_none=True)
    # JSONRPCMessage is a RootModel — unwrap if needed
    if "root" in payload:
        payload = payload["root"]
    assert payload["method"] == "notifications/claude/channel"
    assert payload["params"]["content"] == "hello from alice"
    assert payload["params"]["meta"] == {"from": "alice", "message_id": "42"}
    assert payload["jsonrpc"] == "2.0"


def test_meta_safe_drops_none_and_non_identifier_keys():
    out = server._meta_safe(
        {
            "from": "alice",
            "message_id": 42,  # int → stringified
            "thread_id": None,  # None → dropped
            "x-bad": "v",  # hyphen → dropped
            "channel": "general",
        }
    )
    assert out == {"from": "alice", "message_id": "42", "channel": "general"}


# --- SDK-contract regressions (would have caught the mcp 2.0 break, see #7/#8) ---


def test_tools_handlers_are_registered_on_the_server():
    """Handler registration must survive SDK API changes.

    mcp 2.0 dropped the @server.list_tools()/@server.call_tool() decorators for
    add_request_handler(). Under 1.x the decorators registered handlers at
    import; if a future SDK moves the goalposts again, the decorators silently
    become no-ops and the server starts but serves zero tools. Assert the
    handlers are actually wired.
    """
    assert server.server.get_request_handler("tools/list") is not None
    assert server.server.get_request_handler("tools/call") is not None


@pytest.mark.asyncio
async def test_list_tools_handler_returns_every_advertised_tool():
    """The registered tools/list handler must return the full surface.

    Exercises the adapter, not just the inner coroutine — a broken params_type
    or result-model contract shows up here rather than at session startup.
    """
    import mcp.types as types

    result = await server._handle_list_tools(None, types.PaginatedRequestParams())
    names = {t.name for t in result.tools}
    assert names == {t.name for t in await server.list_tools()}
    assert "intercom_register" in names
    assert "intercom_send" in names
    assert "intercom_subscribe" in names


@pytest.mark.asyncio
async def test_call_tool_handler_dispatches_through_adapter():
    """The registered tools/call handler must reach the tool body and wrap the
    result in the SDK's result model."""
    import mcp.types as types

    await db.init_db()
    params = types.CallToolRequestParams(name="intercom_register", arguments={"name": "alice"})
    result = await server._handle_call_tool(None, params)

    assert isinstance(result, types.CallToolResult)
    block = result.content[0]
    assert isinstance(block, types.TextContent), f"expected TextContent, got {type(block)}"
    payload = json.loads(block.text)
    assert payload["status"] == "registered"
    assert server._current_session == "alice"


@pytest.mark.asyncio
async def test_emit_channel_survives_the_transport_serialization_path():
    """Serialize exactly the way stdio_server does, so a change in how the SDK
    models JSON-RPC frames fails here instead of in production.

    mcp 2.0 turned JSONRPCMessage from a RootModel wrapper into a plain type
    union, so the old `JSONRPCMessage(notification)` call raised TypeError at
    runtime — in the channel-delivery hot path, invisible to import-time checks.
    """
    captured: list = []

    class FakeStream:
        async def send(self, msg):
            captured.append(msg)

    await server._emit_channel(FakeStream(), "body text", {"from": "alice"})

    # This is stdio_server's write loop, verbatim.
    wire = captured[0].message.model_dump_json(by_alias=True, exclude_unset=True)
    frame = json.loads(wire)
    assert frame == {
        "jsonrpc": "2.0",
        "method": "notifications/claude/channel",
        "params": {"content": "body text", "meta": {"from": "alice"}},
    }
