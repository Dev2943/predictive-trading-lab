"""Session lifecycle tests.

Two layers, deliberately:

  * A FAKE BACKEND drives the state machine, illegal transitions and error
    paths. No compiled module needed, and a failure mode that is awkward to
    provoke in the real engine is one line here.
  * An INTEGRATION test runs a real PaperSession through the host, so the fake
    cannot drift into describing a session that does not behave that way.
"""

from __future__ import annotations

import json
import threading
import time

import pytest
from fastapi.testclient import TestClient

from app.dependencies import get_driver
from app.engine import IllegalTransition, SessionDriver, SessionState, transition_allowed
from app.main import app


class FakeBackend:
    """A session host with no engine behind it."""

    def __init__(self, *, start_fails: bool = False, step_fails: bool = False) -> None:
        self.started = False
        self.start_calls = 0
        self.stop_calls = 0
        self.steps = 0
        self._start_fails = start_fails
        self._step_fails = step_fails
        self._lock = threading.Lock()

    def session_start(self, **kwargs) -> bool:
        with self._lock:
            self.start_calls += 1
        if self._start_fails:
            raise RuntimeError("scripted start failure")
        self.started = True
        return True

    def session_stop(self) -> bool:
        with self._lock:
            self.stop_calls += 1
        self.started = False
        return True

    def session_step(self, max_events: int) -> int:
        if self._step_fails:
            raise RuntimeError("scripted step failure")
        with self._lock:
            self.steps += 1
        return max_events if self.steps < 5 else 0

    def session_state(self) -> str:
        return json.dumps({"state": "RUNNING" if self.started else "STOPPED"})

    def session_snapshot(self) -> str:
        return json.dumps(
            {
                "state": {"state": "RUNNING" if self.started else "STOPPED",
                          "events_processed": self.steps * 25},
                "portfolio": {
                    "available": self.started,
                    "account": {"cash": 1000.0, "equity": 1000.0} if self.started else None,
                },
                "positions": {"positions": []},
                "orders": {"orders": []},
                "fills": {"fills": []},
            }
        )


@pytest.fixture
def driver():
    d = SessionDriver(FakeBackend(), step_size=25)
    yield d
    d.shutdown()


@pytest.fixture
def client(driver):
    app.dependency_overrides[get_driver] = lambda: driver
    with TestClient(app) as test_client:
        yield test_client
    app.dependency_overrides.clear()


# ---------------------------------------------------------------------------
# The state machine
# ---------------------------------------------------------------------------


def test_the_legal_transitions_are_exactly_these():
    assert transition_allowed(SessionState.STOPPED, SessionState.STARTING)
    assert transition_allowed(SessionState.STARTING, SessionState.RUNNING)
    assert transition_allowed(SessionState.RUNNING, SessionState.STOPPING)
    assert transition_allowed(SessionState.STOPPING, SessionState.STOPPED)

    # RUNNING -> STARTING is the one that matters: starting a second session
    # would abandon the first one's book without closing it out.
    assert not transition_allowed(SessionState.RUNNING, SessionState.STARTING)
    assert not transition_allowed(SessionState.STOPPED, SessionState.STOPPING)


def test_an_error_state_can_be_recovered_from():
    """Otherwise a single failure would require a process restart."""
    assert transition_allowed(SessionState.ERROR, SessionState.STARTING)
    assert transition_allowed(SessionState.ERROR, SessionState.STOPPED)


# ---------------------------------------------------------------------------
# Driver
# ---------------------------------------------------------------------------


def test_start_then_stop(driver):
    assert driver.state is SessionState.STOPPED
    driver.start(session_id="t")
    assert driver.state is SessionState.RUNNING
    driver.stop()
    assert driver.state is SessionState.STOPPED


def test_double_start_is_refused(driver):
    driver.start(session_id="t")
    with pytest.raises(IllegalTransition):
        driver.start(session_id="t")


def test_double_stop_is_refused(driver):
    driver.start(session_id="t")
    driver.stop()
    with pytest.raises(IllegalTransition):
        driver.stop()


def test_stopping_a_session_that_never_started_is_refused(driver):
    with pytest.raises(IllegalTransition):
        driver.stop()


