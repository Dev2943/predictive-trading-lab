# Frontend F7 — Remaining Features & Integrations

## Audit: what was built and unreachable

Mapping every API path to its UI consumer found three gaps, all of them
capabilities that already existed and had no way in:

| Endpoint | Built in | UI consumer |
|---|---|---|
| `/analytics/rolling`, `/analytics/attribution/factors` | F5 | **none** |
| `/optimization/covariance` | F5 | **none** |
| `/artifacts`, `/diagnostics`, `/metrics`, `/reports` | F2 | **none** |

Plus the control gap I flagged at the end of F6.

## Scope, and why each item is F7 rather than F8

F8 is polish and verification. Everything here adds a capability or closes a
workflow, which is feature work.

**Halt Strategy.** F6 left a hole between two controls at different levels:
`flatten` acts on the book, `stop` tears the session down. Nothing acted on the
*strategy*. An operator wanting the model to pause had to destroy the session and
lose the book — the wrong remedy for "hold on a moment". Closing that needs a
host flag, a binding, an endpoint and a control: feature work, not polish.

**Analytics page.** The endpoints existed with tests and no data to run on. The
session's equity history supplies it, which also makes this the first genuine
cross-page integration: one page consuming another's state.

**Covariance wiring.** The optimization page warned that `minimum_variance`
would be refused and then offered no way to satisfy it — a dead end by
construction. It can now estimate a covariance and run.

**Shared session bar.** Session state lived only on the dashboard, so an
operator on Trading or Analytics could not tell whether the session was running,
halted or gone.

## Architecture decisions

**Halt is a flag, not a lifecycle state.** The session stays RUNNING, events
keep flowing, the book keeps marking and manual orders still work. Making it a
lifecycle transition would have produced a second Stop with no defined resume.

**The bar counter keeps advancing while halted.** Resuming rejoins the schedule
where the market is now rather than handing the strategy a burst of signals it
"missed". A halt must not change what the strategy does afterwards.

**The client derives returns from the equity series, and nothing else.** That is
a change of representation — levels to periods — because the analytics endpoints
take returns and the session publishes levels. Every statistic is the engine's.

## Interesting findings

**My halt test measured the wrong baseline.** It captured the order count
*before* calling `set_halted`, so orders the strategy submitted legitimately in
the gap read as halt violations. Fixed by taking the baseline from the first
snapshot in which `strategy_halted` is true. The test passed in isolation and
failed about half the time in the suite, which is exactly how a race presents.

**The C++ host is a process singleton, so integration tests are not isolated.**
Every `SessionDriver` in a process drives the same session. A driver thread
outliving its test can step or replace a later test's session. An autouse
fixture now forces the host to a known state around each test, and the halt test
asserts it is reading its own `session_id` before comparing counters — so
interference names itself instead of surfacing as a confusing mismatch.

**Benchmarking attribution against a flat series is an alpha reading, not a
beta.** The session has no benchmark instrument, and the page says so rather
than presenting a market beta it cannot know.

## Not in F7

No live broker connection, authentication, WebSockets, or persisted-artifact
browser. The artifact endpoints remain without a UI; that is a known remaining
gap, deliberately left rather than half-built.
