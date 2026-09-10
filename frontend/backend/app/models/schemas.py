"""Wire models.

These mirror the engine's JSON rather than redefining it. Where the engine names
a field, that name is kept -- a gateway that renames things forces every reader
to hold two vocabularies and makes an engine change look like a breaking API
change when it is not.

TWO CONVENTIONS RUN THROUGH THIS FILE.

`available: bool` on anything sourced from a persisted artifact. No session runs
inside the gateway, so portfolio, positions, diagnostics and metrics come from
what a session previously WROTE. When nothing has been written the response says
so. Returning an empty portfolio instead would render identically to a portfolio
worth nothing, and only one of those is true.

`None`, never zero, for a value that does not exist yet. A rolling window that
has not filled and a genuine zero are different states, and a chart must be able
to tell them apart.
"""

from __future__ import annotations

from typing import Any, Literal

from pydantic import BaseModel, ConfigDict, Field


# ---------------------------------------------------------------------------
# System
# ---------------------------------------------------------------------------


class HealthResponse(BaseModel):
    model_config = ConfigDict(
        json_schema_extra={
            "examples": [
                {"status": "ok", "engine_reachable": True, "detail": ""},
                {
                    "status": "unavailable",
                    "engine_reachable": False,
                    "detail": "the ptl bindings are not importable",
                },
            ]
        }
    )

    status: Literal["ok", "degraded", "unavailable"] = Field(
        description="`unavailable` means the engine could not be reached at all."
    )
    engine_reachable: bool
    detail: str = ""


class VersionInfo(BaseModel):
    model_config = ConfigDict(
        json_schema_extra={
            "examples": [
                {
                    "engine_version": "1.0.0",
                    "compiler": "GNU",
                    "build_type": "Release",
                    "transport": "in-process",
                    "gateway_version": "0.2.0",
                }
            ]
        }
    )

    engine_version: str
    compiler: str
    build_type: str
    transport: Literal["in-process", "remote"] = Field(
        description=(
            "Where the engine runs. Declared rather than inferred so a client "
            "can show it without guessing."
        )
    )
    gateway_version: str


class Fingerprints(BaseModel):
    model_config = ConfigDict(
        json_schema_extra={
            "examples": [
                {
                    "config_hash": "30b44e5972450aad",
                    "rng": [
                        "d05ef55272cdfb14",
                        "2e2f422341add64e",
                        "1c120f3d1ce63170",
                    ],
                    "matches_reference": True,
                }
            ]
        }
    )

    config_hash: str
    rng: list[str]
    matches_reference: bool = Field(
        description=(
            "False means this engine does not reproduce the v1.0 reference "
            "build, which is worth knowing before trusting a backtest from it."
        )
    )


class SystemInfo(BaseModel):
    """Everything a status header needs, in one call.

    Aggregated so a client does not fan out three requests to render one bar.
    """

    health: HealthResponse
    version: VersionInfo
    fingerprints: Fingerprints | None = Field(
        default=None,
        description="Absent when the configuration could not be read.",
    )


# ---------------------------------------------------------------------------
# Capabilities
# ---------------------------------------------------------------------------


class OptimizerInfo(BaseModel):
    name: str
    requires_covariance: bool = Field(
        description="The optimizer refuses without a risk model."
    )
    requires_expected_returns: bool = Field(
        description=(
            "The optimizer refuses without forecasts. A zero forecast is a real "
            "opinion, not an absence of one, so none is assumed."
        )
    )


class OptimizationCapabilities(BaseModel):
    model_config = ConfigDict(
        json_schema_extra={
            "examples": [
                {
                    "optimizers": [
                        {
                            "name": "equal_weight",
                            "requires_covariance": False,
                            "requires_expected_returns": False,
                        }
                    ],
                    "count": 9,
                    "note": "Running an optimization requires a request body and arrives in a later phase.",
                }
            ]
        }
    )

    optimizers: list[OptimizerInfo]
    count: int
    note: str


class AnalyticsCapabilities(BaseModel):
    rolling_metrics: list[str]
    attribution: list[str]
    note: str


class RiskCapabilities(BaseModel):
    validated_limits: list[str] = Field(
        description="Limit fields the engine validates semantically."
    )
    note: str


# ---------------------------------------------------------------------------
# Artifact-backed
# ---------------------------------------------------------------------------


class ArtifactEnvelope(BaseModel):
    """A persisted artifact, or an explicit statement that none exists.

    `available: false` is a normal answer, not an error. No session runs inside
    the gateway, so this data exists only if a session previously wrote it.
    """

    model_config = ConfigDict(
        json_schema_extra={
            "examples": [
                {
                    "available": True,
                    "key": "paper/demo/state",
                    "data": {"session_id": "demo", "events_processed": 120},
                    "detail": "",
                },
                {
                    "available": False,
                    "key": "paper/never_ran/state",
                    "data": None,
                    "detail": "no session state has been persisted at 'paper/never_ran/state'",
                },
            ]
        }
    )

    available: bool
    key: str
    data: dict[str, Any] | None = None
    detail: str = ""