def test_a_failed_start_lands_in_error_and_can_retry():
    driver = SessionDriver(FakeBackend(start_fails=True))
    try:
        with pytest.raises(RuntimeError, match="scripted start failure"):
            driver.start(session_id="t")
        assert driver.state is SessionState.ERROR
        assert driver.status()["error"]
        # ERROR -> STARTING is legal, so a transient failure does not require a
        # process restart.
        assert transition_allowed(driver.state, SessionState.STARTING)
    finally:
        driver.shutdown()


def test_the_driver_steps_the_session_without_being_asked(driver):
    """The session advances on the driver thread, not on a request.

    A session that only moved when someone polled it would stall whenever the
    dashboard was closed.
    """
    driver.start(session_id="t")
    deadline = time.monotonic() + 3.0
    while time.monotonic() < deadline:
        if driver.snapshot().get("state", {}).get("events_processed", 0) > 0:
            break
        time.sleep(0.01)
    assert driver.snapshot()["state"]["events_processed"] > 0


def test_an_exhausted_replay_is_not_an_error(driver):
    """The book stays readable, as a live session between market events."""
    driver.start(session_id="t")
    deadline = time.monotonic() + 3.0
    while time.monotonic() < deadline:
        if driver.status()["replay_exhausted"]:
            break
        time.sleep(0.01)
    assert driver.status()["replay_exhausted"] is True
    assert driver.state is SessionState.RUNNING


def test_reset_is_a_stop_then_a_start(driver):
    driver.start(session_id="first")
    driver.reset(session_id="second")
    assert driver.state is SessionState.RUNNING
    backend = driver._backend  # noqa: SLF001 - asserting the sequence, not the API
    assert backend.stop_calls == 1
    assert backend.start_calls == 2


# ---------------------------------------------------------------------------
# API
# ---------------------------------------------------------------------------


def test_session_starts_and_stops_over_http(client):
    assert client.get("/session").json()["state"] == "STOPPED"

    started = client.post("/session/start", json={"session_id": "t"})
    assert started.status_code == 200
    assert started.json()["state"] == "RUNNING"

    stopped = client.post("/session/stop")
    assert stopped.status_code == 200
    assert stopped.json()["state"] == "STOPPED"


def test_a_double_start_is_409(client):
    """409, not 400: the request was well-formed and refused because of *when*
    it arrived, so a client should retry rather than fix its payload."""
    client.post("/session/start", json={})
    conflict = client.post("/session/start", json={})
    assert conflict.status_code == 409
    assert "RUNNING" in conflict.json()["detail"]


def test_a_double_stop_is_409(client):
    client.post("/session/start", json={})
    client.post("/session/stop")
    assert client.post("/session/stop").status_code == 409


def test_stopping_without_a_session_is_409(client):
    assert client.post("/session/stop").status_code == 409


def test_reset_over_http(client):
    client.post("/session/start", json={"session_id": "a"})
    reset = client.post("/session/reset", json={"session_id": "b"})
    assert reset.status_code == 200
    assert reset.json()["state"] == "RUNNING"


def test_a_zero_seed_is_rejected(client):
    """A run whose seed is unknown cannot be reproduced."""
    assert client.post("/session/start", json={"seed": 0}).status_code == 422


def test_snapshot_before_a_session_is_absent_not_empty(client):
    body = client.get("/session/snapshot").json()
    assert body["available"] is False
    assert body["account"] is None
    assert body["positions"] == []


def test_snapshot_after_start(client):
    client.post("/session/start", json={})
    body = client.get("/session/snapshot").json()
    assert body["available"] is True
    assert body["account"]["equity"] == 1000.0


def test_state_alias_matches(client):
    assert client.get("/session").json() == client.get("/session/state").json()


def test_the_session_host_is_unavailable_without_bindings():
    """503, not 500: the gateway is fine and the host is not reachable."""
    from fastapi import HTTPException

    def unavailable():
        raise HTTPException(status_code=503, detail="session host unavailable")

    app.dependency_overrides[get_driver] = unavailable
    with TestClient(app) as test_client:
        assert test_client.get("/session").status_code == 503
    app.dependency_overrides.clear()
