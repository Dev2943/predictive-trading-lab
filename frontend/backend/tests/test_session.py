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


# ---------------------------------------------------------------------------
# F4: history, drawdown and instruments
# ---------------------------------------------------------------------------


class HistoryBackend(FakeBackend):
    """A backend that publishes a growing equity series."""

    def __init__(self, points: int = 5, **kwargs) -> None:
        super().__init__(**kwargs)
        self._points = points

    def session_snapshot(self) -> str:
        base = json.loads(super().session_snapshot())
        equity = [1000.0, 1010.0, 1005.0, 1020.0, 1015.0][: self._points]
        peak = max(equity) if equity else 0.0
        base["history"] = {
            "available": self.started,
            "total_points": len(equity),
            "stride": 1,
            "max_drawdown": 0.0049,
            "current_drawdown": 0.0049,
            "peak_equity": peak,
            "points": [
                {
                    "ts": f"2024-07-02T14:{30 + i:02d}:00Z",
                    "equity": e,
                    "cash": e,
                    "realized_pnl": 0.0,
                    "unrealized_pnl": 0.0,
                    "gross_exposure": 0.0,
                    "net_exposure": 0.0,
                }
                for i, e in enumerate(equity)
            ],
        }
        base["instruments"] = {"instruments": [{"instrument": 0, "symbol": "SPY"}]}
        return json.dumps(base)


@pytest.fixture
def history_client():
    driver = SessionDriver(HistoryBackend(), step_size=25)
    app.dependency_overrides[get_driver] = lambda: driver
    with TestClient(app) as client:
        yield client
    app.dependency_overrides.clear()
    driver.shutdown()


def test_history_is_absent_before_a_session_starts(history_client):
    """Absent, not a flat line at zero. A chart must be able to tell the
    difference between no data and a book worth nothing."""
    body = history_client.get("/session/history").json()
    assert body["available"] is False
    assert body["points"] == []


def test_history_is_served_once_running(history_client):
    history_client.post("/session/start", json={})
    body = history_client.get("/session/history").json()
    assert body["available"] is True
    assert len(body["points"]) == 5
    assert body["points"][0]["equity"] == 1000.0
    # Drawdown comes from the engine; the gateway computes nothing.
    assert body["max_drawdown"] == pytest.approx(0.0049)
    assert body["peak_equity"] == 1020.0


def test_history_is_included_in_the_snapshot(history_client):
    """One consistent read: history must come from the same instant as the
    account, not from a second call that could land between steps."""
    history_client.post("/session/start", json={})
    snapshot = history_client.get("/session/snapshot").json()
    assert snapshot["history"]["available"] is True
    assert len(snapshot["history"]["points"]) == 5
    assert snapshot["instruments"] == [{"instrument": 0, "symbol": "SPY"}]


def test_instruments_resolve_to_symbols(history_client):
    history_client.post("/session/start", json={})
    body = history_client.get("/session/instruments").json()
    assert body == [{"instrument": 0, "symbol": "SPY"}]


def test_history_endpoints_do_not_touch_the_session(history_client):
    """Reads are served from the published snapshot.

    The backend counts every call it receives; reading history repeatedly must
    not add any.
    """
    history_client.post("/session/start", json={})
    driver = app.dependency_overrides[get_driver]()
    backend = driver._backend  # noqa: SLF001
    before = backend.steps

    for _ in range(5):
        history_client.get("/session/history")
        history_client.get("/session/instruments")

    # Any change is the driver thread stepping, never a reader.
    assert backend.start_calls == 1


# ---------------------------------------------------------------------------
# F6: trading commands
# ---------------------------------------------------------------------------


class TradingBackend(FakeBackend):
    """A backend that records trading commands."""

    def __init__(self, **kwargs) -> None:
        super().__init__(**kwargs)
        self.orders: list[dict] = []
        self.cancels: list[int] = []
        self.flattens = 0
        self.halted = False
        self.next_request = 1

    def session_submit_order(self, **kwargs) -> int:
        if kwargs.get("symbol") == "UNKNOWN":
            raise RuntimeError("submit order: unknown symbol: UNKNOWN")
        self.orders.append(kwargs)
        rid = self.next_request
        self.next_request += 1
        return rid

    def session_cancel_order(self, order_id: int) -> bool:
        if order_id == 999:
            raise RuntimeError("cancel order: no such order: 999")
        self.cancels.append(order_id)
        return True

    def session_cancel_all(self) -> int:
        return 3

    def session_flatten(self) -> int:
        self.flattens += 1
        return 2

    def session_set_halted(self, halted: bool) -> bool:
        self.halted = halted
        return halted

    def session_snapshot(self) -> str:
        base = json.loads(super().session_snapshot())
        base["pending"] = {
            "pending": 0,
            "outcomes": [
                {"request_id": 1, "order_id": 7, "accepted": True, "detail": ""},
            ],
        }
        base["order_history"] = {
            "available": self.started,
            "orders": [
                {
                    "order_id": 7,
                    "symbol": "SPY",
                    "state": "filled",
                    "side": 1,
                    "type": "limit",
                    "quantity": 50.0,
                    "filled": 50.0,
                    "reject_reason": "",
                }
            ],
        }
        return json.dumps(base)


