"""Wire models.

These mirror the engine's JSON rather than redefining it. Where the engine names
a field, that name is kept -- a gateway that renames things forces every reader
to hold two vocabularies and makes an engine change look like a breaking API
change when it is not.
"""

from __future__ import annotations

from typing import Literal

from pydantic import BaseModel, Field


class VersionInfo(BaseModel):
    engine_version: str
    compiler: str
    build_type: str
    transport: Literal["in-process", "remote"] = Field(
        description="Declared so a client can see whether the engine is local."
    )
    gateway_version: str


class Fingerprints(BaseModel):
    """Determinism fingerprints, read through the engine.

    Exposed so a client can show whether the running engine reproduces the
    reference build -- the same check the CLI performs.
    """

    config_hash: str
    rng: list[str]
    matches_reference: bool


class HealthResponse(BaseModel):
    status: Literal["ok", "degraded", "unavailable"]
    engine_reachable: bool
    detail: str = ""
