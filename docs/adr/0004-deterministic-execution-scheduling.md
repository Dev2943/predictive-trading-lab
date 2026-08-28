# ADR-0004 — Execution schedules are pure functions of simulated time

**Status:** Accepted
**Date:** 2026-08-28
**Context:** Phase 9 introduces execution algorithms that emit child orders over time.
**Related:** ADR-0003 (conservative fill model, no queue position)

---

## The problem

An execution algorithm decides *when* to send each slice of a parent order.
"When" is the dangerous word. The obvious implementations all break replay:

- reading a wall clock makes two replays of the same data differ;
- a timer thread makes slice order depend on scheduler jitter;
- state carried across executions makes the *n*-th execution depend on the
  *n−1*-th in ways no manifest records;
- iterating a hash map of live executions emits children in an order that
  varies per run, and floating-point summation is not associative.

Any of these would silently break the property every prior phase has preserved:
two identical runs produce identical orders, fills, and P&L.

## Decision

**A schedule is a pure function of (parent order, execution window, policy), and
a slice release is a pure function of (schedule, progress, market state, `now`).**

Concretely:

1. **No algorithm reads a clock.** `now` arrives inside `ExecutionContext`,
   supplied by the engine from the same `IClock` the rest of the system uses.
   There is no clock member on any algorithm.

2. **No timers, no threads.** Slices are released when the engine delivers a
   market event whose timestamp has passed the slice boundary. A replay
   therefore releases at exactly the same simulated instants as a live session
   would, because both are driven by event arrival.

3. **The schedule is computed once, up front.** `plan()` runs before any child
   is sent, so the plan can be inspected rather than reconstructed from fills.

4. **Algorithms are cloned per execution.** `IExecutionAlgorithm::clone()`
   gives each parent a fresh instance, so execution *n* cannot inherit state
   from *n−1*.

5. **Every container iterated during release is ordered.** `Executor` holds
   `std::map` keyed by parent order id, so children are emitted in id order on
   every run.

6. **Algorithms chase the *cumulative* target**, not the per-slice one. A slice
   skipped for participation or a halt is made up later rather than lost, which
   makes the outcome depend on the data rather than on which slices happened to
   be releasable.

## Consequences

**Positive.** Replay and live share one scheduling path. The
`Executor::content_hash()` over emitted children is comparable across runs, and
the Phase 9 determinism tests assert bit-identical child sequences.

**Negative.** Slice release granularity is bounded by event arrival: with
one-minute bars, an algorithm cannot release more finely than once a minute
regardless of its schedule. That is a real limitation and it is the honest one —
a simulator that released between events would be claiming a reaction time the
data cannot support.

**Also negative.** Because algorithms hold no clock, they cannot implement
genuinely time-triggered behaviour such as "cancel if unfilled for 30 seconds"
except as a check performed on the next event. In a thin market the next event
may be far away. `Executor::expire_stale()` exists for exactly this and must be
called by the driver on every event, not only when an execution is active.

## What this ADR does not permit

It does **not** relax ADR-0003. Nothing here estimates queue position. The
iceberg refreshes on completion of its displayed clip and makes no claim about
where the refreshed clip lands in a book we cannot observe.

## Verification

- `tests/leakage/test_execution_algos.cpp` — *"two identical runs emit identical
  children"*, *"algorithms hold no clock"*, *"a slice cannot be released before
  its window opens"*.
- `Executor::content_hash()` compared across runs in the end-to-end test.
