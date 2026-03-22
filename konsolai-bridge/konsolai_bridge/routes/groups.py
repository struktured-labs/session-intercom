"""Session folder (group) REST endpoints — CRUD + membership."""

from __future__ import annotations

from fastapi import APIRouter, Depends, HTTPException, Request, status

from ..auth import verify_token
from ..models import SessionFolderCreate, SessionFolderInfo, SessionFolderUpdate

router = APIRouter(prefix="/api/groups", tags=["groups"])


def _registry(request: Request):
    return request.app.state.registry


# ---------------------------------------------------------------------------
# List / Create
# ---------------------------------------------------------------------------

@router.get("", response_model=list[SessionFolderInfo])
def list_groups(
    registry=Depends(_registry),
    _token: str = Depends(verify_token),
):
    """List all session folders."""
    return registry.list_folders()


@router.post("", response_model=SessionFolderInfo, status_code=status.HTTP_201_CREATED)
def create_group(
    body: SessionFolderCreate,
    registry=Depends(_registry),
    _token: str = Depends(verify_token),
):
    """Create a new session folder."""
    return registry.create_folder(name=body.name, color=body.color)


# ---------------------------------------------------------------------------
# Update / Delete
# ---------------------------------------------------------------------------

@router.put("/{folder_id}", response_model=SessionFolderInfo)
def update_group(
    folder_id: str,
    body: SessionFolderUpdate,
    registry=Depends(_registry),
    _token: str = Depends(verify_token),
):
    """Update an existing session folder."""
    result = registry.update_folder(
        folder_id,
        name=body.name,
        color=body.color,
        sort_order=body.sort_order,
    )
    if result is None:
        raise HTTPException(status.HTTP_404_NOT_FOUND, f"Folder '{folder_id}' not found")
    return result


@router.delete("/{folder_id}", status_code=status.HTTP_204_NO_CONTENT)
def delete_group(
    folder_id: str,
    registry=Depends(_registry),
    _token: str = Depends(verify_token),
):
    """Delete a session folder and ungroup all its members."""
    deleted = registry.delete_folder(folder_id)
    if not deleted:
        raise HTTPException(status.HTTP_404_NOT_FOUND, f"Folder '{folder_id}' not found")


# ---------------------------------------------------------------------------
# Membership
# ---------------------------------------------------------------------------

@router.post("/{folder_id}/members/{session_id}", status_code=status.HTTP_200_OK)
def add_member(
    folder_id: str,
    session_id: str,
    registry=Depends(_registry),
    _token: str = Depends(verify_token),
):
    """Add a session to a folder."""
    # Verify folder exists
    folders = registry.list_folders()
    if not any(f.folder_id == folder_id for f in folders):
        raise HTTPException(status.HTTP_404_NOT_FOUND, f"Folder '{folder_id}' not found")
    moved = registry.move_session_to_folder(session_id, folder_id)
    if not moved:
        raise HTTPException(status.HTTP_404_NOT_FOUND, f"Session '{session_id}' not found")
    return {"status": "added", "session": session_id, "folder": folder_id}


@router.delete("/{folder_id}/members/{session_id}", status_code=status.HTTP_200_OK)
def remove_member(
    folder_id: str,
    session_id: str,
    registry=Depends(_registry),
    _token: str = Depends(verify_token),
):
    """Remove a session from a folder."""
    removed = registry.remove_session_from_folder(session_id)
    if not removed:
        raise HTTPException(status.HTTP_404_NOT_FOUND, f"Session '{session_id}' not found")
    return {"status": "removed", "session": session_id, "folder": folder_id}
