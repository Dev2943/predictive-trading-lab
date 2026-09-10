"""Binding tests.

The load-bearing test is the determinism one. If the binding layer perturbs the
engine's fingerprints, nothing built on top can be trusted and every later phase
is unbuildable.
"""

from __future__ import annotations

import json
import math

import pytest

import ptl

# From `ptl_version -c config/base.toml`. Stable across every phase, compiler and
# build configuration of the engine.
REFERENCE_RNG = ["d05ef55272cdfb14", "2e2f422341add64e", "1c120f3d1ce63170"]
REFERENCE_CONFIG_HASH = "30b44e5972450aad"


# --- determinism -----------------------------------------------------------


def test_rng_fingerprint_matches_the_engine():
    """The same seed through the bindings must give the stream the C++ binary
    reports. A difference means the boundary is not transparent."""
    assert ptl.rng_fingerprint(20240101, 3) == REFERENCE_RNG


def test_config_hash_matches_the_engine(engine_root):
    assert ptl.config_hash(str(engine_root / "config/base.toml")) == REFERENCE_CONFIG_HASH


def test_no_hidden_state_accumulates():
    first = ptl.rng_fingerprint(7, 16)
    assert ptl.rng_fingerprint(7, 16) == first
    # A different seed must differ, or the fingerprint measures nothing.
    assert ptl.rng_fingerprint(8, 16) != first


def test_version_is_reported():
    assert ptl.engine_version.startswith("1.0")
    assert ptl.compiler and ptl.build_type


# --- sessions stay internal -------------------------------------------------


def test_sessions_are_not_reachable_from_python():
    """Engine, PaperSession and LiveSession must NOT be bound.

    Handing a request handler a pointer to a live trading object is the failure
    this architecture exists to prevent. This test is the guard: if someone
    later binds one for convenience, it fails here.
    """
    exposed = set(dir(ptl))
    for forbidden in (
        "Engine",
        "PaperSession",
        "LiveSession",
        "BrokerSimulator",
        "PaperBroker",
        "PaperAccount",
        "PaperSessionHost",
    ):
        assert forbidden not in exposed

    # F3 added a session host, and it is reachable only through free functions
    # returning JSON. No bound class means no handle to engine state.
    for name in ("session_start", "session_stop", "session_state"):
        assert callable(getattr(ptl, name))
        assert not isinstance(getattr(ptl, name), type)


def test_only_the_intended_surface_is_exposed():
    callables = {n for n in dir(ptl) if not n.startswith("_") and callable(getattr(ptl, n))}
    assert callables == {
        "config_hash",
        "estimate_covariance",
        "factor_contribution",
        "optimize",
        "optimizer_names",
        "rng_fingerprint",
        "rolling_metrics",
        "validate_risk_limits",
        # F3 session host. Five FUNCTIONS, no types: the host owns the session
        # in C++ and Python never receives a handle to it.
        "session_snapshot",
        "session_start",
        "session_state",
        "session_step",
        "session_stop",
    }


# --- optimization ----------------------------------------------------------


@pytest.fixture
def observations():
    """Five assets sharing a factor, so they are genuinely correlated."""
    return [
        [
            math.sin(r * 0.11) * 0.01
            + math.sin(r * 0.3 + c * 2.0) * 0.004 * (1.0 + c * 0.5)
            for c in range(5)
        ]
        for r in range(400)
    ]


def test_all_nine_optimizers_are_exposed():
    names = ptl.optimizer_names()
    assert len(names) == 9
    assert "risk_parity" in names and "max_sharpe" in names


def test_equal_weight_is_one_over_n():
    result = json.loads(
        ptl.optimize("equal_weight", volatilities=[0.1] * 4, max_position=1.0)
    )
    assert result["weights"] == pytest.approx([0.25] * 4)


