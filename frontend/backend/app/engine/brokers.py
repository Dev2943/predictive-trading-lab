"""Broker abstraction for future live trading.

INTERFACES ONLY. Nothing here connects to a venue, and nothing simulates one.
These exist so that adding a live broker later is an implementation of known
shapes rather than a redesign — and so the gateway can state truthfully today
that no live adapter exists.

WHY NOT WIRE IT UP NOW.

The engine already has a live module: connection supervision, heartbeat,
reconnect backoff, an Alpaca order translator, venue reconciliation. What it has
never had is a verified market data entitlement (ADR-0001) or a single
authenticated request to a broker. Building a live path on top of that would
produce something that looks connected and is not, which is the one outcome
worse than having no live path at all.

So `LiveBrokerAdapter` raises. A NotImplementedError at the boundary is honest;
a silent fallback to paper is not.
"""

from __future__ import annotations

from typing import Any, Protocol, runtime_checkable


class BrokerUnavailable(RuntimeError):
    """No live broker is connected.

    Distinct from a rejected order: unavailable means the venue was never
    reached, and an interface must not present the two the same way.
    """


@runtime_checkable
class ExecutionProvider(Protocol):
    """Submits and cancels orders at a venue.

    Mirrors what `PaperSessionHost` already does for paper, so a live
    implementation slots in without the routing layer changing shape.
    """

    def submit(self, order: dict[str, Any]) -> int: ...
    def cancel(self, order_id: int) -> bool: ...
    def cancel_all(self) -> int: ...


@runtime_checkable
class AccountProvider(Protocol):
    """Reports the venue's own view of the account.

    Deliberately separate from the portfolio the engine computes. Keeping them
    apart is what makes reconciliation possible; merging them would make a drift
    undetectable by construction — the same reasoning the engine's own live
    module applies.
    """

    def account(self) -> dict[str, Any]: ...
    def positions(self) -> list[dict[str, Any]]: ...


@runtime_checkable
class BrokerAdapter(ExecutionProvider, AccountProvider, Protocol):
    """One venue: execution and account together."""

    @property
    def name(self) -> str: ...

    @property
    def connected(self) -> bool: ...


class PaperBrokerAdapter:
    """The paper venue, served by the session host.

    A thin descriptor rather than a second implementation: orders already reach
    the host through the session driver, and routing them through an adapter as
    well would create a second submission path — exactly what F6 exists to
    avoid.
    """

    name = "paper"

    def __init__(self, driver: Any) -> None:
        self._driver = driver

    @property
    def connected(self) -> bool:
        return True

    def submit(self, order: dict[str, Any]) -> int:
        return int(self._driver.submit_order(**order))

    def cancel(self, order_id: int) -> bool:
        self._driver.cancel_order(order_id)
        return True

    def cancel_all(self) -> int:
        return int(self._driver.cancel_all())

    def account(self) -> dict[str, Any]:
        return (self._driver.snapshot().get("portfolio") or {}).get("account") or {}

    def positions(self) -> list[dict[str, Any]]:
        return (self._driver.snapshot().get("positions") or {}).get("positions", [])


class LiveBrokerAdapter:
    """Placeholder for a real venue.

    Every method raises. There is no fallback to paper: an interface that
    quietly executed against paper when it believed it was live would be the
    most dangerous bug this system could have.
    """

    name = "live"

    @property
    def connected(self) -> bool:
        return False

    def _unavailable(self) -> BrokerUnavailable:
        return BrokerUnavailable(
            "no live broker is connected; a live adapter requires a broker "
            "integration and a verified market data entitlement (ADR-0001)"
        )

    def submit(self, order: dict[str, Any]) -> int:
        raise self._unavailable()

    def cancel(self, order_id: int) -> bool:
        raise self._unavailable()

    def cancel_all(self) -> int:
        raise self._unavailable()

    def account(self) -> dict[str, Any]:
        raise self._unavailable()

    def positions(self) -> list[dict[str, Any]]:
        raise self._unavailable()


class OrderRouter:
    """Chooses the venue for an order.

    Trivial today because there is one venue. It exists so that the choice has a
    home: when a live adapter arrives, routing changes here and no route,
    model or component does.
    """

    def __init__(self, adapters: dict[str, Any] | None = None) -> None:
        self._adapters = adapters or {}

    def register(self, adapter: Any) -> None:
        self._adapters[adapter.name] = adapter

    def route(self, venue: str = "paper") -> Any:
        adapter = self._adapters.get(venue)
        if adapter is None:
            raise BrokerUnavailable(f"no adapter registered for venue '{venue}'")
        if not adapter.connected:
            raise BrokerUnavailable(f"the '{venue}' venue is not connected")
        return adapter

    def available(self) -> list[str]:
        return sorted(name for name, a in self._adapters.items() if a.connected)
