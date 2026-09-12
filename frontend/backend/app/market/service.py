"""MarketDataService: one place that owns providers and the watchlist.

WHY THIS IS SEPARATE FROM THE SESSION DRIVER.

The SessionDriver exists to serialise writes to mutable engine state. Market
data has none: a provider produces quotes for display and never touches a
session. Routing it through the driver would put a network stream on the same
thread that steps the engine, so a slow feed would stall trading -- the precise
failure the single-writer design was built to prevent.

So the service is independent, and the engine remains unaware that live data
exists at all.
"""

from __future__ import annotations

from datetime import datetime, timezone

from .provider import MarketDataProvider, ProviderError, ReplayProvider
from .types import MarketStatus, QuoteView

DEFAULT_WATCHLIST = ["SPY", "QQQ", "AAPL", "MSFT"]


class MarketDataService:
    """Owns the active provider, the watchlist and the last-seen quotes."""

    def __init__(
        self,
        replay: MarketDataProvider | None = None,
        live: MarketDataProvider | None = None,
    ) -> None:
        self._replay = replay or ReplayProvider()
        self._live = live
        self._mode = "replay"
        self._symbols: list[str] = list(DEFAULT_WATCHLIST)
        self._last_update: str | None = None
        self._replay.subscribe(self._symbols)

    # --- mode --------------------------------------------------------------

    @property
    def mode(self) -> str:
        return self._mode

    def provider(self) -> MarketDataProvider:
        if self._mode == "live" and self._live is not None:
            return self._live
        return self._replay

    def set_mode(self, mode: str) -> str:
        """Switch source. Refuses `live` when no live provider is configured.

        Silently falling back to replay would leave a panel labelled LIVE
        showing synthetic prices, which is the single most misleading thing
        this interface could do.
        """
        if mode not in ("replay", "live"):
            raise ProviderError(f"unknown market data mode: {mode}")
        if mode == "live":
            if self._live is None:
                raise ProviderError(
                    "no live provider is configured; set PTL_ALPACA_KEY and "
                    "PTL_ALPACA_SECRET and restart the gateway"
                )
            start = getattr(self._live, "start", None)
            if start is not None:
                start()
            self._live.subscribe(self._symbols)
        self._mode = mode
        return self._mode

    # --- watchlist ---------------------------------------------------------

    @property
    def symbols(self) -> list[str]:
        return list(self._symbols)

    def set_watchlist(self, symbols: list[str]) -> list[str]:
        cleaned: list[str] = []
        for raw in symbols:
            symbol = raw.strip().upper()
            if not symbol:
                continue
            if not symbol.isalnum():
                # Refused, not sanitised: a symbol with punctuation is a typo or
                # an injection attempt, and rewriting it hides which.
                raise ProviderError(f"invalid symbol: {raw!r}")
            if symbol not in cleaned:
                cleaned.append(symbol)
        if not cleaned:
            raise ProviderError("the watchlist cannot be empty")

        removed = [s for s in self._symbols if s not in cleaned]
        added = [s for s in cleaned if s not in self._symbols]
        provider = self.provider()
        if removed:
            provider.unsubscribe(removed)
        if added:
            provider.subscribe(added)
        self._symbols = cleaned
        return self.symbols

    # --- reads -------------------------------------------------------------

    def quotes(self) -> list[QuoteView]:
        provider = self.provider()
        current = provider.quotes()
        self._last_update = datetime.now(timezone.utc).isoformat()

        views: list[QuoteView] = []
        for symbol in self._symbols:
            quote = current.get(symbol)
            if quote is None:
                # The symbol is subscribed but nothing has arrived. Reported as
                # an empty row rather than omitted, so the watchlist shows what
                # was asked for and which entries are silent.
                views.append(QuoteView(symbol=symbol, source=provider.mode))
                continue
            spread = quote.spread
            views.append(
                QuoteView(
                    **quote.model_dump(),
                    spread_value=spread,
                    spread_bps=(
                        (spread / quote.last * 10_000)
                        if spread is not None and quote.last
                        else None
                    ),
                )
            )
        return views

    def status(self) -> MarketStatus:
        provider = self.provider()
        return MarketStatus(
            mode=self._mode,
            connected=provider.connected(),
            provider=provider.name,
            detail=provider.detail(),
            symbols=self.symbols,
            last_update=self._last_update,
        )

    def advance(self, ticks: int = 1) -> None:
        """Advance the replay series. No effect in live mode."""
        if isinstance(self._replay, ReplayProvider) and self._mode == "replay":
            self._replay.advance(ticks)

    def close(self) -> None:
        if self._live is not None:
            self._live.close()
