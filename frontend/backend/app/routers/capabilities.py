"""Capability endpoints: what the engine can do.

WHY THESE DESCRIBE RATHER THAN COMPUTE.

F2 is GET-only, and optimization and rolling analytics take matrices as input --
a covariance matrix does not fit in a query string. So these endpoints report
the engine's CAPABILITIES, which is real engine information, and running a
computation waits for a phase that can carry a request body.

The alternative was encoding a matrix into a URL, which would have produced an
endpoint nobody could call by hand and a request length limit nobody expects.
"""

from __future__ import annotations

from fastapi import APIRouter

from ..dependencies import Engine
from ..models.schemas import (
    AnalyticsCapabilities,
    ErrorDetail,
    OptimizationCapabilities,
    OptimizerInfo,
    RiskCapabilities,
)

router = APIRouter(tags=["capabilities"])

# Which optimizers refuse without which inputs.
#
# DERIVED FROM THE ENGINE'S ACTUAL REFUSALS, not from what the names suggest.
# An earlier draft of this table listed risk_parity and target_volatility as
# requiring a covariance because they are risk-based. They do not: without
# correlations, equal risk contribution reduces to inverse volatility and the
# engine says so rather than refusing. An integration test asks the engine
# directly and fails if this table drifts from it.
_REQUIRES_COVARIANCE = {"minimum_variance"}
_REQUIRES_RETURNS = {"kelly", "max_sharpe", "mean_variance"}


@router.get(
    "/optimization",
    response_model=OptimizationCapabilities,
    responses={503: {"model": ErrorDetail}},
    summary="Available portfolio optimizers",
    description=(
        "The nine optimizers the engine registers, and what each requires. "
        "Running one needs a covariance matrix in a request body and is not "
        "part of the read-only surface."
    ),
)
def optimization(engine: Engine) -> OptimizationCapabilities:
    names = engine.optimizer_names()
    return OptimizationCapabilities(
        optimizers=[
            OptimizerInfo(
                name=name,
                requires_covariance=name in _REQUIRES_COVARIANCE,
                requires_expected_returns=name in _REQUIRES_RETURNS,
            )
            for name in names
        ],
        count=len(names),
        note=(
            "Running an optimization requires a request body and arrives in a "
            "later phase."
        ),
    )


@router.get(
    "/analytics",
    response_model=AnalyticsCapabilities,
    summary="Available analytics",
    description=(
        "What the engine computes. Producing a series requires a return "
        "history in a request body and is not part of the read-only surface."
    ),
)
def analytics() -> AnalyticsCapabilities:
    return AnalyticsCapabilities(
        rolling_metrics=["volatility", "sharpe", "var", "cvar", "omega"],
        attribution=["factor_contribution"],
        note=(
            "Computing a series requires a request body and arrives in a later "
            "phase. Rolling values are absent, never zero, before the window "
            "fills."
        ),
    )


@router.get(
    "/risk",
    response_model=RiskCapabilities,
    summary="Risk limits the engine validates",
    description=(
        "Live risk state -- current exposure and utilisation -- requires a "
        "running session and is not available in the read-only surface. "
        "Serving a zeroed risk panel would show an account with no exposure, "
        "which is indistinguishable from a flat book."
    ),
)
def risk() -> RiskCapabilities:
    return RiskCapabilities(
        validated_limits=[
            "max_order_notional",
            "max_position_notional",
            "max_gross_leverage",
            "max_concentration",
            "max_drawdown_pct",
            "max_daily_turnover",
        ],
        note=(
            "Validation takes a limit set in a request body. Live risk state "
            "requires a session."
        ),
    )