def test_optimization_round_trips_as_json(observations):
    estimated = ptl.estimate_covariance(observations)
    result = json.loads(
        ptl.optimize(
            "risk_parity",
            volatilities=[math.sqrt(estimated["covariance"][i][i]) for i in range(5)],
            covariance=estimated["covariance"],
            symbols=list("ABCDE"),
            max_position=0.5,
        )
    )
    assert result["symbols"] == list("ABCDE")
    assert all(math.isfinite(w) for w in result["weights"])


def test_a_refusal_raises_rather_than_returning_zeros():
    """minimum_variance without a covariance cannot be computed. Silently
    returning zeros would look like a deliberate flat position."""
    with pytest.raises(RuntimeError, match="covariance"):
        ptl.optimize("minimum_variance", volatilities=[0.1] * 4)


def test_optimization_is_deterministic(observations):
    estimated = ptl.estimate_covariance(observations)
    kwargs = dict(
        expected_returns=[0.02] * 5,
        volatilities=[0.1] * 5,
        covariance=estimated["covariance"],
        max_position=0.4,
    )
    # Byte-identical JSON, not merely equal numbers.
    assert ptl.optimize("max_sharpe", **kwargs) == ptl.optimize("max_sharpe", **kwargs)


# --- covariance -------------------------------------------------------------


def test_covariance_carries_diagnostics(observations):
    estimated = ptl.estimate_covariance(observations, method="shrinkage")
    assert estimated["observations"] == 400
    assert estimated["applied_shrinkage"] > 0.0
    for i in range(5):
        assert estimated["correlation"][i][i] == pytest.approx(1.0)


def test_too_few_observations_degrade_and_say_so():
    rows = [[0.01 * (i + c) for c in range(10)] for i in range(4)]
    estimated = ptl.estimate_covariance(rows, method="sample")
    assert estimated["degraded"] is True
    assert "diagonal" in estimated["degradation_reason"]


def test_the_observation_guard_can_only_be_relaxed_deliberately():
    """An earlier draft hardcoded this off, silently disabling an engine
    guarantee. Relaxing it must be an explicit act by the caller."""
    rows = [[0.01 * (i + c) for c in range(10)] for i in range(4)]
    permissive = ptl.estimate_covariance(rows, method="sample", min_observations_ratio=0.0)
    assert permissive["degraded"] is False


def test_ragged_observations_raise():
    with pytest.raises(RuntimeError, match="equal length"):
        ptl.estimate_covariance([[1.0, 2.0], [3.0]])


# --- analytics --------------------------------------------------------------


def test_rolling_values_are_none_before_the_window_fills():
    """None, never zero. Zero would draw a line through the origin that no data
    supports."""
    metrics = ptl.rolling_metrics([math.sin(i * 0.4) * 0.01 for i in range(40)], window=10)
    assert all(v is None for v in metrics["volatility"][:9])
    assert metrics["volatility"][9] is not None


def test_omega_ratio_is_exposed():
    metrics = ptl.rolling_metrics([0.03, -0.01] * 20, window=5)
    assert metrics["omega"] == pytest.approx(3.0)


def test_factor_contribution_recovers_a_known_beta():
    benchmark = [math.sin(i * 0.3) * 0.01 for i in range(100)]
    contribution = ptl.factor_contribution([2.0 * b for b in benchmark], benchmark)
    assert contribution["beta"] == pytest.approx(2.0)
    assert contribution["alpha_contribution"] == pytest.approx(0.0, abs=1e-9)


def test_a_mismatched_benchmark_raises():
    with pytest.raises(RuntimeError, match="does not match"):
        ptl.factor_contribution([0.01, 0.02, 0.03], [0.01, 0.02])


# --- validation -------------------------------------------------------------


def test_validation_reports_every_issue_at_once():
    report = json.loads(
        ptl.validate_risk_limits(
            max_order_notional=0.0,
            max_position_notional=0.0,
            max_gross_leverage=0.0,
            max_concentration=5.0,
            max_drawdown_pct=0.0,
        )
    )
    assert report["ok"] is False
    assert report["fatal"] >= 4


def test_sane_limits_validate():
    assert json.loads(ptl.validate_risk_limits())["ok"] is True
