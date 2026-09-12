"""Market data tests.

The live provider is exercised against a FAKE that speaks Alpaca's documented
message shapes. Every market data host is unreachable from this environment, so
the parsing is verified and the connection handling is not — a distinction the
report states plainly rather than glossing.
"""

from __future__ import annotations

import json

import pytest
from fastapi.testclient import TestClient

from app.dependencies import get_market
from app.main import app
from app.market import MarketDataService, ProviderError, ReplayProvider
from app.market.live import AlpacaLiveProvider


@pytest.fixture
def service() -> MarketDataService:
    return MarketDataService()


@pytest.fixture
def client(service: MarketDataService):
    app.dependency_overrides[get_market] = lambda: service
    with TestClient(app) as test_client:
        yield test_client
    app.dependency_overrides.clear()


# ---------------------------------------------------------------------------
# Determinism: the replay provider is the reference implementation
# ---------------------------------------------------------------------------


def test_the_replay_series_is_deterministic():
    """THE LOAD-BEARING TEST.

    Two providers on the same seed, advanced the same number of ticks, must
    produce identical quotes. Without this the watchlist could not be compared
    between runs and 'replay' would mean nothing.
    """
    a, b = ReplayProvider(seed=42), ReplayProvider(seed=42)
    a.subscribe(["SPY", "AAPL"])
    b.subscribe(["SPY", "AAPL"])
    for _ in range(25):
        a.advance()
        b.advance()
    assert a.quotes() == b.quotes()


def test_two_symbols_do_not_move_in_lockstep():
    """Identical series across symbols would look like a rendering bug."""
    provider = ReplayProvider()
    provider.subscribe(["SPY", "AAPL"])
    provider.advance(10)
    quotes = provider.quotes()
    assert quotes["SPY"].last != quotes["AAPL"].last


def test_replay_never_claims_to_be_a_market():
    provider = ReplayProvider()
    assert provider.mode == "replay"
    assert provider.name == "synthetic-replay"
    assert "not market data" in provider.detail()
    provider.subscribe(["SPY"])
    assert provider.quotes()["SPY"].source == "replay"


# ---------------------------------------------------------------------------
# The engine must not be touched
# ---------------------------------------------------------------------------


def test_market_data_never_reaches_the_engine(service: MarketDataService):
    """P9's central claim.

    The service holds providers and a watchlist. It has no session, no driver
    and no binding handle, so there is no path from a price to the engine.
    """
    forbidden = ("session", "driver", "engine", "ptl")
    for attribute in dir(service):
        assert not any(f in attribute.lower() for f in forbidden), attribute


# ---------------------------------------------------------------------------
# Mode selection
# ---------------------------------------------------------------------------


def test_live_is_refused_when_no_provider_is_configured(service: MarketDataService):
    """Refused, never silently downgraded.

    A panel labelled LIVE showing synthetic prices is the most misleading thing
    this interface could do.
    """
    with pytest.raises(ProviderError, match="no live provider"):
        service.set_mode("live")
    assert service.mode == "replay"


def test_an_unknown_mode_is_refused(service: MarketDataService):
    with pytest.raises(ProviderError, match="unknown market data mode"):
        service.set_mode("delayed")


def test_switching_to_live_over_http_is_422(client):
    response = client.post("/market/mode/live")
    assert response.status_code == 422
    assert "PTL_ALPACA_KEY" in response.json()["detail"]


# ---------------------------------------------------------------------------
# Watchlist
# ---------------------------------------------------------------------------


def test_the_watchlist_is_normalised(service: MarketDataService):
    assert service.set_watchlist([" spy ", "aapl", "SPY"]) == ["SPY", "AAPL"]


def test_an_invalid_symbol_is_refused_not_stripped(service: MarketDataService):
    """Rewriting it would hide whether it was a typo or something worse."""
    with pytest.raises(ProviderError, match="invalid symbol"):
        service.set_watchlist(["SPY", "AA;PL"])


def test_an_empty_watchlist_is_refused(service: MarketDataService):
    with pytest.raises(ProviderError, match="cannot be empty"):
        service.set_watchlist([])


def test_a_silent_symbol_appears_as_an_empty_row(service: MarketDataService):
    """The watchlist shows what was asked for, including what is silent."""
    service.set_watchlist(["SPY"])
    service._replay.unsubscribe(["SPY"])  # noqa: SLF001 - simulating no data
    rows = service.quotes()
    assert len(rows) == 1
    assert rows[0].symbol == "SPY"
    assert rows[0].bid is None


# ---------------------------------------------------------------------------
# Spread
# ---------------------------------------------------------------------------


def test_spread_is_none_when_a_side_is_missing(service: MarketDataService):
    """A zero spread is a real observation and a very different one."""
    service.set_watchlist(["SPY"])
    service._replay.unsubscribe(["SPY"])  # noqa: SLF001
    assert service.quotes()[0].spread_value is None


def test_spread_is_computed_on_the_server(service: MarketDataService):
    row = service.quotes()[0]
    assert row.spread_value == pytest.approx((row.ask or 0) - (row.bid or 0))
    assert row.spread_bps is not None


# ---------------------------------------------------------------------------
# REST
# ---------------------------------------------------------------------------


