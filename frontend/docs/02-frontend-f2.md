# Frontend F2 — Read-only API

## What F2 delivers

Sixteen GET endpoints over the engine, a mock-based test suite that needs no
compiled extension, and a landing page that verifies the chain end to end.

## Architecture

```
React → FastAPI → EngineClient → pybind11 → C++ engine
```

No route imports `ptl`. Routes depend on the `EngineClient` protocol, so the
in-process client can be replaced by a remote one with a change to
`dependencies.py` alone.

## Two kinds of endpoint, and why

**Capability endpoints** (`/optimization`, `/analytics`, `/risk`) report what
the engine can do.

**Artifact endpoints** (`/portfolio`, `/positions`, `/diagnostics`, `/metrics`,
`/reports`) serve what a session previously wrote.

### The GET-only constraint

F2 is GET-only, and optimization and rolling analytics take matrices as input.
A covariance matrix does not fit in a query string, so those endpoints report
*capabilities* rather than running a computation. Encoding a matrix into a URL
would produce an endpoint nobody could call by hand and a length limit nobody
expects. Running them needs a request body and waits for a phase that has one.

### Absence is not zero

No session runs inside the gateway, so portfolio and positions come from
persisted artifacts. When nothing has been written the response says
`available: false`. An empty portfolio and a portfolio worth nothing render
identically on a chart, and only one of them is true.

Three distinct states are kept distinct:

| State | Response |
|---|---|
| Nothing persisted | `200`, `available: false` |
| Report requested by id, absent | `404` — the caller named a specific thing |
| Artifact exists, unreadable | `500` — a fault, not an absence |

## Findings

**`app.routes` does not flatten included routers in FastAPI 0.141.** They appear
as `_IncludedRouter` objects until the OpenAPI schema is built. A guard test
scanning `app.routes` for mutating verbs would never see the router endpoints
and would pass **vacuously**. The test now reads the schema, and asserts the
path count so it cannot pass on an empty surface.

**`/health` was untestable.** It called `get_engine()` directly to avoid 503-ing
on itself, which bypassed FastAPI's injection — so the unreachable-engine path,
the one most worth testing, could not be reached from a test. Split into a
`probe_engine` dependency that returns `None` instead of raising.

**The capability table claimed requirements the engine does not have.** It
listed `risk_parity` and `target_volatility` as requiring a covariance because
they are risk-based. They do not: without correlations, equal risk contribution
reduces to inverse volatility and the engine says so. Only `minimum_variance`
truly requires one. The table is now derived from measured behaviour, and an
integration test checks **both directions** — over-claiming is caught too,
because a table that over-claims makes a UI disable an optimizer that works.

## Not in F2

No charts, trading, orders, paper or live UI, optimization UI, auth, or
WebSockets.
