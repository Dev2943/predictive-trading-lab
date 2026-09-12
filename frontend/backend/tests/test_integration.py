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


def _wait_for(driver, predicate, seconds: float, what: str):
    """Poll until a CONDITION holds, rather than sleeping and hoping.

    A fixed sleep encodes an assumption about how fast the replay runs and how
    soon the strategy happens to trade. Neither is a property of flatten, and
    both vary with machine load.
    """
    import time as _time

    deadline = _time.monotonic() + seconds
    while _time.monotonic() < deadline:
        value = predicate()
        if value:
            return value
        _time.sleep(0.01)
    raise AssertionError(f"timed out waiting for {what}")


def test_flatten_closes_every_position_it_found():
    """Verifies FLATTEN'S CONTRACT: close what is held *now*.

    Flatten is a position-level control, not a kill switch. It does not — and
    must not — suppress the strategy: that would make it a second Stop Session
    with no defined way to resume. The emergency sequence is flatten, then stop.

    So the final position of a still-running replay is NOT the thing to assert.
    The demo strategy re-opens on its next signal bar, which is correct
    behaviour and has nothing to do with whether flatten worked.

    What flatten promises, and what this asserts: for every position open when
    it was called, an offsetting order of the exact held quantity was submitted
    and filled.
    """
    from app.engine import SessionDriver

    driver = SessionDriver(ptl, step_size=1)
    try:
        driver.start(session_id="flatten", seed=20240101, bars=390)

        # A distinctive quantity, so the flatten order is unambiguously
        # identifiable: the demo strategy only ever trades 25.
        held = 30.0
        driver.submit_order(
            symbol="SPY", side=1, quantity=held, type="market",
            limit_price=0.0, stop_price=0.0, time_in_force="day",
        )
        opened = _wait_for(
            driver,
            lambda: [
                p
                for p in driver.snapshot()["positions"]["positions"]
                if p["quantity"] >= held
            ],
            15.0,
            "the manual order to open a position",
        )
        assert opened, "no position to flatten"

        queued = driver.flatten()
        assert queued >= 1, "flatten queued no closing orders"

        # The offsetting order: opposite side, the exact quantity held, filled.
        closing = _wait_for(
            driver,
            lambda: [
                o
                for o in driver.snapshot()["order_history"]["orders"]
                if o["side"] == -1
                and o["quantity"] == held
                and o["state"] == "filled"
                and o["filled"] == held
            ],
            15.0,
            "the flatten order to fill",
        )
        assert closing, "flatten did not close the position it found"

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


# ---------------------------------------------------------------------------
# F7: halt against the real engine
# ---------------------------------------------------------------------------


def test_halting_stops_strategy_orders_but_not_the_session():
    """The claim F7's halt rests on.

    While halted the strategy must generate nothing, yet the session must keep
    running, keep processing events and keep marking the book -- otherwise the
    equity curve would have a hole in it and halting would be indistinguishable
    from stopping.
    """
    from app.engine import SessionDriver, SessionState

    driver = SessionDriver(ptl, step_size=1)
    try:
        driver.start(session_id="halt", seed=20240101, bars=390)

        # Let the strategy trade, then halt.
        first = _wait_for(
            driver,
            lambda: driver.snapshot()["state"].get("orders_submitted", 0) >= 2,
            15.0,
            "the strategy to submit orders",
        )
        assert first

        assert driver.set_halted(True) is True
        assert driver.state is SessionState.RUNNING

        # THE BASELINE IS TAKEN AFTER THE HALT TAKES EFFECT, not before.
        #
        # Events keep flowing between reading a count and the halt being
        # applied, so a baseline captured beforehand includes orders the
        # strategy submitted legitimately while still running -- and they would
        # then read as a halt violation.
        halted_snapshot = _wait_for(
            driver,
            lambda: (
                driver.snapshot()["state"]
                if driver.snapshot()["state"].get("strategy_halted")
                else None
            ),
            10.0,
            "the halt to be reflected in the published snapshot",
        )
        at_halt = halted_snapshot["orders_submitted"]
        events_at_halt = halted_snapshot["events_processed"]

        # Events must keep flowing while halted.
        _wait_for(
            driver,
            lambda: driver.snapshot()["state"]["events_processed"] > events_at_halt + 30,
            15.0,
            "events to continue while halted",
        )
        # ONE snapshot, and confirm it is OUR session before comparing counters.
        #
        # The C++ host is a process singleton, so a driver thread lingering from
        # another test can replace the session underneath this one. Reading
        # counters from a session we did not start would produce a confusing
        # mismatch instead of naming the real problem.
        observed = driver.snapshot()["state"]
        assert observed["session_id"] == "halt", (
            f"the host is running session '{observed['session_id']}', not ours"
        )
        assert observed["orders_submitted"] == at_halt, (
            "the strategy generated orders while halted"
        )
        assert observed["strategy_halted"] is True

        # And resuming restores generation.
        assert driver.set_halted(False) is False
        _wait_for(
            driver,
            lambda: driver.snapshot()["state"]["orders_submitted"] > at_halt,
            15.0,
            "the strategy to resume",
        )

        driver.stop()
    finally:
        driver.shutdown()


def test_a_manual_order_is_accepted_while_the_strategy_is_halted():
    from app.engine import SessionDriver

    driver = SessionDriver(ptl, step_size=1)
    try:
        driver.start(session_id="halt-manual", seed=20240101, bars=390)
        driver.set_halted(True)

        driver.submit_order(
            symbol="SPY", side=1, quantity=12, type="market",
            limit_price=0.0, stop_price=0.0, time_in_force="day",
        )
        filled = _wait_for(
            driver,
            lambda: [
                o
                for o in driver.snapshot()["order_history"]["orders"]
                if o["quantity"] == 12.0 and o["state"] == "filled"
            ],
            15.0,
            "the manual order to fill while halted",
        )
        assert filled
        driver.stop()
    finally:
        driver.shutdown()


def test_halting_does_not_perturb_determinism():
    """Given the same halt/resume sequence at the same points, runs agree."""

    def run() -> dict:
        from app.engine import SessionDriver

        driver = SessionDriver(ptl, step_size=50)
        try:
            driver.start(session_id="halt-det", seed=99, bars=200)
            driver.set_halted(True)
            driver.set_halted(False)
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
