"""FastAPI gateway for the Predictive Trading Lab engine.

READ-ONLY, GET ONLY. No endpoint mutates engine state, and no verb that implies
mutation is routed. A test asserts the routing table contains nothing but GET.

The gateway is the web layer's ONLY contact with the engine. Nothing above this
package imports `ptl`: routes depend on the `EngineClient` protocol, so moving
the engine to another process or machine is a change to `dependencies.py`
alone.
"""

from __future__ import annotations

from fastapi import FastAPI
from fastapi.middleware.cors import CORSMiddleware

from .routers import artifacts, capabilities, system

app = FastAPI(
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
    ],
)

# Restricted to the Next.js dev origin rather than "*": a wildcard on an API
# that will later place orders is a habit worth not forming.
app.add_middleware(
    CORSMiddleware,
    allow_origins=["http://localhost:3000", "http://127.0.0.1:3000"],
    allow_credentials=True,
    allow_methods=["GET"],
    allow_headers=["*"],
)

app.include_router(system.router)
app.include_router(capabilities.router)
app.include_router(artifacts.router)


@app.get("/", tags=["system"], summary="Service banner")
def root() -> dict[str, str]:
    return {
        "service": "predictive-trading-lab-api",
        "docs": "/docs",
        "openapi": "/openapi.json",
        "phase": "F2 (read-only)",
    }
