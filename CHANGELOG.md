# Changelog

## v1.0.0

First public release. Seventeen phases, 766 tests, 243 C++ files.

Everything below was built in order, and each phase was verified across GCC,
Clang, sanitizers, C++20 and C++23 before the next began.

### Phases 0–5 — Foundations and research

- **Core** — named types (`Price`, `Qty`, `Notional`, `Bps`), `Result<T>`,
  `DeterministicRng`, UTC nanosecond time, the seven-stage timestamp chain
- **Market data** — calendars, bars, quotes, trades, corporate actions,
  validation; Alpaca and Databento providers
- **Engine** — one deterministic event loop, strategy interface, order sink
- **OMS, risk, portfolio, accounting** — order lifecycle, pre-trade gates,
  positions, a journal that reconciles
- **Execution simulator** — conservative fill model (ADR-0003), latency and
  cost models
- **Features** — rolling, bivariate, intraday, cross-sectional, all
  point-in-time correct
- **Walk-forward validation** — folds, purging, embargo, a locked holdout

### Phase 6 — Models

OLS and ridge via LDLT, logistic via IRLS. Eigen confined to two translation
units behind a private link.

### Phase 7 — Signals, sizing, construction

Signal generation, position sizing, rebalancing. Found: probability exactly at
0.5 produced Short instead of Flat.

### Phase 8 — Quote-aware execution

Databento integration, quote book, quote-driven fills. Found: zero interval
volume silently blocked all fills; stop orders filled on arrival.

### Phase 9 — Execution algorithms

Immediate, TWAP, VWAP, POV, Iceberg, AdaptiveLimit. Schedules are pure
functions of simulated time (ADR-0004). Found: Immediate sent half the order,
inheriting TWAP's interpolation.

### Phase 10 — Analytics and reporting

Drawdown, exposure, risk statistics, attribution, performance reports in CSV,
JSON and Markdown.

### Phase 11 — Portfolio optimization

Nine optimizers behind one interface; covariance estimators with PSD repair.
Found: rolling VaR used a floor rank and **understated risk**.

*This phase was built twice. The first attempt implemented paper trading from a
misread roadmap and was discarded rather than patched.*

### Phase 12 — Attribution and institutional risk analytics

Economic P&L decomposition, per-trade execution quality, rolling risk series.
Found: MFE/MAE signs inverted for held positions.

### Phase 13 — Strategy lifecycle and research infrastructure

Strategy catalogue, dataset registry, model registry with champion/challenger,
experiment comparison and leaderboards. Found: `champion()` was a linear scan —
6854 ns → 55 ns.

### Phase 14 — Paper trading

Paper broker and session with persistence and recovery. `Engine` gained
`begin`/`step`/`finish`, with `run()` composing them. Found: persistence was
lossy behind a bit-exact checksum, in two separate modules.

*The working tree was lost to an environment reset during this phase and rebuilt
from the Phase 13 archive.*

### Phase 15 — Live broker integration

Connection state machine, heartbeat, reconnect supervision, Alpaca order
translation, live session with venue reconciliation. `BrokerSimulator` gained a
validated ingress so it remains the sole `Fill` constructor. Found: an
uninitialized pointer that only Clang caught.

### Phase 16 — Production operations

Metrics registry, health monitoring, circuit breakers, watchdogs, alerts,
semantic config validation, runtime diagnostics. Found: a benchmark the compiler
had optimised away, reporting 0.000 ns.

### Phase 17 — Release

Documentation, compiled examples, install rules, generated version header,
extended CI. Found: two example programs written against APIs that do not exist.

---

## Architectural decisions

| ADR | Decision |
|---|---|
| 0001 | Market data source and entitlement tiers — **still unverified** |
| 0002 | `std::optional<Price>` rather than NaN for absent limit prices |
| 0003 | Conservative fill model, not queue position |
| 0004 | Deterministic execution scheduling |
| 0005 | *(withdrawn — belonged to the discarded Phase 11)* |
