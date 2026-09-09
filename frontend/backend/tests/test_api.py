"""API tests against a fake engine.

These require no compiled extension module. The integration suite covers the
real engine separately.
"""

from __future__ import annotations

import pytest
from fastapi.testclient import TestClient

from app.dependencies import get_engine
from app.main import app

from .fakes import REFERENCE_CONFIG_HASH, REFERENCE_RNG, SESSION_STATE, FakeEngineClient


def make_client(engine: FakeEngineClient) -> TestClient:
    app.dependency_overrides[get_engine] = lambda: engine
    return TestClient(app)


@pytest.fixture
def engine() -> FakeEngineClient:
    return FakeEngineClient(artifacts={"paper/demo/state": SESSION_STATE})


@pytest.fixture
def client(engine: FakeEngineClient):
    with make_client(engine) as test_client:
        yield test_client
    app.dependency_overrides.clear()


# ---------------------------------------------------------------------------
# The read-only claim
# ---------------------------------------------------------------------------


def test_every_endpoint_is_a_get():
    """THE LOAD-BEARING TEST for F2.

    Read from the OPENAPI SCHEMA, not from `app.routes`. FastAPI wraps included
    routers in `_IncludedRouter` objects that are not flattened until the schema
    is built, so a scan of `app.routes` would never see the router endpoints and
    would pass whatever verbs they used -- a guard that assures nothing.
    """
    schema = app.openapi()
    verbs = {verb for operations in schema["paths"].values() for verb in operations}
    assert verbs == {"get"}
    # And the endpoints really are there, so the assertion above is not vacuous.
    assert len(schema["paths"]) >= 15


def test_the_documented_surface_is_present():
    paths = set(app.openapi()["paths"])
    for expected in (
        "/health",
        "/version",
        "/system",
        "/config",
        "/diagnostics",
        "/metrics",
        "/portfolio",
        "/positions",
        "/analytics",
        "/risk",
        "/optimization",
        "/reports",
    ):
        assert expected in paths


def test_every_endpoint_has_a_response_model_and_summary():
    """OpenAPI must be complete: an endpoint with no described response is one a
    client author has to read the source to use."""
    schema = app.openapi()
    for path, operations in schema["paths"].items():
        for verb, operation in operations.items():
            assert operation.get("summary"), f"{verb} {path} has no summary"
            ok = operation["responses"]["200"]
            assert "content" in ok, f"{verb} {path} has no response schema"


# ---------------------------------------------------------------------------
# System
# ---------------------------------------------------------------------------


def test_health_reports_reachable(client):
    body = client.get("/health").json()
    assert body["status"] == "ok"
    assert body["engine_reachable"] is True


def test_version(client):
    body = client.get("/version").json()
    assert body["engine_version"] == "1.0.0"
    assert body["transport"] == "in-process"
    assert body["gateway_version"]


def test_fingerprints_match_reference(client):
    body = client.get("/fingerprints").json()
    assert body["rng"] == REFERENCE_RNG
    assert body["config_hash"] == REFERENCE_CONFIG_HASH
    assert body["matches_reference"] is True


def test_a_reference_mismatch_is_visible(client):
    body = client.get("/fingerprints?seed=999").json()
    assert body["matches_reference"] is False


def test_an_unreadable_config_is_400(client):
    response = client.get("/fingerprints?config_path=nope.txt")
    assert response.status_code == 400
    # The ENGINE's message, not a generic one.
    assert "config load" in response.json()["detail"]


def test_system_aggregates_without_extra_round_trips(client):
    body = client.get("/system").json()
    assert body["health"]["status"] == "ok"
    assert body["version"]["engine_version"] == "1.0.0"
    assert body["fingerprints"]["matches_reference"] is True


def test_system_omits_fingerprints_rather_than_faking_them():
    """A fabricated hash would read as a real mismatch."""
    engine = FakeEngineClient(config_raises=True)
    with make_client(engine) as client:
        body = client.get("/system").json()
    app.dependency_overrides.clear()
    assert body["fingerprints"] is None
    assert body["version"]["engine_version"] == "1.0.0"


def test_config_returns_the_hash_not_the_file(client):
    body = client.get("/config").json()
    assert body["available"] is True
    assert body["data"]["config_hash"] == REFERENCE_CONFIG_HASH


# ---------------------------------------------------------------------------
# Capabilities
# ---------------------------------------------------------------------------


def test_optimization_lists_nine_optimizers(client):
    body = client.get("/optimization").json()
    assert body["count"] == 9
    names = {o["name"] for o in body["optimizers"]}
    assert "risk_parity" in names

    by_name = {o["name"]: o for o in body["optimizers"]}
    # Requirements mirror the engine's own refusals.
    assert by_name["minimum_variance"]["requires_covariance"] is True
    assert by_name["equal_weight"]["requires_covariance"] is False
    assert by_name["max_sharpe"]["requires_expected_returns"] is True


