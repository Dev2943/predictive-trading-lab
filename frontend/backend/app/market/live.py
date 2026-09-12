"""Alpaca live market data.

WHY ALPACA, chosen against the alternatives rather than by default:

  * **Alpaca** — already this project's venue. ADR-0001 selects it for
    historical data and F6's order translator targets it, so a live feed adds
    no new vendor, no second credential story and no second symbology to
    reconcile. Its free tier carries real-time IEX trades and quotes over a
    single WebSocket. Chosen.
  * **Finnhub** — the simplest WebSocket of the five and a genuine free
    real-time tier. Rejected only because it would introduce a second vendor
    for data while orders go to Alpaca, and two symbologies is a reconciliation
    problem nobody asked for.
  * **Polygon** — the best documentation and the most serious infrastructure,
    but real-time requires a paid tier; the free tier is delayed. Presenting
    delayed data on a panel labelled "live" is precisely the failure this
    project refuses elsewhere.
  * **Twelve Data** — broad non-US coverage, which this system does not need,
    and WebSocket access sits behind paid credits.
  * **Interactive Brokers** — requires a running TWS or Gateway process and a
    desktop session. That is an unreasonable dependency for a web gateway and
    would not survive a container restart.

NOT VERIFIED AGAINST A REAL ENDPOINT. Every market data host is unreachable
from this environment, so this client has been exercised against a local fake
that speaks Alpaca's message shapes and never against Alpaca itself. The
message parsing is written to their documented schema; the connection handling
is not proven. That is stated here rather than in a footnote because the
distinction matters to anyone about to rely on it.
"""

from __future__ import annotations

import json
import os
import threading
from typing import Any, Iterable

from .provider import MarketDataProvider, ProviderError
from .types import Quote

ALPACA_STREAM = "wss://stream.data.alpaca.markets/v2/iex"


