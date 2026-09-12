"""Market data endpoints.

WEBSOCKET *AND* REST, and the split is deliberate.

Ticks are push-shaped and bursty. Polling a twenty-symbol watchlist at one
second is twenty requests a second that mostly return unchanged rows, and at
two seconds it misses the moves it exists to show. Earlier phases avoided
WebSockets because session state is slow and a visible staleness was a feature;
a bid that is two seconds old is a different thing, so this is the one place the
transport is justified.

REST remains for the cases a stream serves badly: the initial render, a client
that cannot hold a socket, and every mutation. `GET /market/quotes` returns the
same rows the stream sends, so a client can start from REST and upgrade.
"""

from __future__ import annotations

import asyncio

from fastapi import APIRouter, HTTPException, WebSocket, WebSocketDisconnect

from ..dependencies import Market
from ..market import ProviderError
from ..models.schemas import ErrorDetail
from ..market.types import MarketStatus, QuoteView, Watchlist

router = APIRouter(prefix="/market", tags=["market"])

_REFUSED = {422: {"model": ErrorDetail, "description": "The provider refused, and says why."}}


@router.get(
    "/status",
    response_model=MarketStatus,
    summary="Feed mode, connection and watchlist",
    description=(
        "`connected` is the provider's own view. In replay it is always true "
        "and `provider` reads `synthetic-replay`, so a client can never "
        "present generated prices as a market."
    ),
)
def status(market: Market) -> MarketStatus:
    return market.status()


@router.get(
    "/quotes",
    response_model=list[QuoteView],
    summary="Current top of book for the watchlist",
    description=(
        "Top of book only — the system models no depth (ADR-0003), so there is "
        "none to return.\n\n"
        "A subscribed symbol that has produced nothing appears as an empty row "
        "rather than being omitted: the watchlist should show what was asked "
        "for, including which entries are silent."
    ),
)
def quotes(market: Market) -> list[QuoteView]:
    return market.quotes()


@router.post(
    "/mode/{mode}",
    response_model=MarketStatus,
    responses=_REFUSED,
    summary="Select replay or live market data",
    description=(
        "Switching to `live` without a configured provider is **refused**, not "
        "silently downgraded. A panel labelled LIVE showing synthetic prices "
        "would be the most misleading thing this interface could do.\n\n"
        "This changes only what the watchlist displays. The paper session "
        "continues on its own deterministic replay either way."
    ),
)
def set_mode(mode: str, market: Market) -> MarketStatus:
    try:
        market.set_mode(mode)
    except ProviderError as exc:
        raise HTTPException(status_code=422, detail=str(exc)) from exc
    return market.status()


@router.post(
    "/watchlist",
    response_model=MarketStatus,
    responses=_REFUSED,
    summary="Replace the watchlist",
    description=(
        "Symbols are upper-cased and de-duplicated. A symbol containing "
        "punctuation is refused rather than stripped: rewriting it would hide "
        "whether it was a typo or something worse."
    ),
)
def watchlist(request: Watchlist, market: Market) -> MarketStatus:
    try:
        market.set_watchlist(request.symbols)
    except ProviderError as exc:
        raise HTTPException(status_code=422, detail=str(exc)) from exc
    return market.status()


@router.websocket("/stream")
async def stream(websocket: WebSocket) -> None:
    """Stream the watchlist.

    Sends the same `QuoteView` rows `GET /market/quotes` returns, so a client
    renders from REST and upgrades without a second code path.

    In replay the series is advanced here, once per frame, which keeps the
    displayed series a pure function of the number of frames sent — the same
    seed produces the same sequence for every client. In live mode nothing is
    advanced; the provider's own thread supplies the data.
    """
    from ..dependencies import get_market

    await websocket.accept()
    market = get_market()

    try:
        while True:
            market.advance(1)
            await websocket.send_json(
                {
                    "status": market.status().model_dump(),
                    "quotes": [q.model_dump() for q in market.quotes()],
                }
            )
            # A fixed cadence rather than per-tick push: the provider caches top
            # of book, so a faster stream would resend unchanged rows, and a
            # slower one would make the spread column visibly stale.
            await asyncio.sleep(1.0)
    except WebSocketDisconnect:
        # The normal way a client leaves. Not an error, and not logged as one.
        return