def test_analytics_and_risk_describe_rather_than_compute(client):
    analytics = client.get("/analytics").json()
    assert "sharpe" in analytics["rolling_metrics"]
    assert "request body" in analytics["note"]

    risk = client.get("/risk").json()
    assert "max_drawdown_pct" in risk["validated_limits"]
    # The absence of live risk state is stated, not implied by zeros.
    assert "session" in risk["note"]


# ---------------------------------------------------------------------------
# Artifacts
# ---------------------------------------------------------------------------


def test_portfolio_from_a_persisted_snapshot(client):
    body = client.get("/portfolio?session=demo").json()
    assert body["available"] is True
    assert body["session_id"] == "demo"
    assert body["account"]["equity"] == 101250.5
    assert body["events_processed"] == 120


def test_a_missing_portfolio_is_absent_not_empty(client):
    """`available: false`, not a zeroed account.

    An empty portfolio and a portfolio worth nothing render identically, and
    only one of them is true.
    """
    body = client.get("/portfolio?session=never_ran").json()
    assert body["available"] is False
    assert body["account"] is None
    assert "no session state" in body["detail"]


def test_absent_account_fields_stay_null():
    """A snapshot from an older session may lack fields. Inventing a zero would
    put a number on screen no session ever recorded."""
    engine = FakeEngineClient(
        artifacts={"paper/old/state": {"session_id": "old", "account": {"cash": 10.0}}}
    )
    with make_client(engine) as client:
        body = client.get("/portfolio?session=old").json()
    app.dependency_overrides.clear()
    assert body["account"]["cash"] == 10.0
    assert body["account"]["equity"] is None


def test_positions_are_served(client):
    body = client.get("/positions?session=demo").json()
    assert body["available"] is True
    assert len(body["positions"]) == 2
    # A short position keeps its sign.
    assert body["positions"][1]["quantity"] == -25.0


def test_missing_positions_are_absent(client):
    body = client.get("/positions?session=never_ran").json()
    assert body["available"] is False
    assert body["positions"] == []


def test_an_invalid_mode_is_422(client):
    assert client.get("/portfolio?mode=production").status_code == 422


def test_a_corrupt_artifact_is_500_not_a_silent_empty():
    """Absence and corruption are different. Absence is normal; a file that
    exists and cannot be parsed is a fault the caller must see."""
    engine = FakeEngineClient(artifact_raises=True)
    with make_client(engine) as client:
        response = client.get("/portfolio?session=demo")
    app.dependency_overrides.clear()
    assert response.status_code == 500
    assert "could not be read" in response.json()["detail"]


def test_artifacts_are_listed_sorted(client):
    body = client.get("/artifacts?prefix=paper").json()
    assert body["keys"] == sorted(body["keys"])
    assert body["count"] == len(body["keys"])


def test_a_missing_report_is_404():
    """Unlike portfolio state, a report requested by id is a specific thing the
    caller believes exists, so absence is an error."""
    engine = FakeEngineClient()
    with make_client(engine) as client:
        response = client.get("/reports/does_not_exist")
    app.dependency_overrides.clear()
    assert response.status_code == 404
    assert "does_not_exist" in response.json()["detail"]


def test_a_present_report_is_served():
    engine = FakeEngineClient(
        artifacts={"experiments/exp1/result": {"experiment_id": "exp1", "sharpe": 1.4}}
    )
    with make_client(engine) as client:
        body = client.get("/reports/exp1").json()
    app.dependency_overrides.clear()
    assert body["available"] is True
    assert body["data"]["sharpe"] == 1.4


def test_diagnostics_and_metrics_report_absence_cleanly(client):
    for path in ("/diagnostics", "/metrics"):
        body = client.get(path).json()
        assert body["available"] is False
        assert body["data"] is None
        assert body["detail"]


# ---------------------------------------------------------------------------
# Engine unreachable
# ---------------------------------------------------------------------------


def test_health_still_answers_when_the_engine_is_unreachable():
    """The one endpoint that must work when everything else does not."""
    from fastapi import HTTPException

    from app.dependencies import probe_engine

    def unavailable():
        raise HTTPException(status_code=503, detail="bindings not importable")

    # Both are overridden: /health uses the probe (which may return None) and
    # every other route uses get_engine (which raises).
    app.dependency_overrides[get_engine] = unavailable
    app.dependency_overrides[probe_engine] = lambda: None
    with TestClient(app) as client:
        body = client.get("/health").json()
        version = client.get("/version")
    app.dependency_overrides.clear()

    assert body["status"] == "unavailable"
    assert body["engine_reachable"] is False
    # Other endpoints correctly report 503 rather than pretending.
    assert version.status_code == 503
