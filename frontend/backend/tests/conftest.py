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


import pytest


@pytest.fixture(autouse=True)
def _clean_session_host():
    """Guarantee a clean C++ session host around every test.

    THE HOST IS A PROCESS SINGLETON. `PaperSessionHost` is a function-local
    static, so every SessionDriver in the process drives the SAME session. A
    driver whose thread outlives its test keeps stepping that session, and the
    next test observes a book it did not create -- or has its own session
    replaced underneath it.

    That made the integration suite order-dependent: tests passed in isolation
    and failed together. This forces the host back to a known state before and
    after each test, so a test can only ever see its own session.
    """
    def _stop_quietly() -> None:
        try:
            import ptl
        except ImportError:
            return
        try:
            ptl.session_stop()
        except RuntimeError:
            # Nothing running, which is the state we wanted.
            pass

    _stop_quietly()
    yield
    _stop_quietly()