class AlpacaLiveProvider(MarketDataProvider):
    """Streams top of book from Alpaca's IEX feed.

    Runs its own thread because a WebSocket read blocks and the gateway is
    synchronous. That thread touches NOTHING the engine owns: it writes only to
    this object's quote cache, which readers copy. The single-writer rule
    protects mutable *session* state, and there is none here.
    """

    name = "alpaca-iex"
    mode = "live"

    def __init__(
        self,
        key: str | None = None,
        secret: str | None = None,
        url: str = ALPACA_STREAM,
        connect: Any | None = None,
    ) -> None:
        # Credentials come from the environment, never from a request body or
        # the TOML. Their VALUES are never logged or echoed.
        self._key = key or os.environ.get("PTL_ALPACA_KEY", "")
        self._secret = secret or os.environ.get("PTL_ALPACA_SECRET", "")
        self._url = url
        # Injectable so the message handling can be tested without a network.
        self._connect = connect
        self._socket: Any | None = None
        self._thread: threading.Thread | None = None
        self._stop = threading.Event()
        self._symbols: list[str] = []
        self._quotes: dict[str, Quote] = {}
        self._connected = False
        self._detail = "not connected"

    # --- lifecycle ---------------------------------------------------------

    def start(self) -> None:
        if not self._key or not self._secret:
            # Refused rather than attempted: a connection without credentials
            # fails with an opaque auth error several seconds later.
            raise ProviderError(
                "PTL_ALPACA_KEY and PTL_ALPACA_SECRET are not set; live market "
                "data requires credentials"
            )
        if self._thread and self._thread.is_alive():
            return

        self._stop.clear()
        self._thread = threading.Thread(
            target=self._run, name="ptl-market-live", daemon=True
        )
        self._thread.start()

    def close(self) -> None:
        self._stop.set()
        socket = self._socket
        if socket is not None:
            try:
                socket.close()
            except Exception:  # noqa: BLE001 - closing a dead socket is fine
                pass
        if self._thread is not None:
            self._thread.join(timeout=3.0)
        self._connected = False
        self._detail = "closed"

    def connected(self) -> bool:
        return self._connected

    def detail(self) -> str:
        return self._detail

    # --- subscription ------------------------------------------------------

    def subscribe(self, symbols: Iterable[str]) -> None:
        added = [s for s in symbols if s not in self._symbols]
        self._symbols.extend(added)
        if added and self._socket is not None:
            self._send({"action": "subscribe", "quotes": added, "trades": added})

    def unsubscribe(self, symbols: Iterable[str]) -> None:
        removed = [s for s in symbols if s in self._symbols]
        for symbol in removed:
            self._symbols.remove(symbol)
            self._quotes.pop(symbol, None)
        if removed and self._socket is not None:
            self._send({"action": "unsubscribe", "quotes": removed, "trades": removed})

    def quotes(self) -> dict[str, Quote]:
        # A copy: the reader must not see the cache mutate mid-iteration while
        # the stream thread is writing to it.
        return dict(self._quotes)

    # --- stream ------------------------------------------------------------

    def _send(self, payload: dict[str, Any]) -> None:
        socket = self._socket
        if socket is None:
            return
        try:
            socket.send(json.dumps(payload))
        except Exception as exc:  # noqa: BLE001
            self._detail = f"send failed: {exc}"

    def _run(self) -> None:
        try:
            connect = self._connect
            if connect is None:  # pragma: no cover - needs a network
                from websockets.sync.client import connect as ws_connect

                connect = ws_connect
            self._socket = connect(self._url)
        except Exception as exc:  # noqa: BLE001
            self._connected = False
            self._detail = f"connect failed: {exc}"
            return

        self._send({"action": "auth", "key": self._key, "secret": self._secret})
        if self._symbols:
            self._send(
                {"action": "subscribe", "quotes": self._symbols, "trades": self._symbols}
            )

        while not self._stop.is_set():
            try:
                raw = self._socket.recv()
            except Exception as exc:  # noqa: BLE001
                self._connected = False
                self._detail = f"stream ended: {exc}"
                return
            self.handle(raw)

    def handle(self, raw: str | bytes) -> None:
        """Apply one frame. Public so it can be tested without a socket."""
        try:
            messages = json.loads(raw)
        except (TypeError, ValueError):
            # A frame we cannot parse is counted, never guessed at.
            self._detail = "received an unparseable frame"
            return
        if isinstance(messages, dict):
            messages = [messages]

        for message in messages:
            kind = message.get("T")
            if kind == "success" and message.get("msg") == "authenticated":
                self._connected = True
                self._detail = "authenticated"
            elif kind == "error":
                self._connected = False
                # Alpaca's own reason, which distinguishes bad credentials from
                # an unentitled feed -- two problems with different remedies.
                self._detail = f"alpaca error {message.get('code')}: {message.get('msg')}"
            elif kind == "q":
                self._apply_quote(message)
            elif kind == "t":
                self._apply_trade(message)

    def _apply_quote(self, message: dict[str, Any]) -> None:
        symbol = message.get("S")
        if not symbol:
            return
        previous = self._quotes.get(symbol)
        self._quotes[symbol] = Quote(
            symbol=symbol,
            bid=message.get("bp"),
            ask=message.get("ap"),
            # A quote frame carries no trade price; the last trade is retained
            # rather than overwritten with None, which would blank the column
            # on every quote update.
            last=previous.last if previous else None,
            volume=previous.volume if previous else None,
            ts=message.get("t"),
            source="live",
        )

    def _apply_trade(self, message: dict[str, Any]) -> None:
        symbol = message.get("S")
        if not symbol:
            return
        previous = self._quotes.get(symbol)
        self._quotes[symbol] = Quote(
            symbol=symbol,
            bid=previous.bid if previous else None,
            ask=previous.ask if previous else None,
            last=message.get("p"),
            volume=(previous.volume or 0.0) + float(message.get("s") or 0.0)
            if previous
            else float(message.get("s") or 0.0),
            ts=message.get("t"),
            source="live",
        )
