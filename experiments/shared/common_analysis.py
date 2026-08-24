"""Small shared helpers for experiment-local analyzers.

The existing analyzer remains in the compatibility path until the next
encapsulation checkpoint; this module provides a stable home for new shared
helpers without changing result semantics.
"""

from __future__ import annotations

from pathlib import Path
import json


def load_json(path: str | Path) -> dict:
    return json.loads(Path(path).read_text())