class AccountSummary(BaseModel):
    """The account fields a persisted session snapshot carries.

    Every field is optional: a snapshot written by an older session may not
    carry all of them, and inventing a zero would be worse than omitting it.
    """

    cash: float | None = None
    equity: float | None = None
    position_value: float | None = None
    realized_pnl: float | None = None
    unrealized_pnl: float | None = None
    gross_exposure: float | None = None
    net_exposure: float | None = None
    available_buying_power: float | None = None
    status: str | None = None


class PortfolioResponse(BaseModel):
    available: bool
    session_id: str | None = None
    mode: Literal["paper", "live"]
    account: AccountSummary | None = None
    events_processed: int | None = None
    detail: str = ""


class Position(BaseModel):
    instrument: int
    quantity: float
    average_cost: float | None = None
    realized_pnl: float | None = None


class PositionsResponse(BaseModel):
    available: bool
    session_id: str | None = None
    mode: Literal["paper", "live"]
    positions: list[Position] = []
    detail: str = ""


class ArtifactList(BaseModel):
    prefix: str
    keys: list[str] = Field(
        description="Sorted, so two calls agree. Directory order is filesystem-defined."
    )
    count: int


class ErrorDetail(BaseModel):
    """The shape of every error body.

    `detail` carries the ENGINE's own message where there is one. An optimizer
    that refuses says why, and that reason is more useful than anything the
    gateway could invent.
    """

    model_config = ConfigDict(
        json_schema_extra={
            "examples": [
                {"detail": "minimum variance needs a covariance matrix"},
                {"detail": "config load: no such file (does/not/exist.toml)"},
            ]
        }
    )

    detail: str


# ---------------------------------------------------------------------------
# Session (F3)
# ---------------------------------------------------------------------------


class StartRequest(BaseModel):
    """Parameters for a new paper session.

    `seed` makes the run reproducible: the same seed replays the same synthetic
    session, so a result can be reproduced rather than merely described.
    """

    model_config = ConfigDict(
        json_schema_extra={
            "examples": [
                {
                    "session_id": "paper",
                    "seed": 20240101,
                    "bars": 390,
                    "starting_cash": 1000000.0,
                }
            ]
        }
    )

    session_id: str = Field(default="paper", min_length=1, max_length=64)
    seed: int = Field(default=20240101, gt=0, description="Zero is rejected: a run whose seed is unknown cannot be reproduced.")
    bars: int = Field(default=390, gt=0, le=5000)
    starting_cash: float = Field(default=1_000_000.0, gt=0)


class EngineSessionState(BaseModel):
    """The host's own view. Every field optional: before the first start there
    is no session to describe, and inventing zeros would show a book that does
    not exist."""

    state: str | None = None
    has_session: bool | None = None
    session_id: str | None = None
    seed: int | None = None
    data_source: str | None = Field(
        default=None,
        description="`synthetic-replay` until ADR-0001's entitlement is verified.",
    )
    phase: str | None = None
    trading_permitted: bool | None = None
    events_processed: int | None = None
    orders_submitted: int | None = None
    orders_rejected: int | None = None
    fills_received: int | None = None
    persists: int | None = None
    started_at: str | None = None
    error: str | None = None


class SessionStatus(BaseModel):
    model_config = ConfigDict(
        json_schema_extra={
            "examples": [
                {
                    "state": "RUNNING",
                    "error": None,
                    "replay_exhausted": False,
                    "engine": {"phase": "running", "events_processed": 120},
                }
            ]
        }
    )

    state: Literal["STOPPED", "STARTING", "RUNNING", "STOPPING", "ERROR"]
    error: str | None = None
    replay_exhausted: bool = Field(
        default=False,
        description=(
            "The replay ran out of events. Not an error and not a stop: the "
            "book stays readable, as a live session between market events."
        ),
    )
    engine: EngineSessionState = Field(default_factory=EngineSessionState)


class SessionActionResponse(BaseModel):
    action: Literal["start", "stop", "reset"]
    state: str


class OpenOrder(BaseModel):
    order_id: int
    instrument: int
    side: int = Field(description="1 for buy, -1 for sell.")
    quantity: float
    filled: float


class FillRecord(BaseModel):
    ts: str
    order_id: int
    instrument: int
    side: int
    quantity: float
    price: float
    commission: float


class SessionSnapshot(BaseModel):
    """One consistent read of the session."""

    state: str
    available: bool = Field(
        description="False before a session has started. Not an empty portfolio."
    )
    account: AccountSummary | None = None
    positions: list[Position] = []
    orders: list[OpenOrder] = []
    fills: list[FillRecord] = Field(
        default=[], description="Most recent first, bounded to the last 200."
    )
    engine: EngineSessionState = Field(default_factory=EngineSessionState)
