"""A fake EngineClient.

WHY THE API TESTS DO NOT USE THE REAL ENGINE.

Every route depends on the `EngineClient` protocol, so a fake satisfies them
completely. That buys three things:

  - API tests run without a compiled extension module, so a contributor working
    on routes needs no C++ toolchain.
  - Failure paths are reachable. Making the real engine return a corrupt
    artifact or go unreachable is difficult; making a fake do it is one line.
  - A test failure localises. If the API suite passes against the fake and the
    integration suite fails against the engine, the fault is in the bindings,
    not the routes.

The separate integration tests still exercise the real engine, so the fake
cannot drift into describing an engine that does not exist.
"""

from __future__ import annotations

from typing import Any

from app.engine import EngineError

REFERENCE_RNG = ["d05ef55272cdfb14", "2e2f422341add64e", "1c120f3d1ce63170"]
REFERENCE_CONFIG_HASH = "30b44e5972450aad"


class FakeEngineClient:
    """Satisfies the EngineClient protocol with scripted answers."""

    def __init__(
        self,
        *,
        artifacts: dict[str, dict[str, Any]] | None = None,
        config_raises: bool = False,
        artifact_raises: bool = False,
    ) -> None:
        self._artifacts = artifacts or {}
        self._config_raises = config_raises
        self._artifact_raises = artifact_raises
        self.calls: list[str] = []

    def version(self) -> dict[str, Any]:
        self.calls.append("version")
        return {
            "engine_version": "1.0.0",
            "compiler": "GNU",
            "build_type": "Release",
            "transport": "in-process",
        }

    def rng_fingerprint(self, seed: int, count: int = 3) -> list[str]:
        self.calls.append("rng_fingerprint")
        if seed == 20240101:
            return REFERENCE_RNG[:count]
        return [f"{seed:016x}"] * count

    def config_hash(self, path: str) -> str:
        self.calls.append("config_hash")
        if self._config_raises or not path.endswith(".toml"):
            raise EngineError(f"config load: no such file ({path})")
        return REFERENCE_CONFIG_HASH

    def optimizer_names(self) -> list[str]:
        self.calls.append("optimizer_names")
        return [
            "equal_weight",
            "inverse_volatility",
            "kelly",
            "max_sharpe",
            "mean_variance",
            "minimum_variance",
            "risk_parity",
            "signal_weighted",
            "target_volatility",
        ]

    def optimize(self, **kwargs: Any) -> dict[str, Any]:  # pragma: no cover - unused in F2
        raise EngineError("optimization is not part of the read-only surface")

    def estimate_covariance(self, **kwargs: Any) -> dict[str, Any]:  # pragma: no cover
        raise EngineError("not part of the read-only surface")

    def rolling_metrics(self, **kwargs: Any) -> dict[str, Any]:  # pragma: no cover
        raise EngineError("not part of the read-only surface")

    def factor_contribution(self, portfolio, benchmark) -> dict[str, Any]:  # pragma: no cover
        raise EngineError("not part of the read-only surface")

    def validate_risk_limits(self, **kwargs: Any) -> dict[str, Any]:  # pragma: no cover
        raise EngineError("not part of the read-only surface")

    def read_artifact(self, key: str) -> dict[str, Any] | None:
        self.calls.append(f"read_artifact:{key}")
        if self._artifact_raises:
            raise EngineError(f"artifact {key} could not be read: corrupt JSON")
        return self._artifacts.get(key)

    def list_artifacts(self, prefix: str) -> list[str]:
        self.calls.append(f"list_artifacts:{prefix}")
        return sorted(k for k in self._artifacts if k.startswith(prefix))


SESSION_STATE = {
    "session_id": "demo",
    "sequence": 3,
    "phase": "running",
    "events_processed": 120,
    "account": {
        "cash": 50000.0,
        "equity": 101250.5,
        "gross_exposure": 51250.5,
        "status": "active",
    },
    "positions": [
        {"instrument": 0, "quantity": 100.0, "average_cost": 499.5},
        {"instrument": 1, "quantity": -25.0, "average_cost": 120.25},
    ],
}
