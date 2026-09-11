# Frontend F5 — Computation Surface

## Audit finding

F1 bound five computation functions — `optimize`, `estimate_covariance`,
`rolling_metrics`, `factor_contribution`, `validate_risk_limits`. F2 was
GET-only, so none of them was ever exposed. `/optimization`, `/analytics` and
`/risk` shipped as *capability* endpoints that describe what the engine can do
without letting anyone do it.

**The engine's computational surface was built, bound, tested, and unreachable.**
That is F5.

The original roadmap called F5 "Optimization & Risk", which turns out to be
right for the wrong reason: the gap is not that those features are missing but
that they were stranded one layer below the API.

## Scope

**In F5:** five POST computation endpoints, an interactive optimizer page, a
risk-limit validation page, and navigation (deferred from F4 because there was
only one route to navigate).

**Not in F5:** live trading, order entry, efficient frontier (a sweep of
repeated solves — new computation rather than exposure of existing), PDF
reports, WebSockets.

## The design decision

**Computation bypasses the SessionDriver, deliberately.**

Two options existed:

1. **Route everything through the single-writer queue.** Uniform, and wrong.
   These functions construct their own inputs, touch no session, and leave no
   state behind. Queuing them would serialise stateless arithmetic behind the
   trading loop, so running an optimization would delay the book from marking.
2. **Call the engine directly through `EngineClient`**, as F2's read-only
   endpoints already do. Chosen.

The single-writer rule exists to protect *mutable session state*. None of these
has any. A test runs optimizations concurrently with a stepping session and
asserts the determinism fingerprints are unmoved.

**POST means "carries a body", not "mutates".** A covariance matrix does not fit
in a query string. The guard test was rewritten to classify every POST as either
`LIFECYCLE` or `COMPUTE` and assert the set is exactly those two — a new POST on
`/portfolio` fails the suite, so the classification is a decision someone must
make rather than drift into.

## Findings

**A real bug in the risk form: decimals were impossible to type.** The inputs
coerced with `Number()` on every keystroke, so "0.25" passed through "0.",
`Number("0.")` gave 0, and the field jumped to 25. Every limit except the
notionals is a fraction, so the form was unusable for its main purpose. Fixed by
holding the draft as strings and converting on submit; pinned by a regression
test.

**Requirements are surfaced before the refusal.** The optimizer page reads the
engine's own requirement table and warns that `minimum_variance` will be refused
without a covariance, rather than letting the user submit and read a 422.

**Binding constraints are shown.** A weight resting exactly on a limit is a
constrained answer, not a free optimum, and the engine reports which constraints
bound.

**Navigation arrived in F5, not F4.** Building a shell around a single route
would have been chrome around nothing.

## Architecture

Unchanged. No new threads, no engine changes, no new bindings — F5 exposes what
F1 already bound.

```
Next.js → FastAPI → EngineClient → pybind11 → engine      (stateless compute)
Next.js → FastAPI → SessionDriver → pybind11 → host       (session lifecycle)
```
