# Frontend F1 — Infrastructure

## What F1 delivers

The chain React → FastAPI → pybind11 → C++ engine, connected and tested, with
no trading screens. Everything lives under `frontend/`; the engine is untouched.

## Design decisions

### The boundary is JSON, not objects

Every non-trivial result crosses as a JSON string produced by the engine's own
`to_json`. This looks like a wasted serialization and is the most important
decision in the layer:

- The gateway never holds a pointer into engine memory, so it cannot mutate
  trading state or outlive an object it borrowed.
- The wire format is identical whether the engine is in-process or remote.
  Relocating it is a transport swap, not an API rewrite.
- The engine already emits ordered, deterministic JSON. Re-deriving that shape
  in pybind11 would be a second serializer that could disagree with the first.

### The bindings are a standalone CMake project

`frontend/backend/bindings/CMakeLists.txt` is configured on its own and pulls
the engine in with `add_subdirectory(... EXCLUDE_FROM_ALL)`. The consequence is
that **no file at the repository root changed** — no option, no conditional, no
`add_subdirectory`. A developer building the engine needs no Python toolchain
and need not know this directory exists.

The engine's tests, apps, examples and benchmarks are switched off for that
sub-build: a consumer builds the libraries it links, not its dependency's test
suite.

### Sessions stay internal

`Engine`, `PaperSession` and `LiveSession` are deliberately unbound, and a test
asserts their absence. Mutation arrives in a later phase through a single-writer
command queue: writes enqueue commands that a session drains *between events*,
which is exactly what `Engine::begin/step/finish` was built for. Reads are
served from a snapshot, so no lock ever sits on the trading path.

That preserves determinism exactly. A command applied between events is
indistinguishable from one a strategy issued at that instant.

## Findings

**The engine's static libraries are not position-independent.** A Python
extension is a shared module, so linking them failed. Fixed with
`CMAKE_POSITION_INDEPENDENT_CODE` in the *bindings* project — a build setting on
a consumer's copy, not a change to the engine, whose own build is unaffected.

**A standalone project does not inherit the engine's language standard.**
Without setting C++23 explicitly, `requires` and `operator<=>` in the public
headers failed to parse, and the error pointed confusingly into the engine
rather than at the consumer.

**An earlier draft of the covariance binding silently disabled an engine
guard.** It hardcoded `min_observations_ratio = 0.0`, switching off the
engine's protection against estimating a 50×50 covariance from 20 rows. A
binding that turns that off has weakened a guarantee its caller still believes
it has. It is now a parameter defaulting to the engine's own value, with a test
proving relaxing it must be explicit.

## Not in F1

No dashboard, charts, trading screens, order management, or optimization UI.
The web project is structure only.
