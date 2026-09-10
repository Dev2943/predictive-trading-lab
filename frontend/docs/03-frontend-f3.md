# Frontend F3 — Session Host

## Architecture

```
Next.js → FastAPI → SessionDriver → pybind11 → PaperSessionHost (C++) → PaperSession
```

### Why the host is C++

A `PaperSession` needs eleven collaborators — clock, source, strategy,
simulator, broker, portfolio, OMS, risk, journal, artifacts, calendar — none
thread-safe, all of which must outlive it. Binding them to Python would hand a
request handler pointers to live trading objects, which is exactly what the F1
boundary was drawn to prevent.

So `PaperSessionHost` owns them in C++ and exposes **five free functions**
returning JSON. `Engine`, `PaperSession`, `PaperBroker`, `PaperAccount` and the
host type itself are never bound. A test asserts their absence.

### Single writer, no mutex

```
request handlers                    driver thread (exactly one)
      |                                     |
      |-- enqueue command --> Queue -->  drains between steps
      |                                     |
      |<-- read ------------ snapshot <-- published after each step
```

- **Writes are commands.** A handler never touches the session.
- **Reads come from an immutable snapshot dict.** Readers never touch the
  session either, so no lock sits on the trading path.
- **One thread steps the engine for its whole life.** A command applied between
  steps is indistinguishable from one issued at that instant, so determinism is
  preserved exactly — and an integration test proves two runs of one seed
  produce identical counters and equity.

A mutex around the engine was rejected: it would serialise readers against the
trading loop, so opening the dashboard would slow trading. That is an
observability layer changing trading behaviour, which the engine's Phase 16 was
built to avoid.

## Lifecycle

```
STOPPED ──start──▶ STARTING ──▶ RUNNING ──stop──▶ STOPPING ──▶ STOPPED
                       │                              │
                       └──────────▶ ERROR ◀───────────┘
                                      │
                                      └── start ──▶ STARTING
```

`RUNNING → STARTING` is illegal and returns **409**: starting a second session
would abandon the first one's book without closing it out. 409 rather than 400
because the request was well-formed and refused for *when* it arrived — a client
should retry, not fix its payload.

`ERROR → STARTING` is legal, so a transient failure does not require a process
restart.

**Reset is a stop followed by a start**, never an in-place reset. Discarding a
book where it stands would skip the close-out that reconciles the journal.

## Market data

A **deterministic synthetic replay**, seeded from the session config. ADR-0001's
entitlement is unverified and no live feed exists. It is labelled
`synthetic-replay` in the API, in the state document and on the dashboard. A
paper session fed by invented data presented as live would be worse than no
session at all.

## Findings

**The pimpl gotcha.** `PaperSessionHost() = default` in the header failed to
compile: a defaulted constructor must be able to destroy members if construction
throws, which needs the complete `Impl`. The error points at the constructor
rather than the member that caused it. Both constructor and destructor are now
defined in the `.cpp`.

**Two guards fired on an intentional change**, which is what they are for. F3
adds the first POST endpoints and five new bindings, so the "everything is GET"
and "only this surface is exposed" tests both failed. Both were made *stronger*
rather than relaxed: the API test now asserts mutation is confined to exactly
`/session/{start,stop,reset}` and nothing else, and the binding test asserts the
new names are functions rather than types.

**The driver steps the session on its own thread.** A session that only advanced
when someone polled it would stall whenever the dashboard was closed — the book
would silently stop marking.

**An exhausted replay is not an error.** The driver keeps the session RUNNING
and the book readable, as a live session between market events.

## Not in F3

No live trading, authentication, WebSockets, or charts.
