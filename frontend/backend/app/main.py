"""FastAPI gateway for the Predictive Trading Lab engine.

F1 IS INFRASTRUCTURE ONLY. The application exposes health, version and
determinism fingerprints -- enough to prove the wiring works end to end. The
full read-only surface arrives in F2 and the command queue in F3.

The gateway is the web layer's ONLY contact with the engine. Nothing above this
package imports `ptl`, so the engine can move to another process or machine by
swapping the EngineClient implementation.
"""

from __future__ import annotations

from fastapi import APIRouter, FastAPI, HTTPException, Query, status
from fastapi.middleware.cors import CORSMiddleware

from .dependencies import Engine, get_engine
from .engine import EngineError
from .models.schemas import Fingerprints, HealthResponse, VersionInfo

GATEWAY_VERSION = "0.1.0"

# The v1.0 reference build. A frontend showing a mismatch is showing that this
# engine does not reproduce the reference -- worth knowing before anyone trusts
# a backtest from it.
REFERENCE_CONFIG_HASH = "30b44e5972450aad"
REFERENCE_RNG = ["d05ef55272cdfb14", "2e2f422341add64e", "1c120f3d1ce63170"]

app = FastAPI(
    title="Predictive Trading Lab API",
    version=GATEWAY_VERSION,
    description=(
        "Gateway over the Predictive Trading Lab C++ engine.\n\n"
        "The engine is the single source of truth. This layer serializes what "
        "the engine computes and never recomputes it."
    ),
)

# Restricted to the Next.js dev origin rather than "*": a wildcard on an API
# that will later place orders is a habit worth not forming.
app.add_middleware(
    CORSMiddleware,
    allow_origins=["http://localhost:3000", "http://127.0.0.1:3000"],
    allow_credentials=True,
    allow_methods=["GET", "POST"],
    allow_headers=["*"],
)

router = APIRouter(tags=["system"])


@router.get("/health", response_model=HealthResponse)
def health() -> HealthResponse:
    """Liveness plus engine reachability.

    Deliberately does NOT use the Engine dependency: that would turn an
    unreachable engine into a 503 on the health endpoint itself, which is the
    one endpoint that must still answer when things are broken.
    """
    try:
        get_engine()
    except HTTPException as exc:
        return HealthResponse(
            status="unavailable", engine_reachable=False, detail=str(exc.detail)
        )
    return HealthResponse(status="ok", engine_reachable=True)


@router.get("/version", response_model=VersionInfo)
def version(engine: Engine) -> VersionInfo:
    return VersionInfo(**engine.version(), gateway_version=GATEWAY_VERSION)


@router.get("/fingerprints", response_model=Fingerprints)
def fingerprints(
    engine: Engine,
    seed: int = Query(default=20240101),
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


@router.get("/")
def root() -> dict[str, str]:
    return {
        "service": "predictive-trading-lab-api",
        "docs": "/docs",
        "phase": "F1 (infrastructure only)",
    }


app.include_router(router)
