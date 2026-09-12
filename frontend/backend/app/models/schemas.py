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
    symbol: str | None = Field(
        default=None,
        description="Resolved by the host. An id alone leaks an internal index at the user.",
    )
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
    strategy_halted: bool | None = Field(
        default=None,
        description=(
            "The strategy is suppressed but the session is running: data "
            "flows, the book marks, and manual orders still work."
        ),
    )
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


class Instrument(BaseModel):
    instrument: int
    symbol: str


class EquityPoint(BaseModel):
    ts: str
    equity: float
    cash: float
    realized_pnl: float
    unrealized_pnl: float
    gross_exposure: float
    net_exposure: float


class EquityHistory(BaseModel):
    """Sampled equity history and drawdown.

    Sampled by the session host on the driver thread and published with the
    snapshot, so reading it never touches the session. Drawdown comes from the
    engine's own tracker -- the gateway computes nothing.
    """

    model_config = ConfigDict(
        json_schema_extra={
            "examples": [
                {
                    "available": True,
                    "total_points": 120,
                    "stride": 1,
                    "max_drawdown": 0.0042,
                    "current_drawdown": 0.0,
                    "peak_equity": 1000059.54,
                    "points": [],
                }
            ]
        }
    )

    available: bool
    total_points: int = 0
    stride: int = Field(
        default=1,
        description=(
            "Points were downsampled by taking every nth. Stride rather than "
            "averaging: averaging smooths away the drawdown troughs a reader "
            "is looking for."
        ),
    )
    max_drawdown: float = 0.0
    current_drawdown: float = 0.0
    peak_equity: float = 0.0
    points: list[EquityPoint] = []


class OpenOrder(BaseModel):
    order_id: int
    instrument: int
    symbol: str | None = None
    side: int = Field(description="1 for buy, -1 for sell.")
    quantity: float
    filled: float


class FillRecord(BaseModel):
    ts: str
    order_id: int
    instrument: int
    symbol: str | None = None
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
    history: EquityHistory = Field(
        default_factory=lambda: EquityHistory(available=False),
        description="Equity series and drawdown, sampled by the host.",
    )
    instruments: list[Instrument] = []
    engine: EngineSessionState = Field(default_factory=EngineSessionState)


# ---------------------------------------------------------------------------
# Computation (F5)
# ---------------------------------------------------------------------------
#
# These endpoints POST because their inputs are matrices, not because they
# mutate anything. Optimization and analytics are pure functions: the same
# request produces the same response and the engine holds no state across
# calls. A test asserts the determinism fingerprints are unmoved by exercising
# every one of them.


class OptimizeRequest(BaseModel):
    model_config = ConfigDict(
        json_schema_extra={
            "examples": [
                {
                    "optimizer": "risk_parity",
                    "volatilities": [0.12, 0.18, 0.09],
                    "symbols": ["AAA", "BBB", "CCC"],
                    "max_position": 0.5,
                }
            ]
        }
    )

    optimizer: str = Field(description="One of the names from GET /optimization.")
    expected_returns: list[float] = []
    volatilities: list[float] = []
    covariance: list[list[float]] = Field(
        default=[],
        description="Square matrix. Required by minimum_variance; others degrade gracefully.",
    )
    signals: list[float] = []
    symbols: list[str] = Field(
        default=[], description="Echoed back alongside the weights, for labelling."
    )
    max_position: float = Field(default=0.20, gt=0, le=1.0)
    long_only: bool = True
    max_gross_leverage: float = Field(default=1.0, gt=0)
    risk_aversion: float = Field(default=1.0, gt=0)
    target_volatility: float = Field(default=0.10, gt=0)


class OptimizeResponse(BaseModel):
    status: str
    weights: list[float]
    symbols: list[str]
    expected_return: float
    expected_volatility: float
    sharpe: float
    gross_exposure: float
    net_exposure: float
    cash_weight: float
    turnover: float
    iterations: int
    binding_constraints: list[str] = Field(
        description=(
            "Constraints that bound the solution. A weight sitting exactly on a "
            "limit is a constrained answer, not a free one, and the difference "
            "matters when reading the result."
        )
    )
    detail: str


class CovarianceRequest(BaseModel):
    observations: list[list[float]] = Field(
        description="Rows are observations, columns are assets. All rows equal length."
    )
    method: Literal["sample", "rolling", "ewma", "shrinkage", "identity"] = "shrinkage"
    window: int = Field(default=0, ge=0)
    shrinkage: float = Field(default=-1.0, description="Negative selects the engine's automatic intensity.")
    min_observations_ratio: float = Field(
        default=1.5,
        ge=0.0,
        description=(
            "Observations per asset required before a full matrix is estimated. "
            "Lowering it disables a guard the engine applies deliberately."
        ),
    )


class CovarianceResponse(BaseModel):
    covariance: list[list[float]]
    correlation: list[list[float]]
    observations: int
    applied_shrinkage: float
    psd_repaired: bool
    degraded: bool
    degradation_reason: str = Field(
        default="",
        description=(
            "Populated when the estimator substituted a simpler model. A caller "
            "must not use a degraded estimate without knowing it was degraded."
        ),
    )


