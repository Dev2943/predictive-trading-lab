"""Dependency wiring.

One place decides which EngineClient the application uses. Routes ask for the
protocol and get whatever this returns, so swapping in a remote engine later is
a change to this file alone.
"""

from __future__ import annotations

import functools
import os
from typing import Annotated

from fastapi import Depends, HTTPException, status

from .market import MarketDataService
from .engine import (
    EngineClient,
    EngineUnavailable,
    InProcessEngineClient,
    SessionDriver,
)


@functools.lru_cache(maxsize=1)
def _client() -> EngineClient:
    # Cached because constructing it imports the extension module. The client
    # holds no session state, so sharing one across requests is safe -- and
    # remains safe when a remote client replaces it.
    return InProcessEngineClient(artifact_root=os.environ.get("PTL_RESULTS", "results"))


def get_engine() -> EngineClient:
    try:
        return _client()
    except EngineUnavailable as exc:
        # 503, not 500: the gateway is fine and the engine is not reachable. A
        # frontend can distinguish "retry later" from "this request was wrong".
        raise HTTPException(
            status_code=status.HTTP_503_SERVICE_UNAVAILABLE, detail=str(exc)
        ) from exc


def probe_engine() -> EngineClient | None:
    """Reachability probe that returns None instead of raising.

    `/health` must answer 200 even when the engine is down -- it is the one
    endpoint that has to work when everything else does not. It therefore
    cannot depend on `get_engine`, which raises.

    This is a separate DEPENDENCY rather than a direct call inside the route,
    so a test can override it. A route that calls `get_engine()` directly
    bypasses FastAPI's injection entirely, which makes the unreachable-engine
    path unreachable in tests -- exactly the path most worth testing.
    """
    try:
        return _client()
    except EngineUnavailable:
        return None


Engine = Annotated[EngineClient, Depends(get_engine)]
MaybeEngine = Annotated[EngineClient | None, Depends(probe_engine)]


@functools.lru_cache(maxsize=1)
def _driver() -> SessionDriver:
    """The single session driver.

    One per process, enforced by the cache. The host it drives is itself a
    single C++ instance, so there is exactly one PaperSession in existence and
    no route can create a second.
    """
    import ptl

    return SessionDriver(ptl)


def get_driver() -> SessionDriver:
    try:
        return _driver()
    except (ImportError, EngineUnavailable) as exc:
        raise HTTPException(
            status_code=status.HTTP_503_SERVICE_UNAVAILABLE,
            detail=f"the session host is unavailable: {exc}",
        ) from exc


Driver = Annotated[SessionDriver, Depends(get_driver)]


@functools.lru_cache(maxsize=1)
def _market() -> MarketDataService:
    """The single market data service.

    One per process, so every client shares one watchlist and one provider
    connection. A service per request would open a WebSocket per browser tab.

    A live provider is constructed only when credentials are present. Without
    them the service offers replay and refuses `live` with a reason, rather
    than pretending a feed exists.
    """
    live = None
    if os.environ.get("PTL_ALPACA_KEY") and os.environ.get("PTL_ALPACA_SECRET"):
        from .market.live import AlpacaLiveProvider

        live = AlpacaLiveProvider()
    return MarketDataService(live=live)


def get_market() -> MarketDataService:
    return _market()


Market = Annotated[MarketDataService, Depends(get_market)]


def shutdown_services() -> None:
    """Release background resources on shutdown.

    Called from the application lifespan. Without it, a container restart
    leaves the session driver thread stepping a session nobody is reading and
    the market data socket open until the process is killed.

    Each is guarded separately: a failure closing one must not prevent the
    others from closing.
    """
    for close in (
        lambda: _driver.cache_info() and _driver().shutdown(),
        lambda: _market.cache_info() and _market().close(),
    ):
        try:
            close()
        except Exception:  # noqa: BLE001 - shutdown is best-effort by nature
            pass
