"""The engine abstraction.

THE WEB LAYER NEVER SEES THIS, AND THAT IS THE POINT.

Routes depend on the `EngineClient` protocol, never on the `ptl` module. Today
the only implementation runs the engine in-process through pybind11. Tomorrow a
`RemoteEngineClient` speaks HTTP to an engine on another machine, and nothing
above this file changes -- not a route, not a model, not a line of TypeScript.

That is only possible because the binding boundary is JSON: both implementations
return the same parsed dictionaries, because both read the same serializers
inside the engine.
"""

from __future__ import annotations

import json
import pathlib
from typing import Any, Protocol, runtime_checkable


class EngineUnavailable(RuntimeError):
    """The engine could not be reached.

    Distinct from a computation that failed: unavailable means we never got an
    answer, and the caller should retry or report an outage rather than treat
    the absence as a result.
    """


class EngineError(RuntimeError):
    """The engine was reached and refused.

    Carries the engine's own message. Refusals are meaningful -- an optimizer
    that cannot answer says why -- so the text is surfaced rather than replaced.
    """


@runtime_checkable
class EngineClient(Protocol):
    """What a route may ask of the engine.

    Note what is ABSENT: nothing here starts, steps or mutates a session. Those
    arrive behind a single-writer command queue in a later phase, never as a
    direct call from a request handler.
    """

    def version(self) -> dict[str, Any]: ...
    def rng_fingerprint(self, seed: int, count: int) -> list[str]: ...
    def config_hash(self, path: str) -> str: ...
    def optimizer_names(self) -> list[str]: ...
    def optimize(self, **kwargs: Any) -> dict[str, Any]: ...
    def estimate_covariance(self, **kwargs: Any) -> dict[str, Any]: ...
    def rolling_metrics(self, **kwargs: Any) -> dict[str, Any]: ...
    def factor_contribution(
        self, portfolio: list[float], benchmark: list[float]
    ) -> dict[str, Any]: ...
    def validate_risk_limits(self, **kwargs: Any) -> dict[str, Any]: ...
    def read_artifact(self, key: str) -> dict[str, Any] | None: ...
    def list_artifacts(self, prefix: str) -> list[str]: ...


class InProcessEngineClient:
    """Runs the engine in this process via the bindings.

    READ-ONLY. It calls stateless computation and reads files the engine
    previously wrote. It holds no session, so there is nothing here that could
    mutate trading state even by mistake.
    """

    def __init__(self, artifact_root: str | pathlib.Path = "results") -> None:
        try:
            import ptl
        except ImportError as exc:  # pragma: no cover - environment dependent
            raise EngineUnavailable(
                "the ptl bindings are not importable; build "
                "frontend/backend/bindings and put the module on PYTHONPATH"
            ) from exc

        self._ptl = ptl
        self._artifact_root = pathlib.Path(artifact_root)

    def version(self) -> dict[str, Any]:
        return {
            "engine_version": self._ptl.engine_version,
            "compiler": self._ptl.compiler,
            "build_type": self._ptl.build_type,
            "transport": "in-process",
        }

    def rng_fingerprint(self, seed: int, count: int = 3) -> list[str]:
        return list(self._ptl.rng_fingerprint(seed, count))

    def config_hash(self, path: str) -> str:
        return self._call(self._ptl.config_hash, path)

    def optimizer_names(self) -> list[str]:
        return list(self._ptl.optimizer_names())

    def optimize(self, **kwargs: Any) -> dict[str, Any]:
        # Parsed here rather than in the route, so a remote client can hand over
        # the identical string and routes stay transport-agnostic.
        return json.loads(self._call(self._ptl.optimize, **kwargs))

    def estimate_covariance(self, **kwargs: Any) -> dict[str, Any]:
        return dict(self._call(self._ptl.estimate_covariance, **kwargs))

    def rolling_metrics(self, **kwargs: Any) -> dict[str, Any]:
        return dict(self._call(self._ptl.rolling_metrics, **kwargs))

    def factor_contribution(
        self, portfolio: list[float], benchmark: list[float]
    ) -> dict[str, Any]:
        return dict(self._call(self._ptl.factor_contribution, portfolio, benchmark))

    def validate_risk_limits(self, **kwargs: Any) -> dict[str, Any]:
        return json.loads(self._call(self._ptl.validate_risk_limits, **kwargs))

    def read_artifact(self, key: str) -> dict[str, Any] | None:
        """Read a JSON artifact a session previously wrote.

        Returns None when absent. Absence is legitimate -- no session has run
        yet -- and is reported as such rather than as an empty portfolio, which
        renders identically to a portfolio worth nothing.
        """
        path = self._safe_path(key)
        if path is None or not path.is_file():
            return None
        try:
            return json.loads(path.read_text())
        except (OSError, json.JSONDecodeError) as exc:
            raise EngineError(f"artifact {key} could not be read: {exc}") from exc

    def list_artifacts(self, prefix: str) -> list[str]:
        base = self._safe_path(prefix, allow_dir=True)
        if base is None or not base.is_dir():
            return []
        keys = [
            str(p.relative_to(self._artifact_root).with_suffix(""))
            for p in base.rglob("*.json")
        ]
        # Sorted: directory order is filesystem-defined, so two calls would
        # otherwise disagree.
        return sorted(keys)

    def _safe_path(self, key: str, *, allow_dir: bool = False) -> pathlib.Path | None:
        """Resolve an artifact key, refusing anything that escapes the root.

        A key containing '..' is a bug or an attack, and quietly rewriting it
        hides which. Refused rather than sanitised, matching the engine's own
        ArtifactStore.
        """
        if not key or key.startswith("/") or ".." in key:
            return None
        path = self._artifact_root / key
        if not allow_dir:
            path = path.with_suffix(".json")
        try:
            path.resolve().relative_to(self._artifact_root.resolve())
        except (ValueError, OSError):
            return None
        return path

    @staticmethod
    def _call(fn: Any, *args: Any, **kwargs: Any) -> Any:
        """Translate a binding refusal into an EngineError.

        Wrapping once here means every route reports refusals the same way, and
        the engine's reasoning reaches the user instead of a generic 500.
        """
        try:
            return fn(*args, **kwargs)
        except RuntimeError as exc:
            raise EngineError(str(exc)) from exc
