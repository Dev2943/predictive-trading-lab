"""Health, version, determinism and configuration."""

from __future__ import annotations

from fastapi import APIRouter, HTTPException, Query, status

from ..dependencies import Engine, MaybeEngine
from ..engine import EngineClient, EngineError
from ..models.schemas import (
    ArtifactEnvelope,
    ErrorDetail,
    Fingerprints,
    HealthResponse,
    SystemInfo,
    VersionInfo,
)

router = APIRouter(tags=["system"])

GATEWAY_VERSION = "0.2.0"

# The v1.0 reference build.
REFERENCE_CONFIG_HASH = "30b44e5972450aad"
REFERENCE_RNG = ["d05ef55272cdfb14", "2e2f422341add64e", "1c120f3d1ce63170"]


def _health(engine: EngineClient | None) -> HealthResponse:
    if engine is None:
        return HealthResponse(
            status="unavailable",
            engine_reachable=False,
            detail="the engine could not be reached",
        )
    return HealthResponse(status="ok", engine_reachable=True)


@router.get(
    "/health",
    response_model=HealthResponse,
    summary="Liveness and engine reachability",
    description=(
        "Deliberately does not use the engine dependency: that would turn an "
        "unreachable engine into a 503 on the health endpoint itself, which is "
        "the one endpoint that must still answer when things are broken."
    ),
)
def health(engine: MaybeEngine) -> HealthResponse:
    return _health(engine)


@router.get(
    "/version",
    response_model=VersionInfo,
    responses={503: {"model": ErrorDetail, "description": "Engine unreachable"}},
    summary="Engine and gateway versions",
)
def version(engine: Engine) -> VersionInfo:
    return VersionInfo(**engine.version(), gateway_version=GATEWAY_VERSION)


@router.get(
    "/fingerprints",
    response_model=Fingerprints,
    responses={
        400: {"model": ErrorDetail, "description": "Configuration could not be read"},
        503: {"model": ErrorDetail, "description": "Engine unreachable"},
    },
    summary="Determinism fingerprints",
    description=(
        "The same values `ptl_version` prints. A client can show whether the "
        "running engine reproduces the v1.0 reference build."
    ),
)
def fingerprints(
    engine: Engine,
    seed: int = Query(default=20240101, description="RNG seed to fingerprint."),
    config_path: str = Query(default="config/base.toml"),
) -> Fingerprints:
    try:
        rng = engine.rng_fingerprint(seed, 3)
        config = engine.config_hash(config_path)
    except EngineError as exc:
        raise HTTPException(
            status_code=status.HTTP_400_BAD_REQUEST, detail=str(exc)
        ) from exc
    return Fingerprints(
        config_hash=config,
        rng=rng,
        matches_reference=config == REFERENCE_CONFIG_HASH and rng == REFERENCE_RNG,
    )


@router.get(
    "/system",
    response_model=SystemInfo,
    summary="Health, version and fingerprints in one call",
    description=(
        "Aggregated so a client renders a status bar with one request rather "
        "than three. Fingerprints are omitted rather than faked when the "
        "configuration cannot be read."
    ),
)
def system(engine: Engine) -> SystemInfo:
    health_response = _health(engine)
    version_response = VersionInfo(**engine.version(), gateway_version=GATEWAY_VERSION)

    prints: Fingerprints | None = None
    try:
        rng = engine.rng_fingerprint(20240101, 3)
        config = engine.config_hash("config/base.toml")
        prints = Fingerprints(
            config_hash=config,
            rng=rng,
            matches_reference=config == REFERENCE_CONFIG_HASH and rng == REFERENCE_RNG,
        )
    except EngineError:
        # Omitted, not zeroed. A fabricated hash would read as a real mismatch.
        prints = None

    return SystemInfo(
        health=health_response, version=version_response, fingerprints=prints
    )


@router.get(
    "/config",
    response_model=ArtifactEnvelope,
    responses={400: {"model": ErrorDetail}},
    summary="Configuration hash for a config file",
    description=(
        "Returns the hash the engine computes, not the file contents. The "
        "gateway does not parse TOML: doing so would be a second reader that "
        "could disagree with the engine's."
    ),
)
def config(
    engine: Engine,
    path: str = Query(default="config/base.toml"),
) -> ArtifactEnvelope:
    try:
        digest = engine.config_hash(path)
    except EngineError as exc:
        raise HTTPException(
            status_code=status.HTTP_400_BAD_REQUEST, detail=str(exc)
        ) from exc
    return ArtifactEnvelope(available=True, key=path, data={"config_hash": digest})