def test_status_and_quotes(client):
    status = client.get("/market/status").json()
    assert status["mode"] == "replay"
    assert status["provider"] == "synthetic-replay"
    assert status["connected"] is True

    quotes = client.get("/market/quotes").json()
    assert len(quotes) == len(status["symbols"])
    assert quotes[0]["source"] == "replay"


def test_the_watchlist_can_be_replaced_over_http(client):
    body = client.post("/market/watchlist", json={"symbols": ["tsla", "nvda"]}).json()
    assert body["symbols"] == ["TSLA", "NVDA"]
    assert client.post("/market/watchlist", json={"symbols": []}).status_code == 422


# ---------------------------------------------------------------------------
# WebSocket
# ---------------------------------------------------------------------------


def test_the_stream_sends_status_and_quotes(client):
    with client.websocket_connect("/market/stream") as ws:
        frame = ws.receive_json()
    assert frame["status"]["mode"] == "replay"
    assert len(frame["quotes"]) >= 1
    # Identical shape to the REST rows, so a client upgrades without a second
    # code path.
    assert set(frame["quotes"][0]) >= {"symbol", "bid", "ask", "spread_value"}


def test_the_stream_advances_the_replay_series(client):
    with client.websocket_connect("/market/stream") as ws:
        first = ws.receive_json()
        second = ws.receive_json()
    assert first["quotes"][0]["last"] != second["quotes"][0]["last"]


# ---------------------------------------------------------------------------
# Live provider, against a fake socket
# ---------------------------------------------------------------------------


class FakeSocket:
    def __init__(self) -> None:
        self.sent: list[dict] = []

    def send(self, payload: str) -> None:
        self.sent.append(json.loads(payload))

    def close(self) -> None:
        return None


def test_live_requires_credentials():
    provider = AlpacaLiveProvider(key="", secret="")
    with pytest.raises(ProviderError, match="PTL_ALPACA_KEY"):
        provider.start()


def test_authentication_success_marks_connected():
    provider = AlpacaLiveProvider(key="k", secret="s")
    provider.handle(json.dumps([{"T": "success", "msg": "authenticated"}]))
    assert provider.connected() is True


def test_an_alpaca_error_is_surfaced_with_its_reason():
    """Bad credentials and an unentitled feed are different problems with
    different remedies, and the code distinguishes them."""
    provider = AlpacaLiveProvider(key="k", secret="s")
    provider.handle(json.dumps([{"T": "error", "code": 402, "msg": "auth failed"}]))
    assert provider.connected() is False
    assert "402" in provider.detail()
    assert "auth failed" in provider.detail()


def test_a_quote_frame_populates_bid_and_ask():
    provider = AlpacaLiveProvider(key="k", secret="s")
    provider.handle(
        json.dumps([{"T": "q", "S": "SPY", "bp": 503.11, "ap": 503.13, "t": "2024-07-02T14:31:05Z"}])
    )
    quote = provider.quotes()["SPY"]
    assert quote.bid == 503.11
    assert quote.ask == 503.13
    assert quote.source == "live"
    assert quote.spread == pytest.approx(0.02)


def test_a_quote_frame_does_not_blank_the_last_trade():
    """A quote carries no trade price. Overwriting `last` with None would blank
    the column on every quote update."""
    provider = AlpacaLiveProvider(key="k", secret="s")
    provider.handle(json.dumps([{"T": "t", "S": "SPY", "p": 503.12, "s": 100}]))
    provider.handle(json.dumps([{"T": "q", "S": "SPY", "bp": 503.11, "ap": 503.13}]))
    assert provider.quotes()["SPY"].last == 503.12


def test_trade_frames_accumulate_volume():
    provider = AlpacaLiveProvider(key="k", secret="s")
    provider.handle(json.dumps([{"T": "t", "S": "SPY", "p": 1.0, "s": 100}]))
    provider.handle(json.dumps([{"T": "t", "S": "SPY", "p": 1.0, "s": 50}]))
    assert provider.quotes()["SPY"].volume == 150.0


def test_an_unparseable_frame_is_reported_not_guessed():
    provider = AlpacaLiveProvider(key="k", secret="s")
    provider.handle("not json at all")
    assert "unparseable" in provider.detail()
    assert provider.quotes() == {}


def test_subscription_messages_match_alpacas_protocol():
    provider = AlpacaLiveProvider(key="k", secret="s")
    socket = FakeSocket()
    provider._socket = socket  # noqa: SLF001 - standing in for a connection
    provider.subscribe(["SPY", "QQQ"])
    assert socket.sent[0]["action"] == "subscribe"
    assert socket.sent[0]["quotes"] == ["SPY", "QQQ"]

    provider.unsubscribe(["QQQ"])
    assert socket.sent[1]["action"] == "unsubscribe"
    assert socket.sent[1]["quotes"] == ["QQQ"]


def test_credentials_never_appear_in_the_provider_detail():
    """A masked credential in a log is still a credential in a log."""
    provider = AlpacaLiveProvider(key="SECRET-KEY", secret="SECRET-VALUE")
    provider.handle(json.dumps([{"T": "error", "code": 401, "msg": "unauthorized"}]))
    assert "SECRET-KEY" not in provider.detail()
    assert "SECRET-VALUE" not in provider.detail()
