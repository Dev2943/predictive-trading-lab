"""Order entry and management.

EVERY COMMAND GOES THROUGH THE SESSION DRIVER. A request handler never touches
the session: it enqueues, the driver applies between steps, and the engine
receives the order at the next event through the same OrderSink a strategy uses.

That means the risk gate sees a manual order exactly as it sees an automatic
one. There is one submission path, and a manual order can be rejected by risk
just as a strategy's can — the rejection carries the engine's own reason.

ACCEPTANCE HERE IS NOT A FILL, and the response says so. An order is queued;
it faces the venue at the next event.
"""

from __future__ import annotations

from fastapi import APIRouter, HTTPException, status

from ..dependencies import Driver
from ..engine import IllegalTransition
from ..models.schemas import (
    BulkActionResponse,
    ErrorDetail,
    OrderAccepted,
    OrderHistory,
    OrderRequest,
    PendingOrders,
    TradingMode,
)

router = APIRouter(prefix="/trading", tags=["trading"])

_CONFLICT = {
    409: {
        "model": ErrorDetail,
        "description": "No running session, or the lifecycle forbids this.",
    }
}
_REFUSED = {422: {"model": ErrorDetail, "description": "The engine refused, and says why."}}


@router.get(
    "/mode",
    response_model=TradingMode,
    summary="Which venue the session trades against",
    description=(
        "Always PAPER today. No live broker is connected and none is "
        "simulated — `live_available` stays false until a real adapter exists, "
        "so an interface cannot accidentally present paper fills as live ones."
    ),
)
def mode() -> TradingMode:
    return TradingMode(
        mode="PAPER",
        live_available=False,
        label="PAPER",
        detail=(
            "Live trading requires a broker adapter and a verified market data "
            "entitlement (ADR-0001); neither exists yet."
        ),
    )


@router.post(
    "/orders",
    response_model=OrderAccepted,
    responses={**_CONFLICT, **_REFUSED},
    summary="Submit an order",
    description=(
        "Queues the order. It reaches the engine at the next market event and "
        "passes the same risk gate a strategy's order does, so a 200 here means "
        "*accepted for submission*, not filled.\n\n"
        "Poll `/trading/pending` for the outcome, matched by `request_id`."
    ),
)
def submit(request: OrderRequest, driver: Driver) -> OrderAccepted:
    try:
        request_id = driver.submit_order(
            symbol=request.symbol,
            side=request.side,
            quantity=request.quantity,
            type=request.type,
            limit_price=request.limit_price,
            stop_price=request.stop_price,
            time_in_force=request.time_in_force,
        )
    except IllegalTransition as exc:
        raise HTTPException(
            status_code=status.HTTP_409_CONFLICT, detail=str(exc)
        ) from exc
    except RuntimeError as exc:
        # The host validates symbol, quantity and price BEFORE queuing, so a
        # malformed order is refused while the user can still connect the
        # rejection to what they typed.
        raise HTTPException(status_code=422, detail=str(exc)) from exc
    return OrderAccepted(request_id=request_id)


@router.delete(
    "/orders/{order_id}",
    response_model=BulkActionResponse,
    responses={**_CONFLICT, **_REFUSED},
    summary="Cancel one order",
    description=(
        "DELETE because it removes a working order. Like a submission it is "
        "queued and applied at the next event: the order is cancelled when the "
        "engine says so, not when this returns."
    ),
)
def cancel(order_id: int, driver: Driver) -> BulkActionResponse:
    try:
        driver.cancel_order(order_id)
    except IllegalTransition as exc:
        raise HTTPException(
            status_code=status.HTTP_409_CONFLICT, detail=str(exc)
        ) from exc
    except RuntimeError as exc:
        raise HTTPException(status_code=422, detail=str(exc)) from exc
    return BulkActionResponse(action="cancel_all", queued=1)


@router.post(
    "/cancel-all",
    response_model=BulkActionResponse,
    responses=_CONFLICT,
    summary="Cancel every working order",
)
def cancel_all(driver: Driver) -> BulkActionResponse:
    try:
        queued = driver.cancel_all()
    except IllegalTransition as exc:
        raise HTTPException(
            status_code=status.HTTP_409_CONFLICT, detail=str(exc)
        ) from exc
    return BulkActionResponse(action="cancel_all", queued=queued)


@router.post(
    "/flatten",
    response_model=BulkActionResponse,
    responses=_CONFLICT,
    summary="Cancel everything and close every position",
    description=(
        "Cancels working orders first, then queues market orders closing each "
        "open position. The order matters: closing while an order still works "
        "could leave the book flat and an order live, which would re-open the "
        "position the caller asked to eliminate.\n\n"
        "**Paper only**, enforced in the host rather than by a disabled button."
    ),
)
def flatten(driver: Driver) -> BulkActionResponse:
    try:
        queued = driver.flatten()
    except IllegalTransition as exc:
        raise HTTPException(
            status_code=status.HTTP_409_CONFLICT, detail=str(exc)
        ) from exc
    return BulkActionResponse(action="flatten", queued=queued)


@router.get(
    "/pending",
    response_model=PendingOrders,
    summary="Queued requests and their outcomes",
    description=(
        "Served from the published snapshot. `outcomes` reports what happened "
        "to each request once the engine saw it, including a risk rejection — "
        "an order that vanished silently would leave the user believing it was "
        "live."
    ),
)
def pending(driver: Driver) -> PendingOrders:
    raw = driver.snapshot().get("pending") or {"pending": 0, "outcomes": []}
    return PendingOrders(**raw)


@router.get(
    "/orders",
    response_model=OrderHistory,
    summary="Every order this session has seen",
    description=(
        "Working, filled, cancelled and rejected, most recent first. The OMS "
        "exposes only working orders, so the host tracks submitted ids to keep "
        "terminal orders in the blotter rather than letting them vanish on fill."
    ),
)
def orders(driver: Driver) -> OrderHistory:
    raw = driver.snapshot().get("order_history") or {"available": False, "orders": []}
    return OrderHistory(**raw)
