from pathlib import Path

import pytest

from session_intercom.db import _common


@pytest.fixture(autouse=True)
def tmp_db(tmp_path: Path, monkeypatch: pytest.MonkeyPatch):
    """Redirect DB to a temp directory for each test.

    Patches at the _common module — that's where get_connection actually
    reads DB_DIR / DB_PATH from. The package facade (db/__init__.py)
    re-exports them as fresh bindings, so patching db.DB_PATH alone would
    have no effect on the connection function.
    """
    monkeypatch.setattr(_common, "DB_DIR", tmp_path)
    monkeypatch.setattr(_common, "DB_PATH", tmp_path / "test.db")
