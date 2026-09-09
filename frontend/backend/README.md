# Backend

FastAPI gateway and pybind11 bindings.

```
backend/
    bindings/       pybind11 module (standalone CMake project)
    app/            FastAPI application
        engine/     the EngineClient abstraction
        models/     pydantic wire models
        routers/    (F2+)
    tests/          gateway tests
```

## The abstraction that matters

`app/engine/client.py` defines `EngineClient`, a protocol. Routes depend on it
and never import `ptl`. Today `InProcessEngineClient` calls the bindings;
tomorrow a `RemoteEngineClient` speaks HTTP to an engine elsewhere. Swapping
them is a change to `app/dependencies.py` alone — no route, model or component
is affected.

## What the bindings expose, and what they do not

**Exposed:** determinism fingerprints, the nine optimizers, covariance
estimation with diagnostics, rolling analytics, factor attribution, risk-limit
validation. All stateless, so a request handler can call them safely.

**Not exposed:** `Engine`, `PaperSession`, `LiveSession`. Handing a request
handler a pointer to a live trading object is the failure this design exists to
prevent. Sessions arrive behind a single-writer command queue in a later phase.
`test_sessions_are_not_reachable_from_python` is the guard.

## Errors

The bindings raise `RuntimeError` carrying the engine's own message; the client
wraps it as `EngineError`; routes turn it into a 4xx with that message intact.
An optimizer that refuses says *why*, and that reason reaches the user rather
than a generic failure.

`EngineUnavailable` is separate and becomes a 503: the gateway is fine and the
engine is not reachable, which a client should treat differently from a bad
request.

## Tests

```bash
PYTHONPATH=build/bindings/lib python3 -m pytest frontend/backend
```

The load-bearing one is `test_rng_fingerprint_matches_the_engine`. If the
binding layer perturbs determinism, nothing above it can be trusted.
