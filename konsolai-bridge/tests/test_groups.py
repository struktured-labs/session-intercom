"""Tests for session folder (group) REST endpoints."""

from __future__ import annotations

import json


def test_list_groups_empty(client):
    """Empty list on fresh start."""
    resp = client.get("/api/groups")
    assert resp.status_code == 200
    assert resp.json() == []


def test_create_group(client):
    """Creates a folder with name and color."""
    resp = client.post("/api/groups", json={"name": "Work", "color": "#ff5500"})
    assert resp.status_code == 201
    data = resp.json()
    assert data["name"] == "Work"
    assert data["color"] == "#ff5500"
    assert data["folder_id"]
    assert data["sort_order"] >= 1
    assert data["member_count"] == 0

    # Verify it appears in the list
    resp2 = client.get("/api/groups")
    assert resp2.status_code == 200
    folders = resp2.json()
    assert len(folders) == 1
    assert folders[0]["folder_id"] == data["folder_id"]


def test_rename_group(client):
    """PUT with new name updates the folder."""
    # Create first
    resp = client.post("/api/groups", json={"name": "Old Name", "color": "#000000"})
    assert resp.status_code == 201
    folder_id = resp.json()["folder_id"]

    # Rename
    resp2 = client.put(f"/api/groups/{folder_id}", json={"name": "New Name"})
    assert resp2.status_code == 200
    assert resp2.json()["name"] == "New Name"
    # Color unchanged
    assert resp2.json()["color"] == "#000000"


def test_update_group_not_found(client):
    """PUT on a non-existent folder returns 404."""
    resp = client.put("/api/groups/nonexistent", json={"name": "Nope"})
    assert resp.status_code == 404


def test_delete_group(client, config):
    """Deletes a folder and ungroups its members."""
    # Create a folder
    resp = client.post("/api/groups", json={"name": "ToDelete"})
    folder_id = resp.json()["folder_id"]

    # Create a session entry that belongs to this folder
    sessions_data = [
        {"name": "konsolai-Default-a1b2c3d4", "sessionId": "a1b2c3d4", "folderId": folder_id},
        {"name": "konsolai-Dev-e5f6a7b8", "sessionId": "e5f6a7b8"},
    ]
    config.sessions_file.write_text(json.dumps(sessions_data, indent=2))

    # Delete the folder
    resp2 = client.delete(f"/api/groups/{folder_id}")
    assert resp2.status_code == 204

    # Folder is gone
    resp3 = client.get("/api/groups")
    assert len(resp3.json()) == 0

    # Session no longer has folderId
    data = json.loads(config.sessions_file.read_text())
    for entry in data:
        assert "folderId" not in entry


def test_delete_group_not_found(client):
    """DELETE on a non-existent folder returns 404."""
    resp = client.delete("/api/groups/nonexistent")
    assert resp.status_code == 404


def test_add_session_to_group(client, config):
    """POST member assigns a session to a folder."""
    # Create a folder
    resp = client.post("/api/groups", json={"name": "MyGroup"})
    folder_id = resp.json()["folder_id"]

    # Write a session entry
    sessions_data = [
        {"name": "konsolai-Default-a1b2c3d4", "sessionId": "a1b2c3d4"},
    ]
    config.sessions_file.write_text(json.dumps(sessions_data, indent=2))

    # Add session to folder
    resp2 = client.post(f"/api/groups/{folder_id}/members/konsolai-Default-a1b2c3d4")
    assert resp2.status_code == 200
    assert resp2.json()["status"] == "added"

    # Verify in sessions.json
    data = json.loads(config.sessions_file.read_text())
    assert data[0]["folderId"] == folder_id


def test_add_session_to_nonexistent_group(client, config):
    """POST member to a non-existent folder returns 404."""
    sessions_data = [{"name": "konsolai-Default-a1b2c3d4", "sessionId": "a1b2c3d4"}]
    config.sessions_file.write_text(json.dumps(sessions_data, indent=2))

    resp = client.post("/api/groups/nonexistent/members/konsolai-Default-a1b2c3d4")
    assert resp.status_code == 404


def test_add_nonexistent_session_to_group(client):
    """POST member with a non-existent session returns 404."""
    resp = client.post("/api/groups", json={"name": "G"})
    folder_id = resp.json()["folder_id"]

    resp2 = client.post(f"/api/groups/{folder_id}/members/no-such-session")
    assert resp2.status_code == 404


def test_remove_session_from_group(client, config):
    """DELETE member removes the session from a folder."""
    # Create a folder
    resp = client.post("/api/groups", json={"name": "RemTest"})
    folder_id = resp.json()["folder_id"]

    # Write a session that belongs to the folder
    sessions_data = [
        {"name": "konsolai-Default-a1b2c3d4", "sessionId": "a1b2c3d4", "folderId": folder_id},
    ]
    config.sessions_file.write_text(json.dumps(sessions_data, indent=2))

    # Remove from folder
    resp2 = client.delete(f"/api/groups/{folder_id}/members/konsolai-Default-a1b2c3d4")
    assert resp2.status_code == 200
    assert resp2.json()["status"] == "removed"

    # Verify folderId cleared
    data = json.loads(config.sessions_file.read_text())
    assert "folderId" not in data[0]


def test_session_summary_includes_folder_id(client, config):
    """Verify folder_id appears in session list responses."""
    # Create a folder
    resp = client.post("/api/groups", json={"name": "ShowFolder"})
    folder_id = resp.json()["folder_id"]

    # Write a session with folderId
    sessions_data = [
        {"name": "konsolai-Default-a1b2c3d4", "sessionId": "a1b2c3d4", "folderId": folder_id},
    ]
    config.sessions_file.write_text(json.dumps(sessions_data, indent=2))

    # Get session list
    resp2 = client.get("/api/sessions")
    assert resp2.status_code == 200
    data = resp2.json()
    # Find the session with our folder_id
    matched = [s for s in data if s.get("folder_id") == folder_id]
    assert len(matched) == 1
    assert matched[0]["name"] == "konsolai-Default-a1b2c3d4"


def test_auth_required_groups(unauth_client):
    """Group endpoints require authentication."""
    resp = unauth_client.get("/api/groups")
    assert resp.status_code == 401


def test_member_count_updates(client, config):
    """Folder member_count reflects actual session membership."""
    # Create a folder
    resp = client.post("/api/groups", json={"name": "Counted"})
    folder_id = resp.json()["folder_id"]
    assert resp.json()["member_count"] == 0

    # Write sessions, two in this folder
    sessions_data = [
        {"name": "konsolai-Default-a1b2c3d4", "sessionId": "a1b2c3d4", "folderId": folder_id},
        {"name": "konsolai-Dev-e5f6a7b8", "sessionId": "e5f6a7b8", "folderId": folder_id},
    ]
    config.sessions_file.write_text(json.dumps(sessions_data, indent=2))

    # List folders -- should show count=2
    resp2 = client.get("/api/groups")
    folders = resp2.json()
    assert len(folders) == 1
    assert folders[0]["member_count"] == 2
