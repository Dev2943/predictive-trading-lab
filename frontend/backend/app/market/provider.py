"""The market data provider boundary.

THE ENGINE NEVER SEES ANY OF THIS.

A provider produces quotes for display. It does not feed the engine, and P9
deliberately does not plumb it into one -- see `docs/09-live-market-data.md` for
why. The abstraction exists so the interface can show replay or live prices
without either knowing which it is looking at.
"""

from __future__ import annotations

import abc
import math
from typing import Iterable

from .types import Quote


class ProviderError(RuntimeError):
    """The provider could not deliver data.

    Distinct from "the market is closed" or "no such symbol": this means the
    feed itself is unusable, which is what a status panel must report.
    """


class MarketDataProvider(abc.ABC):
    """Produces top-of-book quotes for a set of symbols."""

    name: str = "provider"
    mode: str = "replay"

    @abc.abstractmethod
    def subscribe(self, symbols: Iterable[str]) -> None: ...

    @abc.abstractmethod
    def unsubscribe(self, symbols: Iterable[str]) -> None: ...

    @abc.abstractmethod
    def quotes(self) -> dict[str, Quote]:
        """Current top of book for every subscribed symbol."""

    @abc.abstractmethod
    def connected(self) -> bool: ...

    def detail(self) -> str:
        return ""

    def close(self) -> None:  # pragma: no cover - default is a no-op
        return None


class ReplayProvider(MarketDataProvider):
    """The deterministic reference implementation.

    Prices are a pure function of (symbol, seed, tick index), so the same seed
    produces the same series on every run and on every machine. That is what
    makes this the reference: a client rendering replay quotes can be compared
    byte-for-byte between runs, which no live feed permits.

    It is NOT the engine's replay. The engine has its own bar series inside the
    session host; this one drives the watchlist. Sharing them would mean the
    display advancing the engine's clock, which is exactly the coupling the
    architecture forbids.
    """

    name = "synthetic-replay"
    mode = "replay"

    def __init__(self, seed: int = 20240101) -> None:
        self._seed = seed
        self._symbols: list[str] = []
        self._tick = 0

    def subscribe(self, symbols: Iterable[str]) -> None:
        for symbol in symbols:
            if symbol not in self._symbols:
                self._symbols.append(symbol)

    def unsubscribe(self, symbols: Iterable[str]) -> None:
        for symbol in symbols:
            if symbol in self._symbols:
                self._symbols.remove(symbol)

    def connected(self) -> bool:
        return True

    def detail(self) -> str:
        return "deterministic synthetic series; not market data"

    def advance(self, ticks: int = 1) -> None:
        self._tick += ticks

    def quotes(self) -> dict[str, Quote]:
        out: dict[str, Quote] = {}
        for symbol in self._symbols:
            # Symbol-dependent phase so two symbols do not move in lockstep,
            # which would look like a rendering bug rather than a market.
            phase = sum(ord(c) for c in symbol) % 97
            base = 100.0 + phase
            drift = math.sin((self._tick + phase) * 0.07) * (base * 0.004)
            mid = base + drift
            half = max(0.01, base * 0.00002)
            out[symbol] = Quote(
                symbol=symbol,
                bid=round(mid - half, 4),
                ask=round(mid + half, 4),
                last=round(mid, 4),
                volume=float((self._tick + phase) * 137 % 1_000_000),
                ts=None,
                source="replay",
            )
        return out
