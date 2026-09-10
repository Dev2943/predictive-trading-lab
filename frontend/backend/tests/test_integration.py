"""Integration tests against the REAL engine.

The API suite runs on a fake, which keeps route tests fast and makes failure
paths reachable. These tests exist so the fake cannot drift into describing an
engine that does not exist: they assert the real engine still agrees with what
the fake claims.

Skipped when the bindings are not built, so a contributor working on routes
without a C++ toolchain still gets a green suite.
"""

from __future__ import annotations

import pathlib

import pytest
from fastapi.testclient import TestClient

ptl = pytest.importorskip("ptl", reason="bindings not built")

from app.main import app  # noqa: E402

REPO_ROOT = pathlib.Path(__file__).resolve().parents[3]


@pytest.fixture
def client(monkeypatch):
    # The engine resolves config paths relative to the working directory.
    monkeypatch.chdir(REPO_ROOT)
    with TestClient(app) as test_client:
        yield test_client


def test_the_real_engine_reports_its_version(client):
    body = client.get("/version").json()
    assert body["engine_version"].startswith("1.0")
    assert body["transport"] == "in-process"


def test_the_real_engine_reproduces_the_reference_build(client):
    body = client.get("/fingerprints").json()
    assert body["matches_reference"] is True


def test_going_through_http_does_not_perturb_determinism(client):
    """The gateway observes; it changes nothing.

    Exercise every read-only endpoint, then re-read the fingerprints.
    """
    before = client.get("/fingerprints").json()

    for path in (
        "/",
        "/health",
        "/version",
        "/system",
        "/config",
        "/optimization",
        "/analytics",
        "/risk",
        "/artifacts",
        "/portfolio",
        "/positions",
        "/diagnostics",
        "/metrics",
        "/reports",
    ):
        assert client.get(path).status_code == 200

    assert client.get("/fingerprints").json() == before


def test_the_fake_optimizer_list_matches_the_real_engine(client):
    """Guards against the fake drifting from the engine it stands in for."""
    from .fakes import FakeEngineClient

    real = {o["name"] for o in client.get("/optimization").json()["optimizers"]}
    assert real == set(FakeEngineClient().optimizer_names())


def test_optimizer_requirements_match_the_engines_own_refusals(client):
    """The capability table is a claim about the engine. Ask the engine.

    Checked in BOTH directions: an optimizer the table says needs a covariance
    must refuse without one, and one the table says does not must succeed. A
    one-directional check would let the table over-claim, which is how a UI ends
    up disabling an optimizer that works perfectly well.
    """
    covariance = [[0.04 if i == j else 0.01 for j in range(4)] for i in range(4)]

    for entry in client.get("/optimization").json()["optimizers"]:
        name = entry["name"]

        if entry["requires_covariance"]:
            with pytest.raises(RuntimeError):
                ptl.optimize(name, volatilities=[0.1] * 4, expected_returns=[0.02] * 4)
        else:
            ptl.optimize(
                name,
                volatilities=[0.1] * 4,
                expected_returns=[0.02] * 4,
                signals=[0.5] * 4,
            )

        if entry["requires_expected_returns"]:
            with pytest.raises(RuntimeError):
                ptl.optimize(name, volatilities=[0.1] * 4, covariance=covariance)
        else:
            ptl.optimize(
                name,
                volatilities=[0.1] * 4,
                covariance=covariance,
                signals=[0.5] * 4,
            )


# ---------------------------------------------------------------------------
# F3: a real PaperSession through the host
# ---------------------------------------------------------------------------


def test_a_real_paper_session_runs_end_to_end():
    """The fake backend proves the state machine; this proves the engine.

    Runs a genuine PaperSession -- engine, broker, OMS, risk, portfolio,
    journal -- and asserts it produced orders, fills and a moved book.
    """
    import json as _json
    import time as _time

    from app.engine import SessionDriver, SessionState

    driver = SessionDriver(ptl, step_size=50)
    try:
        driver.start(session_id="integration", seed=20240101, bars=200)
        assert driver.state is SessionState.RUNNING

        deadline = _time.monotonic() + 10.0
        while _time.monotonic() < deadline:
            if driver.status()["replay_exhausted"]:
                break
            _time.sleep(0.02)

        snapshot = driver.snapshot()
        engine_state = snapshot["state"]
        assert engine_state["events_processed"] > 0
        assert engine_state["orders_submitted"] > 0
        assert engine_state["fills_received"] > 0
        # The data source is labelled, so nothing downstream can mistake a
        # synthetic replay for a live feed.
        assert engine_state["data_source"] == "synthetic-replay"

        account = snapshot["portfolio"]["account"]
        assert account["equity"] != 0.0

        driver.stop()
        assert driver.state is SessionState.STOPPED
    finally:
        driver.shutdown()


def test_the_same_seed_reproduces_the_same_session():
    """Determinism survives the host.

    Two sessions with one seed must produce identical counters. If they did
    not, the session host would have introduced non-determinism the engine does
    not have.
    """
    import time as _time

    from app.engine import SessionDriver

    def run() -> dict:
        driver = SessionDriver(ptl, step_size=50)
        try:
            driver.start(session_id="determinism", seed=4242, bars=150)
            deadline = _time.monotonic() + 10.0
            while _time.monotonic() < deadline:
                if driver.status()["replay_exhausted"]:
                    break
                _time.sleep(0.02)
            state = dict(driver.snapshot()["state"])
            account = dict(driver.snapshot()["portfolio"]["account"])
            driver.stop()
            return {
                "events": state["events_processed"],
                "orders": state["orders_submitted"],
                "fills": state["fills_received"],
                "equity": account["equity"],
            }
        finally:
            driver.shutdown()

    first = run()
    second = run()
    # EXACT equality on equity: float summation is not associative, so any
    # ordering difference would surface in the last bits.
    assert first == second
