"""Dependency wiring.

One place decides which EngineClient the application uses. Routes ask for the
protocol and get whatever this returns, so swapping in a remote engine later is
a change to this file alone.
"""

from __future__ import annotations

import functools
import os
from typing import Annotated

from fastapi import Depends, HTTPException, status

from .engine import EngineClient, EngineUnavailable, InProcessEngineClient


@functools.lru_cache(maxsize=1)
def _client() -> EngineClient:
    # Cached because constructing it imports the extension module. The client
    # holds no session state, so sharing one across requests is safe -- and
    # remains safe when a remote client replaces it.
    return InProcessEngineClient(artifact_root=os.environ.get("PTL_RESULTS", "results"))


def get_engine() -> EngineClient:
    try:
        return _client()
    except EngineUnavailable as exc:
        # 503, not 500: the gateway is fine and the engine is not reachable. A
        # frontend can distinguish "retry later" from "this request was wrong".
        raise HTTPException(
            status_code=status.HTTP_503_SERVICE_UNAVAILABLE, detail=str(exc)
        ) from exc


Engine = Annotated[EngineClient, Depends(get_engine)]
