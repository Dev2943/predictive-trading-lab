"""Paper session lifecycle.

The only endpoints in the gateway that change anything. They do so by ENQUEUING
A COMMAND, never by touching the session: a request handler that called into the
engine directly would break the single-writer guarantee the whole design rests
on.

Illegal transitions are 409, not 400. The request was well-formed and
understood; it was refused because of *when* it arrived, and a client should
retry after the state changes rather than fix its payload.
"""

from __future__ import annotations

from fastapi import APIRouter, HTTPException, status

from ..dependencies import Driver
from ..engine import IllegalTransition
from ..models.schemas import (
    ErrorDetail,
    SessionActionResponse,
    SessionSnapshot,
    SessionStatus,
    StartRequest,
)

router = APIRouter(prefix="/session", tags=["session"])

_CONFLICT = {
    409: {
        "model": ErrorDetail,
        "description": "The current lifecycle state does not permit this.",
    }
}


def _status(driver: Driver) -> SessionStatus:
    return SessionStatus(**driver.status())


@router.get(
    "",
    response_model=SessionStatus,
    summary="Current session lifecycle state",
    description=(
        "Served from the snapshot the driver published, so reading never "
        "touches the session and costs the trading loop nothing."
    ),
)
def session(driver: Driver) -> SessionStatus:
    return _status(driver)


@router.get(
    "/state",
    response_model=SessionStatus,
    summary="Alias of GET /session",
)
def session_state(driver: Driver) -> SessionStatus:
    return _status(driver)


@router.post(
    "/start",
    response_model=SessionActionResponse,
    responses=_CONFLICT,
    summary="Start a paper session",
    description=(
        "Refused with 409 if a session is already running: starting a second "
        "would abandon the first one's book without closing it out.\n\n"
        "Market data is a **deterministic synthetic replay**. ADR-0001's "
        "entitlement is unverified and no live feed exists, so a given seed "
        "always produces the same session."
    ),
)
def start(request: StartRequest, driver: Driver) -> SessionActionResponse:
    try:
        driver.start(
            session_id=request.session_id,
            seed=request.seed,
            bars=request.bars,
            starting_cash=request.starting_cash,
        )
    except IllegalTransition as exc:
        raise HTTPException(
            status_code=status.HTTP_409_CONFLICT, detail=str(exc)
        ) from exc
    except RuntimeError as exc:
        raise HTTPException(
            status_code=status.HTTP_500_INTERNAL_SERVER_ERROR, detail=str(exc)
        ) from exc
    return SessionActionResponse(action="start", state=driver.state.value)


@router.post(
    "/stop",
    response_model=SessionActionResponse,
    responses=_CONFLICT,
    summary="Stop the running session",
    description=(
        "Runs the engine's close-out: on_stop, trade matching and journal "
        "reconciliation, the same sequence a backtest performs."
    ),
)
def stop(driver: Driver) -> SessionActionResponse:
    try:
        driver.stop()
    except IllegalTransition as exc:
        raise HTTPException(
            status_code=status.HTTP_409_CONFLICT, detail=str(exc)
        ) from exc
    except RuntimeError as exc:
        raise HTTPException(
            status_code=status.HTTP_500_INTERNAL_SERVER_ERROR, detail=str(exc)
        ) from exc
    return SessionActionResponse(action="stop", state=driver.state.value)


@router.post(
    "/reset",
    response_model=SessionActionResponse,
    responses=_CONFLICT,
    summary="Stop the session and start a new one",
    description=(
        "Explicitly **not** an in-place reset. Discarding a book where it "
        "stands would skip the close-out that reconciles the journal, so this "
        "is a stop followed by a start with the same guarantees as performing "
        "them separately."
    ),
)
def reset(request: StartRequest, driver: Driver) -> SessionActionResponse:
    try:
        driver.reset(
            session_id=request.session_id,
            seed=request.seed,
            bars=request.bars,
            starting_cash=request.starting_cash,
        )
    except IllegalTransition as exc:
        raise HTTPException(
            status_code=status.HTTP_409_CONFLICT, detail=str(exc)
        ) from exc
    except RuntimeError as exc:
        raise HTTPException(
            status_code=status.HTTP_500_INTERNAL_SERVER_ERROR, detail=str(exc)
        ) from exc
    return SessionActionResponse(action="reset", state=driver.state.value)


@router.get(
    "/snapshot",
    response_model=SessionSnapshot,
    summary="Account, positions, orders and fills in one consistent read",
    description=(
        "A single document, so a dashboard renders one instant. Assembling it "
        "from four calls would let the account and the positions come from "
        "different moments."
    ),
)
def snapshot(driver: Driver) -> SessionSnapshot:
    return SessionSnapshot(state=driver.state.value, **_extract(driver.snapshot()))


def _extract(snapshot: dict) -> dict:
    """Flatten the host document, tolerating a session that has not started.

    Missing sections become empty rather than raising: a dashboard polling
    before the first start is a normal state, not an error.
    """
    portfolio = snapshot.get("portfolio", {})
    return {
        "available": bool(portfolio.get("available")),
        "account": portfolio.get("account"),
        "positions": snapshot.get("positions", {}).get("positions", []),
        "orders": snapshot.get("orders", {}).get("orders", []),
        "fills": snapshot.get("fills", {}).get("fills", []),
        "engine": snapshot.get("state", {}),
    }
