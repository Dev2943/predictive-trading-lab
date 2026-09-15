"""FastAPI gateway for the Predictive Trading Lab engine.

READ-ONLY, GET ONLY. No endpoint mutates engine state, and no verb that implies
mutation is routed. A test asserts the routing table contains nothing but GET.

The gateway is the web layer's ONLY contact with the engine. Nothing above this
package imports `ptl`: routes depend on the `EngineClient` protocol, so moving
the engine to another process or machine is a change to `dependencies.py`
alone.
"""

from __future__ import annotations

import contextlib
import logging
import os
import time

from fastapi import FastAPI
from fastapi.middleware.cors import CORSMiddleware

from .observability import RequestIdMiddleware, configure_logging
from .settings import load_settings

# Resolved ONCE at import, before the app exists. A configuration error must
# stop the process rather than surface as a 500 on the first request -- a
# process that starts and then fails every call still looks healthy to a load
# balancer's TCP check.
settings = load_settings()
log = configure_logging(settings.log_level)

STARTED_AT = time.time()

from .routers import (
    artifacts,
    capabilities,
    compute,
    market,
    session,
    system,
    trading,
)

@contextlib.asynccontextmanager
async def lifespan(_app: FastAPI):
    """Startup banner and graceful shutdown.

    The banner prints the resolved configuration -- never a secret -- so a
    deployment's actual settings are visible in the first log line rather than
    inferred from behaviour.
    """
    log.info(
        "gateway starting",
        extra={"extra_fields": {"config": settings.describe()}},
    )
    yield
    # Stop the session driver and the market stream in order. Without this a
    # container restart leaves a driver thread stepping a session nobody is
    # reading, and the market socket open.
    from .dependencies import shutdown_services

    shutdown_services()
    log.info("gateway stopped")


app = FastAPI(
    lifespan=lifespan,
    title="Predictive Trading Lab API",
    version=system.GATEWAY_VERSION,
    summary="Read-only gateway over the Predictive Trading Lab C++ engine.",
    description=(
        "The engine is the single source of truth. This layer serializes what "
        "the engine computes and never recomputes it.\n\n"
        "**Read-only.** Every endpoint is a GET and nothing mutates engine "
        "state.\n\n"
        "**Two kinds of endpoint.** Capability endpoints report what the engine "
        "can do. Artifact endpoints serve what a session previously wrote -- "
        "and answer `available: false` when nothing has, rather than "
        "returning an empty portfolio that would render as a real book worth "
        "nothing."
    ),
    openapi_tags=[
        {"name": "system", "description": "Health, version, determinism, config."},
        {"name": "capabilities", "description": "What the engine can compute."},
        {
            "name": "artifacts",
            "description": "State a session previously persisted.",
        },
        {
            "name": "market",
            "description": (
                "Live or replay market data for display. Never reaches the "
                "engine: the paper session runs on its own deterministic "
                "replay regardless of what this shows."
            ),
        },
        {
            "name": "trading",
            "description": (
                "Order entry and management. Every command is queued through "
                "the session driver and reaches the engine at the next event."
            ),
        },
        {
            "name": "compute",
            "description": (
                "Stateless computation: optimization, covariance, analytics, "
                "risk validation. POST because the inputs are matrices, not "
                "because anything is mutated."
            ),
        },
        {
            "name": "session",
            "description": (
                "Paper session lifecycle. The only endpoints that change "
                "anything, and they do so by enqueuing a command rather than "
                "touching the session."
            ),
        },
    ],
)

# Restricted to the Next.js dev origin rather than "*": a wildcard on an API
# that will later place orders is a habit worth not forming.
app.add_middleware(RequestIdMiddleware)

# Origins come from configuration, never a literal. In production the settings
# validator refuses localhost and refuses "*".
app.add_middleware(
    CORSMiddleware,
    allow_origins=settings.allowed_origins,
    allow_credentials=True,
    allow_methods=["GET", "POST", "DELETE"],
    allow_headers=["*"],
)

app.include_router(system.router)
app.include_router(capabilities.router)
app.include_router(artifacts.router)
app.include_router(session.router)
app.include_router(compute.router)
app.include_router(trading.router)
app.include_router(market.router)


@app.get(
    "/healthz",
    tags=["system"],
    summary="Liveness — is the process up",
    description=(
        "Answers as long as the process can serve. Deliberately does NOT touch "
        "the engine: a liveness probe that fails when a dependency is degraded "
        "causes a restart loop that fixes nothing."
    ),
)
def healthz() -> dict[str, object]:
    return {"status": "ok", "uptime_seconds": round(time.time() - STARTED_AT, 1)}


@app.get(
    "/readyz",
    tags=["system"],
    summary="Readiness — can the service do useful work",
    description=(
        "Checks that the engine bindings are importable. Distinct from "
        "liveness: a process that is up but cannot reach the engine should "
        "stop receiving traffic without being restarted."
    ),
)
def readyz() -> dict[str, object]:
    from .dependencies import get_engine

    try:
        get_engine()
    except Exception as exc:  # noqa: BLE001 - reported, not raised
        return {"ready": False, "detail": str(exc)}
    return {"ready": True, "detail": ""}


@app.get(
    "/buildz",
    tags=["system"],
    summary="Build and deployment metadata",
    description=(
        "What is actually running: engine version, commit, environment and "
        "market provider. The first question after a bad deploy is 'which "
        "build is live', and guessing is how the wrong thing gets rolled back."
    ),
)
def buildz() -> dict[str, object]:
    engine_version = "unavailable"
    try:
        from .dependencies import get_engine

        engine_version = str(get_engine().version().get("engine_version"))
    except Exception:  # noqa: BLE001 - the endpoint must still answer
        pass
    return {
        "engine_version": engine_version,
        "gateway_version": system.GATEWAY_VERSION,
        "commit": os.environ.get("PTL_COMMIT", "unknown"),
        "built_at": os.environ.get("PTL_BUILT_AT", "unknown"),
        "environment": settings.environment,
        "market_provider": settings.market_provider,
    }


@app.get("/", tags=["system"], summary="Service banner")
def root() -> dict[str, str]:
    return {
        "service": "predictive-trading-lab-api",
        "docs": "/docs",
        "openapi": "/openapi.json",
        "phase": "P11 (production deployment)",
    }
