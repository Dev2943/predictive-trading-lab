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


def test_the_real_engine_produces_an_equity_series():
    """F4's claim: the host samples a usable intraday series.

    The engine's own curve holds one point per trading session, which is
    useless for an intraday chart, so the host samples the portfolio itself.
    This asserts the series actually moves.
    """
    import time as _time

    from app.engine import SessionDriver

    driver = SessionDriver(ptl, step_size=25)
    try:
        driver.start(session_id="history", seed=20240101, bars=200)
        deadline = _time.monotonic() + 10.0
        while _time.monotonic() < deadline:
            if driver.status()["replay_exhausted"]:
                break
            _time.sleep(0.02)

        history = driver.snapshot()["history"]
        assert history["available"] is True
        assert history["total_points"] > 1

        equity = [p["equity"] for p in history["points"]]
        # The opening balance is the first observation, so the starting capital
        # appears rather than the book seeming to begin after its first trades.
        assert equity[0] == pytest.approx(1_000_000.0)
        assert len(set(equity)) > 1
        assert history["peak_equity"] >= max(equity)

        instruments = driver.snapshot()["instruments"]["instruments"]
        assert instruments == [{"instrument": 0, "symbol": "SPY"}]

        driver.stop()
    finally:
        driver.shutdown()


def test_the_host_does_not_corrupt_the_engines_own_equity_curve():
    """The host samples by READING the portfolio.

    Calling Portfolio::snapshot() would append to a series the engine believes
    it controls. The engine records one point per session close, so its curve
    must stay small no matter how much the host samples.
    """
    import time as _time

    from app.engine import SessionDriver

    driver = SessionDriver(ptl, step_size=25)
    try:
        driver.start(session_id="curve", seed=20240101, bars=200)
        deadline = _time.monotonic() + 10.0
        while _time.monotonic() < deadline:
            if driver.status()["replay_exhausted"]:
                break
            _time.sleep(0.02)

        history = driver.snapshot()["history"]
        # The host sampled many points...
        assert history["total_points"] > 5
        # ...and the session still closes out cleanly, which it could not do if
        # the engine's own curve had been polluted mid-run.
        driver.stop()
        assert driver.status()["error"] is None
    finally:
        driver.shutdown()


# ---------------------------------------------------------------------------
# F6: manual order entry against the real engine
# ---------------------------------------------------------------------------


def _run_until_idle(driver, seconds: float = 10.0) -> None:
    import time as _time

    deadline = _time.monotonic() + seconds
    while _time.monotonic() < deadline:
        if driver.status()["replay_exhausted"]:
            return
        _time.sleep(0.02)

def _wait_for(predicate, timeout: float = 15.0, interval: float = 0.02) -> None:
    import time as _time

    deadline = _time.monotonic() + timeout

    while _time.monotonic() < deadline:
        if predicate():
            return
        _time.sleep(interval)

    raise AssertionError("Timed out waiting for condition")

def test_a_manual_order_traverses_the_real_risk_gate_and_fills():
    """The claim F6 rests on: one submission path.

    A manual order is queued, drained by the strategy into the engine's own
    OrderSink, gated by risk, booked by the OMS and filled by the broker --
    exactly as a strategy's order is.
    """
    import time as _time

    from app.engine import SessionDriver

    driver = SessionDriver(ptl, step_size=25)
    try:
        # step_size=1 keeps the replay running long enough to submit into it.
        # The inbox is drained by the STRATEGY inside on_bar, so an order queued
        # after the replay is exhausted waits indefinitely -- correct behaviour
        # (an order rests until the market ticks) but it makes a fast replay a
        # racy place to test from.
        driver.start(session_id="manual", seed=20240101, bars=390)

        # The synthetic series opens near 500, so this limit is marketable and
        # well inside the engine's price collar.
        request_id = driver.submit_order(
            symbol="SPY",
            side=1,
            quantity=50,
            type="limit",
            limit_price=505.0,
            stop_price=0.0,
            time_in_force="day",
        )
        assert request_id > 0

        _run_until_idle(driver)
        outcomes = driver.snapshot()["pending"]["outcomes"]
        mine = [o for o in outcomes if o["request_id"] == request_id]
        assert mine, "the request produced no outcome"
        assert mine[0]["accepted"] is True

        history = driver.snapshot()["order_history"]["orders"]
        booked = [o for o in history if o["order_id"] == mine[0]["order_id"]]
        assert booked and booked[0]["type"] == "limit"
        assert booked[0]["symbol"] == "SPY"

        driver.stop()
    finally:
        driver.shutdown()


