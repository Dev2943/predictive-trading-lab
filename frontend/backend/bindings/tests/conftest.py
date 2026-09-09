"""Locate the built module without an install step.

The bindings build into a CMake tree, not site-packages, so pytest can run
straight after a build. Searching rather than hardcoding one path means the
tests work with any build directory name.
"""

from __future__ import annotations

import pathlib
import sys

# frontend/backend/bindings/tests -> repository root
_root = pathlib.Path(__file__).resolve().parents[4]

for candidate in sorted(_root.glob("build/*/lib")) + sorted(_root.glob("build/*")):
    if any(candidate.glob("ptl*.so")):
        sys.path.insert(0, str(candidate))
        break


import pytest


@pytest.fixture(scope="session")
def engine_root() -> pathlib.Path:
    """The engine source root, so tests do not depend on the working directory."""
    return _root
