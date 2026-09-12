"""Computation endpoint tests.

Run against the real engine where the arithmetic matters -- a mocked optimizer
would only prove the route calls something -- and against a fake for the
refusal paths, which are awkward to provoke otherwise.
"""

from __future__ import annotations

import math

import pytest
from fastapi.testclient import TestClient

ptl = pytest.importorskip("ptl", reason="bindings not built")

from app.main import app  # noqa: E402


@pytest.fixture
def client():
    with TestClient(app) as test_client:
        yield test_client


@pytest.fixture
def observations():
    """Five assets sharing a factor, so risk parity differs from equal weight."""
    return [
        [
            math.sin(r * 0.11) * 0.01
            + math.sin(r * 0.3 + c * 2.0) * 0.004 * (1.0 + c * 0.5)
            for c in range(5)
        ]
        for r in range(400)
    ]


# ---------------------------------------------------------------------------
# Optimization
# ---------------------------------------------------------------------------


def test_equal_weight_is_one_over_n(client):
    body = client.post(
        "/optimization/optimize",
        json={
            "optimizer": "equal_weight",
            "volatilities": [0.1] * 4,
            "max_position": 1.0,
            "symbols": ["A", "B", "C", "D"],
        },
    ).json()
    assert body["weights"] == pytest.approx([0.25] * 4)
    assert body["symbols"] == ["A", "B", "C", "D"]


def test_risk_parity_tilts_toward_lower_volatility(client):
    """The property that distinguishes it from equal weight."""
    body = client.post(
        "/optimization/optimize",
        json={
            "optimizer": "risk_parity",
            "volatilities": [0.05, 0.30],
            "max_position": 1.0,
        },
    ).json()
    assert body["weights"][0] > body["weights"][1]


def test_a_refusal_is_422_carrying_the_engines_reason(client):
    response = client.post(
        "/optimization/optimize",
        json={"optimizer": "minimum_variance", "volatilities": [0.1] * 4},
    )
    assert response.status_code == 422
    # The engine's own words, not a generic failure.
    assert "covariance" in response.json()["detail"]


def test_an_unknown_optimizer_is_422(client):
    response = client.post(
        "/optimization/optimize",
        json={"optimizer": "does_not_exist", "volatilities": [0.1] * 3},
    )
    assert response.status_code == 422


def test_binding_constraints_are_reported(client):
    """A weight resting exactly on a limit is a constrained answer, not a free
    one, and a reader needs to know which."""
    body = client.post(
        "/optimization/optimize",
        json={
            "optimizer": "inverse_volatility",
            "volatilities": [0.01, 0.5, 0.5, 0.5],
            "max_position": 0.3,
        },
    ).json()
    assert max(body["weights"]) <= 0.3 + 1e-9
    assert isinstance(body["binding_constraints"], list)


def test_optimization_is_repeatable(client):
    payload = {
        "optimizer": "risk_parity",
        "volatilities": [0.1, 0.2, 0.15],
        "max_position": 0.6,
    }
    assert (
        client.post("/optimization/optimize", json=payload).json()
        == client.post("/optimization/optimize", json=payload).json()
    )


def test_an_out_of_range_max_position_is_422(client):
    assert (
        client.post(
            "/optimization/optimize",
            json={"optimizer": "equal_weight", "volatilities": [0.1], "max_position": 5.0},
        ).status_code
        == 422
    )


# ---------------------------------------------------------------------------
# Covariance
# ---------------------------------------------------------------------------


def test_covariance_has_a_unit_correlation_diagonal(client, observations):
    body = client.post(
        "/optimization/covariance",
        json={"observations": observations, "method": "shrinkage"},
    ).json()
    assert body["observations"] == 400
    assert body["degraded"] is False
    for i in range(5):
        assert body["correlation"][i][i] == pytest.approx(1.0)


