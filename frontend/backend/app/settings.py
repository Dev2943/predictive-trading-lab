"""Deployment configuration.

EVERY DEPLOYMENT-SPECIFIC VALUE LIVES HERE, read from the environment once at
import and validated at startup. Nothing else in the application reads
`os.environ` for deployment concerns, so what a deployment can change is a
single readable list rather than a search.

FAIL FAST, WITH A REMEDY. A misconfigured production process that starts and
then serves errors is worse than one that refuses to start: the first looks
healthy to a load balancer. Validation therefore runs before the first request
and names the variable and the fix.
"""

from __future__ import annotations

import os
from dataclasses import dataclass, field
from typing import Literal


def _bool(name: str, default: bool) -> bool:
    raw = os.environ.get(name)
    if raw is None:
        return default
    return raw.strip().lower() in ("1", "true", "yes", "on")


def _csv(name: str, default: list[str]) -> list[str]:
    raw = os.environ.get(name)
    if not raw:
        return list(default)
    return [item.strip() for item in raw.split(",") if item.strip()]


@dataclass(frozen=True)
class Settings:
    """Resolved deployment settings."""

    environment: Literal["development", "production"] = field(
        default_factory=lambda: (
            "production" if os.environ.get("PTL_ENV") == "production" else "development"
        )
    )
    host: str = field(default_factory=lambda: os.environ.get("PTL_HOST", "0.0.0.0"))
    # Render and most PaaS inject PORT. Honouring it is what makes the image
    # deployable without a platform-specific entrypoint.
    port: int = field(
        default_factory=lambda: int(os.environ.get("PORT", os.environ.get("PTL_PORT", "8000")))
    )
    log_level: str = field(
        default_factory=lambda: os.environ.get("PTL_LOG_LEVEL", "info").lower()
    )
    debug: bool = field(default_factory=lambda: _bool("PTL_DEBUG", False))

    allowed_origins: list[str] = field(
        default_factory=lambda: _csv(
            "PTL_ALLOWED_ORIGINS",
            ["http://localhost:3000", "http://127.0.0.1:3000"],
        )
    )
    results_dir: str = field(
        default_factory=lambda: os.environ.get("PTL_RESULTS", "results")
    )
    config_path: str = field(
        default_factory=lambda: os.environ.get("PTL_CONFIG", "config/base.toml")
    )

    market_provider: str = field(
        default_factory=lambda: os.environ.get("PTL_MARKET_PROVIDER", "replay")
    )

    @property
    def alpaca_configured(self) -> bool:
        # Presence only. Values are never read into a field, logged or echoed --
        # a masked credential in a log is still a credential in a log.
        return bool(
            os.environ.get("PTL_ALPACA_KEY") and os.environ.get("PTL_ALPACA_SECRET")
        )

    @property
    def is_production(self) -> bool:
        return self.environment == "production"

    def validate(self) -> list[str]:
        """Return every problem, not just the first.

        An operator fixing a deployment should get one list, not a sequence of
        failed restarts -- the same rule the engine's own ConfigValidator
        applies.
        """
        problems: list[str] = []

        if not 1 <= self.port <= 65535:
            problems.append(f"PORT must be 1-65535, got {self.port}")

        if self.log_level not in ("critical", "error", "warning", "info", "debug", "trace"):
            problems.append(
                f"PTL_LOG_LEVEL is '{self.log_level}'; expected one of "
                "critical, error, warning, info, debug, trace"
            )

        if self.is_production:
            if self.debug:
                # Debug in production leaks stack traces to clients.
                problems.append(
                    "PTL_DEBUG must not be set in production; unset it or set PTL_ENV=development"
                )
            if any("localhost" in o or "127.0.0.1" in o for o in self.allowed_origins):
                problems.append(
                    "PTL_ALLOWED_ORIGINS still contains localhost in production; "
                    "set it to your deployed frontend origin"
                )
            if "*" in self.allowed_origins:
                # A wildcard on an API that places orders is not a default worth
                # having, even a paper one.
                problems.append(
                    "PTL_ALLOWED_ORIGINS must not be '*' in production; list explicit origins"
                )

        if self.market_provider not in ("replay", "alpaca"):
            problems.append(
                f"PTL_MARKET_PROVIDER is '{self.market_provider}'; expected replay or alpaca"
            )
        if self.market_provider == "alpaca" and not self.alpaca_configured:
            problems.append(
                "PTL_MARKET_PROVIDER=alpaca requires PTL_ALPACA_KEY and PTL_ALPACA_SECRET"
            )

        return problems

    def describe(self) -> dict[str, object]:
        """Startup banner contents. Deliberately contains no secret."""
        return {
            "environment": self.environment,
            "host": self.host,
            "port": self.port,
            "log_level": self.log_level,
            "debug": self.debug,
            "allowed_origins": self.allowed_origins,
            "results_dir": self.results_dir,
            "market_provider": self.market_provider,
            "alpaca_credentials": "present" if self.alpaca_configured else "absent",
        }


class ConfigurationError(RuntimeError):
    """Startup configuration is unusable."""


def load_settings() -> Settings:
    settings = Settings()
    problems = settings.validate()
    if problems:
        raise ConfigurationError(
            "invalid deployment configuration:\n  - " + "\n  - ".join(problems)
        )
    return settings
