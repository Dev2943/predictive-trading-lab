"""Artifact-backed endpoints: portfolio, positions, diagnostics, metrics, reports.

WHAT THESE CAN AND CANNOT SHOW.

No session runs inside the gateway, so there is no live portfolio to report.
What exists is what sessions previously WROTE: paper and live session state,
account snapshots, reports. These endpoints serve those.

When nothing has been written, the response says `available: false`. An empty
portfolio and a portfolio worth nothing render identically on a chart, and only
one of them is true. Live state arrives with the session host in a later phase.
"""

from __future__ import annotations

from fastapi import APIRouter, HTTPException, Path, Query, status

from ..dependencies import Engine
from ..engine import EngineError
from ..models.schemas import (
    AccountSummary,
    ArtifactEnvelope,
    ArtifactList,
    ErrorDetail,
    PortfolioResponse,
    Position,
    PositionsResponse,
)

router = APIRouter(tags=["artifacts"])

_NOT_FOUND = {404: {"model": ErrorDetail, "description": "No such artifact"}}


def _read(engine: Engine, key: str, what: str) -> ArtifactEnvelope:
    try:
        data = engine.read_artifact(key)
    except EngineError as exc:
        # 500: the artifact exists and could not be read. Distinct from absence,
        # which is a normal state and answered with available=false.
        raise HTTPException(
            status_code=status.HTTP_500_INTERNAL_SERVER_ERROR, detail=str(exc)
        ) from exc

    if data is None:
        return ArtifactEnvelope(
            available=False, key=key, detail=f"no {what} has been persisted at '{key}'"
        )
    return ArtifactEnvelope(available=True, key=key, data=data)


@router.get(
    "/artifacts",
    response_model=ArtifactList,
    summary="List persisted artifact keys",
)
def artifacts(
    engine: Engine,
    prefix: str = Query(default="", description="Key prefix, e.g. 'paper'."),
) -> ArtifactList:
    keys = engine.list_artifacts(prefix)
    return ArtifactList(prefix=prefix, keys=keys, count=len(keys))


@router.get(
    "/portfolio",
    response_model=PortfolioResponse,
    summary="Account state from the last persisted session snapshot",
    description=(
        "Sourced from what a session wrote, not from a live book. "
        "`available: false` when no session has run."
    ),
)
def portfolio(
    engine: Engine,
    session: str = Query(default="paper", description="Session id."),
    mode: str = Query(default="paper", pattern="^(paper|live)$"),
) -> PortfolioResponse:
    envelope = _read(engine, f"{mode}/{session}/state", "session state")
    if not envelope.available or envelope.data is None:
        return PortfolioResponse(available=False, mode=mode, detail=envelope.detail)

    data = envelope.data
    account = data.get("account") or {}
    return PortfolioResponse(
        available=True,
        session_id=data.get("session_id"),
        mode=mode,
        # Fields absent from an older snapshot stay None. Inventing a zero
        # would put a number on a screen that no session ever recorded.
        account=AccountSummary(**{
            key: account.get(key)
            for key in AccountSummary.model_fields
            if key in account
        }),
        events_processed=data.get("events_processed"),
    )


@router.get(
    "/positions",
    response_model=PositionsResponse,
    summary="Open positions from the last persisted session snapshot",
)
def positions(
    engine: Engine,
    session: str = Query(default="paper"),
    mode: str = Query(default="paper", pattern="^(paper|live)$"),
) -> PositionsResponse:
    envelope = _read(engine, f"{mode}/{session}/state", "session state")
    if not envelope.available or envelope.data is None:
        return PositionsResponse(available=False, mode=mode, detail=envelope.detail)

    data = envelope.data
    return PositionsResponse(
        available=True,
        session_id=data.get("session_id"),
        mode=mode,
        positions=[Position(**p) for p in data.get("positions", [])],
    )


@router.get(
    "/diagnostics",
    response_model=ArtifactEnvelope,
    summary="Last persisted live session state",
)
def diagnostics(
    engine: Engine, session: str = Query(default="live")
) -> ArtifactEnvelope:
    return _read(engine, f"live/{session}/state", "diagnostics snapshot")


@router.get(
    "/metrics",
    response_model=ArtifactEnvelope,
    summary="Operational metrics from a persisted snapshot",
    description=(
        "The engine's ops metrics live in the process that ran the session. "
        "The gateway serves the last snapshot written to disk; it does not "
        "report its own request counts, which would be gateway telemetry "
        "presented as engine state."
    ),
)
def metrics(
    engine: Engine, session: str = Query(default="live")
) -> ArtifactEnvelope:
    return _read(engine, f"live/{session}/metrics", "metrics snapshot")


@router.get(
    "/reports",
    response_model=ArtifactList,
    summary="List available reports",
)
def reports(engine: Engine) -> ArtifactList:
    keys = engine.list_artifacts("experiments")
    return ArtifactList(prefix="experiments", keys=keys, count=len(keys))


@router.get(
    "/reports/{experiment_id}",
    response_model=ArtifactEnvelope,
    responses=_NOT_FOUND,
    summary="One experiment report",
    description=(
        "404 when the report does not exist. Unlike portfolio state, a report "
        "requested by id is a specific thing the caller believes exists, so "
        "absence is an error rather than a normal empty state."
    ),
)
def report(
    engine: Engine,
    experiment_id: str = Path(description="Experiment identifier."),
) -> ArtifactEnvelope:
    envelope = _read(engine, f"experiments/{experiment_id}/result", "report")
    if not envelope.available:
        raise HTTPException(
            status_code=status.HTTP_404_NOT_FOUND,
            detail=f"no report for experiment '{experiment_id}'",
        )
    return envelope