def test_a_degraded_estimate_says_so(client):
    """A caller must not be able to use a degraded estimate unknowingly."""
    body = client.post(
        "/optimization/covariance",
        json={
            "observations": [[0.01 * (i + c) for c in range(10)] for i in range(4)],
            "method": "sample",
        },
    ).json()
    assert body["degraded"] is True
    assert "diagonal" in body["degradation_reason"]


def test_relaxing_the_observation_guard_is_explicit(client):
    """It can be lowered, but only as a deliberate act by the caller."""
    rows = [[0.01 * (i + c) for c in range(10)] for i in range(4)]
    body = client.post(
        "/optimization/covariance",
        json={"observations": rows, "method": "sample", "min_observations_ratio": 0.0},
    ).json()
    assert body["degraded"] is False


def test_ragged_observations_are_422(client):
    assert (
        client.post(
            "/optimization/covariance", json={"observations": [[1.0, 2.0], [3.0]]}
        ).status_code
        == 422
    )


# ---------------------------------------------------------------------------
# Analytics
# ---------------------------------------------------------------------------


def test_rolling_values_are_null_before_the_window_fills(client):
    body = client.post(
        "/analytics/rolling",
        json={"returns": [math.sin(i * 0.4) * 0.01 for i in range(40)], "window": 10},
    ).json()
    assert body["volatility"][:9] == [None] * 9
    assert body["volatility"][9] is not None


def test_empty_returns_are_422(client):
    assert client.post("/analytics/rolling", json={"returns": []}).status_code == 422


def test_factor_attribution_recovers_a_known_beta(client):
    benchmark = [math.sin(i * 0.3) * 0.01 for i in range(100)]
    body = client.post(
        "/analytics/attribution/factors",
        json={"portfolio": [2.0 * b for b in benchmark], "benchmark": benchmark},
    ).json()
    assert body["beta"] == pytest.approx(2.0)
    assert body["alpha_contribution"] == pytest.approx(0.0, abs=1e-9)


def test_a_mismatched_benchmark_is_422(client):
    assert (
        client.post(
            "/analytics/attribution/factors",
            json={"portfolio": [0.01, 0.02, 0.03], "benchmark": [0.01, 0.02]},
        ).status_code
        == 422
    )


# ---------------------------------------------------------------------------
# Risk validation
# ---------------------------------------------------------------------------


def test_sane_limits_validate(client):
    assert client.post("/risk/validate", json={}).json()["ok"] is True


def test_every_issue_is_reported_at_once(client):
    body = client.post(
        "/risk/validate",
        json={
            "max_order_notional": 0.0,
            "max_position_notional": 0.0,
            "max_gross_leverage": 0.0,
            "max_concentration": 5.0,
            "max_drawdown_pct": 0.0,
        },
    ).json()
    assert body["ok"] is False
    assert body["fatal"] >= 4
    assert {i["field"] for i in body["issues"]} >= {"risk.max_order_notional"}
    # Every issue carries a remedy: "invalid" alone makes the operator guess.
    assert all(i["remedy"] for i in body["issues"])


def test_a_limit_too_large_to_bind_is_a_warning_not_an_error(client):
    body = client.post("/risk/validate", json={"max_gross_leverage": 1e9}).json()
    assert body["ok"] is True
    assert body["warnings"] >= 1


# ---------------------------------------------------------------------------
# The determinism claim
# ---------------------------------------------------------------------------


def test_computation_does_not_perturb_determinism(client, monkeypatch, observations):
    """POST here means 'carries a body', not 'mutates'.

    Exercise every computation endpoint, then re-read the fingerprints.
    """
    import pathlib

    monkeypatch.chdir(pathlib.Path(__file__).resolve().parents[3])
    before = client.get("/fingerprints").json()

    client.post(
        "/optimization/optimize",
        json={"optimizer": "equal_weight", "volatilities": [0.1] * 4},
    )
    client.post("/optimization/covariance", json={"observations": observations})
    client.post("/analytics/rolling", json={"returns": [0.001] * 100, "window": 20})
    client.post(
        "/analytics/attribution/factors",
        json={"portfolio": [0.02] * 50, "benchmark": [0.01] * 50},
    )
    client.post("/risk/validate", json={})

    assert client.get("/fingerprints").json() == before


