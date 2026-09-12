"""Market data types.

Deliberately NOT the engine's `market::Quote`. These describe what a DISPLAY
needs — a top-of-book snapshot with a source and a staleness — and they never
cross into the engine. Reusing the engine's type here would suggest these values
are engine state, and they are not: they come from a provider the engine has
never seen.
"""

from __future__ import annotations

from typing import Literal

from pydantic import BaseModel, ConfigDict, Field


class Quote(BaseModel):
    """Top of book for one symbol.

    TOP OF BOOK ONLY, as the engine models it (ADR-0003). There is no depth
    here because there is no depth anywhere in this system, and a panel
    implying otherwise would be inventing data.
    """

    model_config = ConfigDict(
        json_schema_extra={
            "examples": [
                {
                    "symbol": "SPY",
                    "bid": 503.11,
                    "ask": 503.13,
                    "last": 503.12,
                    "volume": 1_240_000,
                    "ts": "2024-07-02T14:31:05Z",
                    "source": "replay",
                }
            ]
        }
    )

    symbol: str
    bid: float | None = None
    ask: float | None = None
    last: float | None = None
    volume: float | None = None
    ts: str | None = Field(default=None, description="Provider timestamp, UTC.")
    source: Literal["replay", "live"]

    @property
    def spread(self) -> float | None:
        # Derived on the server so every client agrees, and returns None rather
        # than 0.0 when a side is missing -- a zero spread is a real and very
        # different observation from an unknown one.
        if self.bid is None or self.ask is None:
            return None
        return self.ask - self.bid


class QuoteView(Quote):
    """A quote as sent to a client, with the spread resolved."""

    spread_value: float | None = None
    spread_bps: float | None = None


class MarketStatus(BaseModel):
    """Whether the feed is usable, and why not when it is not."""

    mode: Literal["replay", "live"]
    connected: bool
    provider: str
    detail: str = ""
    symbols: list[str] = []
    last_update: str | None = None


class Watchlist(BaseModel):
    symbols: list[str] = []
