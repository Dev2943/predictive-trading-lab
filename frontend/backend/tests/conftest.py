"""Backend test fixtures.

Locates the built bindings without an install step and keeps the backend
package importable regardless of the working directory.
"""

from __future__ import annotations

import pathlib
import sys

_backend = pathlib.Path(__file__).resolve().parents[1]
_root = _backend.parents[1]

for candidate in sorted(_root.glob("build/*/lib")) + sorted(_root.glob("build/*")):
    if any(candidate.glob("ptl*.so")):
        sys.path.insert(0, str(candidate))
        break

sys.path.insert(0, str(_backend))