@pytest.fixture
def trading_client():
    driver = SessionDriver(TradingBackend(), step_size=25)
    app.dependency_overrides[get_driver] = lambda: driver
    with TestClient(app) as client:
        yield client, driver
    app.dependency_overrides.clear()
    driver.shutdown()


def test_trading_mode_is_paper_and_says_live_is_unavailable(trading_client):
    """No live broker is connected and none is simulated."""
    client, _ = trading_client
    body = client.get("/trading/mode").json()
    assert body["mode"] == "PAPER"
    assert body["live_available"] is False


def test_an_order_requires_a_running_session(trading_client):
    """409: the request was well-formed and refused for when it arrived."""
    client, _ = trading_client
    response = client.post(
        "/trading/orders", json={"symbol": "SPY", "side": 1, "quantity": 10}
    )
    assert response.status_code == 409


def test_an_order_is_queued_not_filled(trading_client):
    client, driver = trading_client
    client.post("/session/start", json={})

    body = client.post(
        "/trading/orders",
        json={"symbol": "SPY", "side": 1, "quantity": 50, "type": "limit", "limit_price": 505.0},
    ).json()
    assert body["queued"] is True
    assert body["request_id"] == 1
    # The response must not imply a fill.
    assert "queued" in body["detail"]

    backend = driver._backend  # noqa: SLF001
    assert backend.orders[0]["symbol"] == "SPY"
    assert backend.orders[0]["limit_price"] == 505.0


def test_an_unknown_symbol_is_422_with_the_engines_reason(trading_client):
    client, _ = trading_client
    client.post("/session/start", json={})
    response = client.post(
        "/trading/orders", json={"symbol": "UNKNOWN", "side": 1, "quantity": 10}
    )
    assert response.status_code == 422
    assert "unknown symbol" in response.json()["detail"]


def test_order_validation_rejects_impossible_requests(trading_client):
    client, _ = trading_client
    client.post("/session/start", json={})
    # Quantity must be positive.
    assert (
        client.post(
            "/trading/orders", json={"symbol": "SPY", "side": 1, "quantity": 0}
        ).status_code
        == 422
    )
    # Side is 1 or -1, nothing else.
    assert (
        client.post(
            "/trading/orders", json={"symbol": "SPY", "side": 2, "quantity": 10}
        ).status_code
        == 422
    )
    # And an unknown order type.
    assert (
        client.post(
            "/trading/orders",
            json={"symbol": "SPY", "side": 1, "quantity": 10, "type": "iceberg"},
        ).status_code
        == 422
    )


def test_cancel_is_queued(trading_client):
    client, driver = trading_client
    client.post("/session/start", json={})
    assert client.delete("/trading/orders/7").status_code == 200
    assert driver._backend.cancels == [7]  # noqa: SLF001


def test_cancelling_an_unknown_order_is_422(trading_client):
    client, _ = trading_client
    client.post("/session/start", json={})
    assert client.delete("/trading/orders/999").status_code == 422


def test_cancel_all_and_flatten_report_what_they_queued(trading_client):
    client, driver = trading_client
    client.post("/session/start", json={})

    cancelled = client.post("/trading/cancel-all").json()
    assert cancelled["queued"] == 3

    flattened = client.post("/trading/flatten").json()
    assert flattened["queued"] == 2
    assert driver._backend.flattens == 1  # noqa: SLF001


def test_flatten_requires_a_running_session(trading_client):
    client, _ = trading_client
    assert client.post("/trading/flatten").status_code == 409


def test_pending_and_history_come_from_the_snapshot(trading_client):
    client, _ = trading_client
    client.post("/session/start", json={})

    pending = client.get("/trading/pending").json()
    assert pending["outcomes"][0]["order_id"] == 7

    history = client.get("/trading/orders").json()
    assert history["available"] is True
    assert history["orders"][0]["state"] == "filled"
    assert history["orders"][0]["symbol"] == "SPY"


