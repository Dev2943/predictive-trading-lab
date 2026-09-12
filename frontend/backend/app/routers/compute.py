"""Computation endpoints: optimization, covariance, analytics, risk validation.

WHY THESE DO NOT GO THROUGH THE SESSION DRIVER.

Every one of these is a pure function of its request. They construct their own
inputs, touch no session, and the engine keeps no state across calls. Routing
them through the single-writer queue would serialise stateless arithmetic behind
the trading loop -- so a dashboard running an optimization would delay the book
from marking, for no benefit whatsoever.

The single-writer rule exists to protect MUTABLE SESSION STATE. Nothing here has
any. They reach the engine through `EngineClient`, exactly as the read-only
endpoints of F2 do.

WHY THEY ARE POST.

Their inputs are matrices. A covariance matrix does not fit in a query string,
and encoding one would produce an endpoint nobody could call by hand with a
length limit nobody expects. POST here means "carries a body", not "mutates" --
and a test asserts the determinism fingerprints are unmoved by calling all of
them.
"""

from __future__ import annotations

from fastapi import APIRouter, HTTPException

from ..dependencies import Engine
from ..engine import EngineError
from ..models.schemas import (
    CovarianceRequest,
    CovarianceResponse,
    ErrorDetail,
    FactorRequest,
    FactorResponse,
    OptimizeRequest,
    OptimizeResponse,
    PerformanceMetrics,
    PerformanceRequest,
    RiskLimitsRequest,
    RollingRequest,
    RollingResponse,
    ValidationResponse,
)

router = APIRouter(tags=["compute"])

# 422 rather than 500 for a refusal. The engine understood the request and
# cannot answer it -- "minimum variance needs a covariance matrix" tells the
# caller what to do, where a 500 would suggest we broke.
_REFUSED = {
    422: {
        "model": ErrorDetail,
        "description": "The engine refused, and says why.",
    }
}


@router.post(
    "/optimization/optimize",
    response_model=OptimizeResponse,
    responses=_REFUSED,
    summary="Run one portfolio optimizer",
    description=(
        "Pure computation: the same request always produces the same response, "
        "and no session state is touched.\n\n"
        "An optimizer that cannot answer raises rather than returning zeros — "
        "a silently flat weight vector looks like a deliberate decision to hold "
        "nothing."
    ),
)
def optimize(request: OptimizeRequest, engine: Engine) -> OptimizeResponse:
    try:
        result = engine.optimize(
            optimizer=request.optimizer,
            expected_returns=request.expected_returns,
            volatilities=request.volatilities,
            covariance=request.covariance,
            signals=request.signals,
            symbols=request.symbols,
            max_position=request.max_position,
            long_only=request.long_only,
            max_gross_leverage=request.max_gross_leverage,
            risk_aversion=request.risk_aversion,
            target_volatility=request.target_volatility,
        )
    except EngineError as exc:
        raise HTTPException(status_code=422, detail=str(exc)) from exc
    return OptimizeResponse(**result)


@router.post(
    "/optimization/covariance",
    response_model=CovarianceResponse,
    responses=_REFUSED,
    summary="Estimate a covariance and correlation matrix",
    description=(
        "Diagnostics travel with the matrix. When the estimator substitutes a "
        "simpler model — too few observations for the asset count — it says so, "
        "so a caller cannot use a degraded estimate believing it is a full one."
    ),
)
def covariance(request: CovarianceRequest, engine: Engine) -> CovarianceResponse:
    try:
        result = engine.estimate_covariance(
            observations=request.observations,
            method=request.method,
            window=request.window,
            shrinkage=request.shrinkage,
            min_observations_ratio=request.min_observations_ratio,
        )
    except EngineError as exc:
        raise HTTPException(status_code=422, detail=str(exc)) from exc
    return CovarianceResponse(**result)


@router.post(
    "/analytics/rolling",
    response_model=RollingResponse,
    responses=_REFUSED,
    summary="Rolling volatility, Sharpe, VaR and CVaR",
    description=(
        "Values before the window fills are `null`, never zero. A zero would "
        "draw a line through the origin that no data supports."
    ),
)
def rolling(request: RollingRequest, engine: Engine) -> RollingResponse:
    try:
        result = engine.rolling_metrics(
            returns=request.returns,
            window=request.window,
            periods_per_year=request.periods_per_year,
            var_confidence=request.var_confidence,
        )
    except EngineError as exc:
        raise HTTPException(status_code=422, detail=str(exc)) from exc
    return RollingResponse(**result)


@router.post(
    "/analytics/attribution/factors",
    response_model=FactorResponse,
    responses=_REFUSED,
    summary="Split returns into beta and alpha contributions",
    description=(
        "A mismatched benchmark is refused rather than truncated: pairing each "
        "return with the wrong observation produces a beta that means nothing."
    ),
)
def factors(request: FactorRequest, engine: Engine) -> FactorResponse:
    try:
        result = engine.factor_contribution(request.portfolio, request.benchmark)
    except EngineError as exc:
        raise HTTPException(status_code=422, detail=str(exc)) from exc
    return FactorResponse(**result)


@router.post(
    "/risk/validate",
    response_model=ValidationResponse,
    responses=_REFUSED,
    summary="Semantic validation of a risk limit set",
    description=(
        "Catches values that parse perfectly and are still wrong: a limit of "
        "zero rejects every order, and a limit of 1e9 can never bind while "
        "reading as protection on a control report.\n\n"
        "Every issue is reported at once — an operator fixing a config should "
        "get one list, not six consecutive failed starts."
    ),
)
def validate_risk(
    request: RiskLimitsRequest, engine: Engine
) -> ValidationResponse:
    try:
        result = engine.validate_risk_limits(
            max_order_notional=request.max_order_notional,
            max_position_notional=request.max_position_notional,
            max_gross_leverage=request.max_gross_leverage,
            max_concentration=request.max_concentration,
            max_drawdown_pct=request.max_drawdown_pct,
            max_daily_turnover=request.max_daily_turnover,
            require_live=request.require_live,
        )
    except EngineError as exc:
        raise HTTPException(status_code=422, detail=str(exc)) from exc
    return ValidationResponse(**result)


@router.post(
    "/analytics/performance",
    response_model=PerformanceMetrics,
    responses=_REFUSED,
    summary="Full performance metrics for an equity series",
    description=(
        "Computed by the engine's `MetricsEngine` — cumulative and annualised "
        "return, CAGR, volatility, downside volatility, Sharpe, Sortino, "
        "Calmar, maximum drawdown and its duration, skewness, best and worst "
        "period.\n\n"
        "Takes equity **levels**, not returns: the engine derives returns per "
        "its configured basis so the basis is decided once.\n\n"
        "Trade-derived fields (win rate, profit factor, expectancy) are zero "
        "here. An equity series cannot distinguish a round trip from a mark, "
        "and the engine reports zero rather than inferring trades that were "
        "never supplied."
    ),
)
def performance(request: PerformanceRequest, engine: Engine) -> PerformanceMetrics:
    try:
        result = engine.performance_metrics(
            equity=request.equity, periods_per_year=request.periods_per_year
        )
    except EngineError as exc:
        raise HTTPException(status_code=422, detail=str(exc)) from exc
    return PerformanceMetrics(**result)