class RollingRequest(BaseModel):
    returns: list[float] = Field(min_length=1)
    window: int = Field(default=60, gt=1)
    periods_per_year: float = Field(default=252.0, gt=0)
    var_confidence: float = Field(default=0.05, gt=0, lt=1)


class RollingResponse(BaseModel):
    window: int
    omega: float
    # None before the window fills -- never zero, which would draw a line
    # through the origin that no data supports.
    volatility: list[float | None]
    sharpe: list[float | None]
    var: list[float | None]
    cvar: list[float | None]


class FactorRequest(BaseModel):
    portfolio: list[float] = Field(min_length=2)
    benchmark: list[float] = Field(min_length=2)


class FactorResponse(BaseModel):
    beta: float
    beta_contribution: float
    alpha_contribution: float
    residual_contribution: float
    portfolio_return: float
    benchmark_return: float
    periods: int


class RiskLimitsRequest(BaseModel):
    model_config = ConfigDict(
        json_schema_extra={
            "examples": [
                {
                    "max_order_notional": 1000000,
                    "max_position_notional": 1000000,
                    "max_gross_leverage": 1.0,
                    "max_concentration": 0.1,
                    "max_drawdown_pct": 0.2,
                    "max_daily_turnover": 10.0,
                    "require_live": False,
                }
            ]
        }
    )

    max_order_notional: float = 1e6
    max_position_notional: float = 1e6
    max_gross_leverage: float = 1.0
    max_concentration: float = 0.10
    max_drawdown_pct: float = 0.20
    max_daily_turnover: float = 10.0
    require_live: bool = Field(
        default=False,
        description="Live sessions require limits a backtest may legitimately omit.",
    )


class ValidationIssue(BaseModel):
    severity: Literal["warning", "fatal"]
    field: str
    message: str
    remedy: str = Field(description="What a correct value looks like.")


class ValidationResponse(BaseModel):
    ok: bool = Field(description="False when any issue is fatal.")
    fatal: int
    warnings: int
    issues: list[ValidationIssue] = Field(
        description="Every issue, not just the first: an operator should get one list."
    )


# ---------------------------------------------------------------------------
# Trading (F6)
# ---------------------------------------------------------------------------


class TradingMode(BaseModel):
    """Which venue the session is trading against.

    Surfaced so the interface can label it unambiguously. There is no live
    broker connection and none is simulated: `live_available` is false and stays
    false until a real adapter exists.
    """

    mode: Literal["PAPER", "LIVE"] = "PAPER"
    live_available: bool = False
    label: str = Field(description='"PAPER" or "LIVE (not connected)".')
    detail: str = ""


class OrderRequest(BaseModel):
    model_config = ConfigDict(
        json_schema_extra={
            "examples": [
                {"symbol": "SPY", "side": 1, "quantity": 50, "type": "limit", "limit_price": 505.0},
                {"symbol": "SPY", "side": -1, "quantity": 25, "type": "market"},
            ]
        }
    )

    symbol: str = Field(min_length=1, description="Must be an instrument the session trades.")
    side: Literal[1, -1] = Field(description="1 buy, -1 sell.")
    quantity: float = Field(gt=0)
    type: Literal["market", "limit", "stop", "stop_limit"] = "market"
    limit_price: float = Field(default=0.0, ge=0)
    stop_price: float = Field(default=0.0, ge=0)
    time_in_force: Literal["day", "ioc", "fok", "gtc"] = "day"


class OrderAccepted(BaseModel):
    """The order was QUEUED, not filled.

    It reaches the engine at the next event and faces the risk gate there, so
    acceptance here is not acceptance by the venue. `request_id` matches the
    outcome that appears once it has been through.
    """

    request_id: int
    queued: bool = True
    detail: str = "queued; submitted at the next market event"


class OrderOutcome(BaseModel):
    request_id: int
    order_id: int
    accepted: bool
    detail: str = Field(
        default="", description="The engine's rejection reason when not accepted."
    )


class PendingOrders(BaseModel):
    pending: int = Field(description="Requests not yet submitted to the engine.")
    outcomes: list[OrderOutcome] = Field(default=[], description="Most recent first.")


class OrderHistoryEntry(BaseModel):
    order_id: int
    symbol: str
    state: str = Field(description="OMS state: working, filled, cancelled, rejected…")
    side: int
    type: str
    quantity: float
    filled: float
    reject_reason: str = ""


class OrderHistory(BaseModel):
    available: bool
    orders: list[OrderHistoryEntry] = []


class BulkActionResponse(BaseModel):
    action: Literal["cancel_all", "flatten"]
    queued: int = Field(description="Requests enqueued, not orders completed.")


class HaltRequest(BaseModel):
    halted: bool = Field(description="True suppresses strategy order generation.")


class HaltResponse(BaseModel):
    strategy_halted: bool
    detail: str = Field(
        default="",
        description="What remains active while halted, so the control is not mistaken for a stop.",
    )
