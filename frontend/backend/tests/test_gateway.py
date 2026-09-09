"""Gateway tests for F1.

F1 is infrastructure only, so the surface is small: health, version and
fingerprints. The point of these tests is to prove the wiring works end to end
and that going through HTTP does not perturb the engine.
"""

from __future__ import annotations

import pytest
from fastapi.testclient import TestClient

from app.main import app

REFERENCE_RNG = ["d05ef55272cdfb14", "2e2f422341add64e", "1c120f3d1ce63170"]
REFERENCE_CONFIG_HASH = "30b44e5972450aad"


@pytest.fixture
def client():
    with TestClient(app) as test_client:
        yield test_client


def test_health_reports_engine_reachable(client):
    body = client.get("/health").json()
    assert body["status"] == "ok"
    assert body["engine_reachable"] is True


def test_version_reports_engine_and_gateway(client):
    body = client.get("/version").json()
    assert body["engine_version"].startswith("1.0")
    assert body["gateway_version"]
    # The transport is declared, so a client can show whether the engine is
    # local without having to infer it.
    assert body["transport"] == "in-process"


def test_fingerprints_match_the_reference_build(client, monkeypatch):
    import pathlib

    monkeypatch.chdir(pathlib.Path(__file__).resolve().parents[3])
    body = client.get("/fingerprints").json()
    assert body["rng"] == REFERENCE_RNG
    assert body["config_hash"] == REFERENCE_CONFIG_HASH
    assert body["matches_reference"] is True


def test_going_through_http_does_not_perturb_determinism(client, monkeypatch):
    """The gateway observes; it does not change anything.

    If exercising the routes moved the fingerprints, the read-only claim would
    be false and nothing above this layer could be trusted.
    """
    import pathlib

    monkeypatch.chdir(pathlib.Path(__file__).resolve().parents[3])
    before = client.get("/fingerprints").json()
    client.get("/version")
    client.get("/health")
    client.get("/")
    after = client.get("/fingerprints").json()
    assert before == after


def test_a_reference_mismatch_is_visible(client, monkeypatch):
    """A client must be able to see that this engine does not reproduce the
    reference build."""
    import pathlib

    monkeypatch.chdir(pathlib.Path(__file__).resolve().parents[3])
    body = client.get("/fingerprints?seed=999").json()
    assert body["rng"] != REFERENCE_RNG
    assert body["matches_reference"] is False


def test_a_missing_config_is_a_client_error(client):
    assert client.get("/fingerprints?config_path=does/not/exist.toml").status_code == 400


def test_no_mutating_verbs_are_exposed():
    """F1 exposes nothing that could change engine state."""
    methods = {
        method
        for route in app.routes
        for method in getattr(route, "methods", set())
        if method not in ("HEAD", "OPTIONS")
    }
    assert methods == {"GET"}
