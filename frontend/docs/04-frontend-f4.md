# Frontend F4 — Portfolio History & Visualisation

## Why F4 was redefined

The original roadmap put "Dashboard & Portfolio" in F4. F3 delivered most of it
— session controls, account summary, positions, orders, fills, counters,
polling, tests. Implementing the original F4 would have duplicated working code.

What F3 left behind was the *Portfolio* half in its literal sense: **there was no
time dimension anywhere in the system.** Every value was point-in-time. `recharts`
was a declared dependency imported by zero files. No chart was possible because
no series existed.

F4 is therefore: **give the system a time dimension, and draw it.**

## The finding that shaped the phase

`Portfolio` keeps an equity curve, and `Engine` appends to it — but only when
`EngineConfig::snapshot_on_bar` is set, and `PaperSession` constructs its Engine
without exposing that flag. Over one trading day the engine's curve holds
**exactly one point**.

My first implementation read that curve and produced a one-point chart. Three
options followed:

1. **Modify the engine** to expose the flag — rejected, the engine is frozen.
2. **Call `Portfolio::snapshot()` from the host** — rejected, and this is the
   important one. It would append to a series the engine believes it owns, at a
   cadence the engine did not choose. The engine's curve is its own.
3. **Sample independently** — chosen. The host *reads* equity, cash and
   exposures on the driver thread and appends to its own bounded buffer.

That distinction — reading state versus writing into the engine's series — is
what keeps the engine's ownership intact.

## Architecture

No change to the chain, and no new threads:

```
Next.js → FastAPI → SessionDriver → pybind11 → PaperSessionHost → PaperSession → Engine
```

**C++** — the host gained an `InstrumentTable`, a bounded sample buffer, and
`record_sample()`, called from `step()` on the driver thread when the book
actually moved. Drawdown comes from the engine's own `DrawdownTracker`, fed
those samples. `history_json` downsamples by **stride, not averaging**:
averaging smooths away the drawdown troughs a reader is looking for.

**Python** — no new threads, no new state. `/session/history` and
`/session/instruments` read the snapshot the driver already publishes.

**Why history rides inside the snapshot.** A reader fetching history on demand
would have to call into the session from a request thread, breaking the
single-writer guarantee. Publishing it with everything else also means the chart
and the account come from one instant rather than two calls that could land
either side of a step.

**React** — `EquityChart` plots; it does not calculate. The one locally derived
value is each point's distance from peak, computed from **the peak the engine
reported**, not from a peak inferred over the visible window — those disagree
whenever the series is downsampled.

## Determinism

Unchanged and verified: two runs of one seed produce byte-identical equity
series and account state. Sampling is a read on the single writer thread, so it
cannot reorder or perturb anything.

## Other findings

**Sampling only when the book moved.** Recording on every idle poll would fill
the series with duplicate points and make a stalled session look busy.

**The opening balance is a real observation.** Without seeding the series at
start, a chart's first point is the equity *after* the first trades and the
starting capital never appears.

**Instrument ids leak an internal index.** The host resolves them to symbols,
and the client falls back to `#id` rather than rendering a blank cell.

## Not in F4

Optimization UI, risk UI, live trading, order entry, multi-page navigation.