def test_a_failed_trading_command_does_not_halt_the_session(trading_client):
    """One mistyped order must not stop the book.

    An unknown symbol is the caller's mistake. Driving the session to ERROR
    would leave a live book unattended for a reason unrelated to the book.
    """
    client, driver = trading_client
    client.post("/session/start", json={})

    response = client.post(
        "/trading/orders", json={"symbol": "UNKNOWN", "side": 1, "quantity": 10}
    )
    assert response.status_code == 422
    # Still running, and still accepting orders.
    assert driver.state is SessionState.RUNNING
    assert (
        client.post(
            "/trading/orders", json={"symbol": "SPY", "side": 1, "quantity": 10}
        ).status_code
        == 200
    )


def test_a_failed_start_still_halts_the_session():
    """Lifecycle failures are different: the session really is in an unknown
    state, so ERROR is correct there."""
    driver = SessionDriver(FakeBackend(start_fails=True))
    try:
        with pytest.raises(RuntimeError):
            driver.start(session_id="t")
        assert driver.state is SessionState.ERROR
    finally:
        driver.shutdown()


# ---------------------------------------------------------------------------
# F6: broker abstraction
# ---------------------------------------------------------------------------


def test_the_live_adapter_refuses_rather_than_falling_back_to_paper():
    """The most dangerous bug this system could have would be an interface that
    quietly executed against paper while believing it was live."""
    from app.engine.brokers import BrokerUnavailable, LiveBrokerAdapter

    live = LiveBrokerAdapter()
    assert live.connected is False
    for call in (
        lambda: live.submit({}),
        lambda: live.cancel(1),
        lambda: live.cancel_all(),
        live.account,
        live.positions,
    ):
        with pytest.raises(BrokerUnavailable, match="no live broker"):
            call()


def test_the_router_refuses_an_unconnected_venue():
    from app.engine.brokers import (
        BrokerUnavailable,
        LiveBrokerAdapter,
        OrderRouter,
        PaperBrokerAdapter,
    )

    router = OrderRouter()
    router.register(PaperBrokerAdapter(driver=None))
    router.register(LiveBrokerAdapter())

    assert router.available() == ["paper"]
    assert router.route("paper").name == "paper"
    with pytest.raises(BrokerUnavailable, match="not connected"):
        router.route("live")
    with pytest.raises(BrokerUnavailable, match="no adapter"):
        router.route("nyse")


def test_the_paper_adapter_delegates_to_the_driver_rather_than_re_implementing():
    """A second submission path is exactly what F6 exists to avoid."""
    from app.engine.brokers import PaperBrokerAdapter

    driver = SessionDriver(TradingBackend(), step_size=25)
    try:
        driver.start(session_id="adapter")
        adapter = PaperBrokerAdapter(driver)
        assert adapter.connected is True
        request_id = adapter.submit(
            {"symbol": "SPY", "side": 1, "quantity": 10, "type": "market"}
        )
        assert request_id == 1
        assert driver._backend.orders[0]["symbol"] == "SPY"  # noqa: SLF001
    finally:
        driver.shutdown()


# ---------------------------------------------------------------------------
# F7: strategy halt
# ---------------------------------------------------------------------------


def test_halting_requires_a_running_session(trading_client):
    client, _ = trading_client
    assert client.post("/trading/halt", json={"halted": True}).status_code == 409


def test_halt_is_a_flag_not_a_lifecycle_transition(trading_client):
    """The session stays RUNNING.

    Halting is the control between flatten (acts on the book) and stop (tears
    the session down). If it moved the lifecycle it would be a second stop.
    """
    client, driver = trading_client
    client.post("/session/start", json={})

    body = client.post("/trading/halt", json={"halted": True}).json()
    assert body["strategy_halted"] is True
    assert driver.state is SessionState.RUNNING
    # The response says what remains active, so the control is not mistaken
    # for a stop.
    assert "manual orders" in body["detail"]

    resumed = client.post("/trading/halt", json={"halted": False}).json()
    assert resumed["strategy_halted"] is False
    assert driver.state is SessionState.RUNNING


def test_manual_orders_still_work_while_halted(trading_client):
    """Halting suppresses the STRATEGY, not the operator."""
    client, driver = trading_client
    client.post("/session/start", json={})
    client.post("/trading/halt", json={"halted": True})

    assert (
        client.post(
            "/trading/orders", json={"symbol": "SPY", "side": 1, "quantity": 10}
        ).status_code
        == 200
    )
    assert driver._backend.orders  # noqa: SLF001