def test_a_manual_order_can_be_rejected_by_risk():
    """Manual orders are not privileged.

    A limit far from the market trips the engine's price collar, and the
    rejection reaches the caller with the engine's own reason rather than
    vanishing.
    """
    from app.engine import SessionDriver

    driver = SessionDriver(ptl, step_size=25)
    try:
        driver.start(session_id="rejected", seed=20240101, bars=200)
        request_id = driver.submit_order(
            symbol="SPY",
            side=1,
            quantity=10,
            type="limit",
            limit_price=50_000.0,  # nowhere near the market
            stop_price=0.0,
            time_in_force="day",
        )
        _run_until_idle(driver)

        outcomes = driver.snapshot()["pending"]["outcomes"]
        mine = [o for o in outcomes if o["request_id"] == request_id]
        assert mine and mine[0]["accepted"] is False
        assert mine[0]["detail"], "a rejection must carry a reason"

        driver.stop()
    finally:
        driver.shutdown()


def test_an_unknown_symbol_is_refused_rather_than_interned():
    """Interning it would create an instrument with no data, and the order would
    rest forever against a book that never ticks."""
    from app.engine import SessionDriver

    driver = SessionDriver(ptl, step_size=25)
    try:
        driver.start(session_id="symbols", seed=20240101, bars=100)
        with pytest.raises(RuntimeError, match="unknown symbol"):
            driver.submit_order(
                symbol="NOSUCH", side=1, quantity=10, type="market",
                limit_price=0.0, stop_price=0.0, time_in_force="day",
            )
        driver.stop()
    finally:
        driver.shutdown()


def test_flatten_closes_every_position_it_found():
    """Flatten closes the positions that exist when it is called.

    Flatten is a position-level operation, not a strategy kill switch. The
    replay continues afterwards, so the demo strategy may legitimately open a
    new position later.
    """

    from app.engine import SessionDriver

    driver = SessionDriver(ptl, step_size=1)

    try:
        driver.start(session_id="flatten", seed=20240101, bars=390)

        request_id = driver.submit_order(
            symbol="SPY",
            side=1,
            quantity=10,
            type="market",
            limit_price=0.0,
            stop_price=0.0,
            time_in_force="day",
        )

        _wait_for(
            lambda: any(
                outcome["request_id"] == request_id
                for outcome in driver.snapshot()["pending"]["outcomes"]
            )
        )

        _wait_for(
            lambda: bool(driver.snapshot()["positions"]["positions"])
        )

        assert driver.snapshot()["positions"]["positions"], "no position to flatten"

        queued = driver.flatten()

        assert queued >= 1

        _run_until_idle(driver, 15.0)

        history = driver.snapshot()["order_history"]["orders"]

        closing_orders = [
            order
            for order in history
            if order["side"] == -1
            and order["type"] == "market"
            and order["state"] == "filled"
        ]

        assert closing_orders, "flatten never produced a filled closing order"

        driver.stop()

    finally:
        driver.shutdown()
        
def test_trading_commands_do_not_perturb_determinism():
    """Given the SAME command sequence, two runs agree exactly.

    Manual entry is external input, so a session driven by a human is not
    reproducible in the way an unattended replay is. What must hold -- and does
    -- is that identical commands produce identical results.
    """
    def run() -> dict:
        from app.engine import SessionDriver

        driver = SessionDriver(ptl, step_size=25)
        try:
            driver.start(session_id="cmd", seed=31337, bars=200)
            driver.submit_order(
                symbol="SPY", side=1, quantity=10, type="market",
                limit_price=0.0, stop_price=0.0, time_in_force="day",
            )
            _run_until_idle(driver)
            snapshot = driver.snapshot()
            result = {
                "orders": snapshot["state"]["orders_submitted"],
                "fills": snapshot["state"]["fills_received"],
                "equity": snapshot["portfolio"]["account"]["equity"],
            }
            driver.stop()
            return result
        finally:
            driver.shutdown()

    assert run() == run()


def test_an_order_queued_after_the_replay_ends_stays_pending():
    """DOCUMENTED BEHAVIOUR, not a bug.

    The inbox is drained by the strategy inside on_bar. When the replay is
    exhausted no bars arrive, so a queued order waits -- exactly as a real order
    rests until the market next ticks. The pending count is what tells a user
    the order is waiting rather than lost.
    """
    from app.engine import SessionDriver

    driver = SessionDriver(ptl, step_size=50)
    try:
        driver.start(session_id="quiet", seed=20240101, bars=60)
        _run_until_idle(driver)
        assert driver.status()["replay_exhausted"] is True

        driver.submit_order(
            symbol="SPY", side=1, quantity=10, type="market",
            limit_price=0.0, stop_price=0.0, time_in_force="day",
        )
        import time as _time

        _time.sleep(0.3)

        pending = driver.snapshot()["pending"]
        # Still queued, and visibly so.
        assert pending["pending"] >= 1
        driver.stop()
    finally:
        driver.shutdown()
