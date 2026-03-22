import os
import tempfile
from pathlib import Path

import pytest

from session_intercom import db


@pytest.fixture(autouse=True)
def tmp_db(tmp_path: Path, monkeypatch: pytest.MonkeyPatch):
    """Redirect DB to a temp directory for each test."""
    monkeypatch.setattr(db, "DB_DIR", tmp_path)
    monkeypatch.setattr(db, "DB_PATH", tmp_path / "test.db")
