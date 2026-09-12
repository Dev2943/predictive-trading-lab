# Frontend

The web interface and API gateway for the Predictive Trading Lab engine.
Everything lives under this directory; the engine at the repository root is
byte-identical to its v1.0 release.

```
frontend/
    backend/        FastAPI gateway + pybind11 bindings + session host
    web/            Next.js interface
    docs/           Architecture notes, one per phase, and the OpenAPI schema
```

## The chain

```
Next.js → FastAPI → SessionDriver → pybind11 → PaperSessionHost (C++) → PaperSession → Engine
```

and, without changing anything above the gateway:

```
Next.js → FastAPI → HTTP → remote engine
```

That works because the binding boundary is **JSON, not objects**. Both
transports return the same parsed data, because both read the same serializers
inside the engine.

## Invariants

These held through every phase and are enforced by tests:

| Invariant | How |
|---|---|
| The engine owns every trading object | `Engine`, `PaperSession`, `PaperBroker` and `PaperAccount` are never bound; a test asserts their absence |
| One writer | Every mutation is a command the `SessionDriver` applies between steps; readers consume published snapshots and never touch the session |
| No lock on the trading path | Reads come from an immutable snapshot dict, so opening the interface cannot slow trading |
| React performs no business logic | The client plots and sends intent; every statistic comes from the engine |
| Replay determinism | Fingerprints unchanged since v1.0: `config 30b44e5972450aad` |

## Build

The engine builds with no Python, Node or frontend dependency:

```bash
cmake --preset linux-gcc-release && cmake --build build/linux-gcc-release
```

The bindings are a standalone CMake project — nothing at the repository root
changed to accommodate them:

```bash
pip install -r frontend/backend/requirements.txt

cmake -S frontend/backend/bindings -B build/bindings -G Ninja \
      -DCMAKE_BUILD_TYPE=Release \
      -Dpybind11_DIR=$(python3 -c "import pybind11; print(pybind11.get_cmake_dir())")
cmake --build build/bindings

PYTHONPATH=build/bindings/lib python3 -m pytest frontend/backend
```

Gateway and web:

```bash
cd frontend/backend && PYTHONPATH=../../build/bindings/lib uvicorn app.main:app --reload
cd frontend/web && npm install && npm run dev
```

## Pages

| Route | Purpose |
|---|---|
| `/` | Session controls, account, equity and drawdown charts, positions, orders, fills |
| `/trading` | Order entry, working orders, trade blotter, halt / cancel-all / flatten |
| `/analytics` | Rolling volatility, Sharpe, VaR, CVaR and attribution over the session's history |
| `/optimization` | The nine optimizers, with covariance estimation |
| `/risk` | Semantic validation of a risk limit set |

## Trading controls, and what each one does

Three controls at three levels. Conflating them is the mistake this design is
built to prevent:

- **Flatten** acts on the *book* — cancel working orders, then close every open
  position. It does not stop the strategy, which may re-open on its next signal.
- **Halt strategy** acts on the *strategy* — suppress its order generation while
  the session keeps running, data keeps flowing, the book keeps marking and
  manual orders still work.
- **Stop session** acts on the *session* — close out and tear it down.

The emergency sequence is flatten, then halt, then stop.

## Known limitations

- **Market data is a deterministic synthetic replay**, labelled `synthetic-replay`
  everywhere it surfaces. ADR-0001's entitlement has never been verified and no
  live feed exists.
- **No live broker.** `LiveBrokerAdapter` raises on every method and never falls
  back to paper.
- **Manual order timing is not replayable.** Ordering *within* a bar is
  deterministic — manual requests FIFO, then strategy orders — but which bar a
  human's order lands on depends on wall-clock arrival. Full replay of a manual
  session would require journalling commands against event indices.
- **Some API endpoints have no UI.** Persisted artifacts, diagnostics, metrics
  and reports are available to programmatic clients and documented in the
  OpenAPI schema; no screen consumes them.