# ---------------------------------------------------------------------------
# P10: performance metrics
# ---------------------------------------------------------------------------


@pytest.fixture
def rising_equity():
    """A series with a real drawdown, so the drawdown fields are exercised."""
    equity = [100_000.0]
    for i in range(250):
        equity.append(equity[-1] * (1 + math.sin(i * 0.3) * 0.004 + 0.0004))
    return equity


def test_performance_metrics_come_from_the_engine(client, rising_equity):
    body = client.post(
        "/analytics/performance", json={"equity": rising_equity}
    ).json()

    # `periods` counts equity observations, not returns.
    assert body["periods"] == len(rising_equity)
    assert body["cumulative_return"] > 0
    assert body["annualized_volatility"] > 0
    assert body["max_drawdown"] > 0
    assert body["max_drawdown_periods"] > 0
    # Sortino uses downside deviation only, so it exceeds Sharpe on a series
    # whose losses are smaller than its total variability.
    assert body["sortino"] > body["sharpe"]


def test_cumulative_return_matches_the_series(client, rising_equity):
    """A cross-check against the input, so a wrong field mapping is caught."""
    body = client.post("/analytics/performance", json={"equity": rising_equity}).json()
    expected = rising_equity[-1] / rising_equity[0] - 1
    assert body["cumulative_return"] == pytest.approx(expected, rel=1e-9)
    assert body["initial_equity"] == pytest.approx(rising_equity[0])
    assert body["final_equity"] == pytest.approx(rising_equity[-1])


def test_trade_fields_are_zero_without_trades(client, rising_equity):
    """An equity series cannot distinguish a round trip from a mark.

    The engine reports zero rather than inferring trades, and the UI says so
    rather than presenting the zeros as results.
    """
    body = client.post("/analytics/performance", json={"equity": rising_equity}).json()
    assert body["trades"] == 0
    assert body["win_rate"] == 0.0
    assert body["profit_factor"] == 0.0


def test_a_single_observation_is_refused(client):
    """One point is not a series; a metric derived from it would be fiction."""
    assert client.post("/analytics/performance", json={"equity": [100.0]}).status_code == 422


def test_a_non_finite_equity_value_is_refused_at_the_binding():
    """JSON cannot carry an infinity, so HTTP can never deliver one.

    The guard still matters: a programmatic caller using the bindings directly
    can, and a NaN in an equity series would silently poison every statistic
    derived from it.
    """
    with pytest.raises(RuntimeError, match="non-finite"):
        ptl.performance_metrics([100.0, float("nan"), 101.0])


def test_a_flat_series_has_no_drawdown_and_no_return(client):
    body = client.post(
        "/analytics/performance", json={"equity": [100.0] * 50}
    ).json()
    assert body["cumulative_return"] == pytest.approx(0.0)
    assert body["max_drawdown"] == pytest.approx(0.0)
    # Volatility of a constant series is zero, and Sharpe must not be an
    # infinity dressed up as a number.
    assert body["annualized_volatility"] == pytest.approx(0.0)
    assert math.isfinite(body["sharpe"])


def test_performance_is_deterministic(client, rising_equity):
    payload = {"equity": rising_equity}
    assert (
        client.post("/analytics/performance", json=payload).json()
        == client.post("/analytics/performance", json=payload).json()
    )


def test_performance_does_not_perturb_determinism(client, monkeypatch, rising_equity):
    import pathlib

    monkeypatch.chdir(pathlib.Path(__file__).resolve().parents[3])
    before = client.get("/fingerprints").json()
    client.post("/analytics/performance", json={"equity": rising_equity})
    assert client.get("/fingerprints").json() == before
