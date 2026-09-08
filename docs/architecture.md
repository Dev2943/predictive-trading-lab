# Architecture

## The one rule

Dependencies point downward. A module may use anything in a layer below it and
nothing in a layer above. This is not a style preference: it is what makes the
guarantees in the README checkable, and CI greps for the violations that matter
most.

```
┌─────────────────────────────────────────────────────────────┐
│ OBSERVATION   analytics  attribution  report  reporting  ops │  read-only
├─────────────────────────────────────────────────────────────┤
│ SESSIONS      engine  paper  live  strategy  storage         │
│               experiment                                     │
├─────────────────────────────────────────────────────────────┤
│ TRADING       signal  sizing  optimization  construction     │
│               oms  risk  execution  algo  portfolio          │
│               accounting  pipeline                           │
├─────────────────────────────────────────────────────────────┤
│ RESEARCH      market  databento  features  labels            │
│               validation  research  models  experiments      │
├─────────────────────────────────────────────────────────────┤
│ FOUNDATION    core  log  config                              │
└─────────────────────────────────────────────────────────────┘
```

Three consequences worth stating explicitly:

- **`ops` observes but is never observed.** No trading module includes
  `ptl/ops`. Instrumentation therefore cannot change trading behaviour, and the
  property is verifiable by grep rather than by argument.
- **`strategy` cannot reach the venue.** The catalogue links `core` only. A
  registry that could reach the engine would be a registry that could trade.
- **`labels` is not linked into live binaries.** Forward-looking labels exist
  for research and are physically absent from anything that trades.

---

## Data flow

### Research (offline)

```
raw data → validate → normalise → features → labels → walk-forward split
        → train → out-of-sample predictions → evaluation
```

The split is the load-bearing part. `validation` produces folds; every
estimator downstream is handed one fold's worth of history and cannot see past
its boundary. The holdout is locked and unlocking it is recorded in the
experiment registry, because a holdout you can peek at is not a holdout.

### Trading (online)

```
event → Engine.step()
      → strategy.on_bar/on_quote
        → features → model → signal → sizing
        → optimizer → targets → rebalance → orders
      → risk gate
      → OMS
      → execution algorithm → child orders
      → broker (simulator | paper | live)
      → fills → portfolio → journal
      → analytics
```

Every arrow is a function call on one thread. There is no queue, no callback
from a foreign thread, and no async continuation. That is what makes a replay
reproduce byte-for-byte.

---

## The seven-stage timestamp chain

Every order and fill carries seven instants, and each must not precede the one
before it:

```
data_time → receive_time → decision_time → submit_time
          → arrival_time → execution_time → fill_time
```

`arrival_time > decision_time` strictly. An order that arrives at the venue at
the instant it was decided has zero latency, which is not a thing that happens,
and a backtest built on it will overstate its edge. The chain is validated at
construction, and violations are counted and surfaced in the run summary rather
than silently accepted.

---

## Execution modes

Replay, paper and live differ in exactly three injected objects:

| | Replay | Paper | Live |
|---|---|---|---|
| Clock | `SimulatedClock` | `SimulatedClock` or wall | wall |
| Market data | `ReplaySource` | replay or paper feed | `LiveMarketDataAdapter` |
| Broker | `BrokerSimulator` | `PaperBroker` | `LiveBroker` |

Everything else is shared. There is no mode flag anywhere in the codebase, and
`grep -rn "if (live)"` returns nothing. See
[`execution-modes.md`](execution-modes.md).

---

## Broker abstraction

```
        strategy code
              │  submits ptl::oms::Order
              ▼
        risk gate → OMS → execution algorithm
              │
              ▼
   ┌──────────┴──────────┬─────────────────┐
   ▼                     ▼                 ▼
BrokerSimulator     PaperBroker        LiveBroker
(matches quotes)   (adds acks,        (adds venue
                    screening)         protocol)
                         │                 │
                         └────────┬────────┘
                                  ▼
                    BrokerSimulator constructs
                    every Fill in the system
```

`PaperBroker` and `LiveBroker` are adapters, not fill engines. Both route fills
through `BrokerSimulator`, which owns the sole `Fill` constructor. Live fills
arrive through `ingest_external_fill`, which validates the venue's report
against the working order before believing it — an unknown order, an over-fill,
or a timestamp-chain violation is refused.

Adding a venue means writing one `IOrderTranslator`. Nothing in Engine, OMS,
Risk, Execution or any strategy changes.

---

## Portfolio pipeline

```
predictions → SignalGenerator → signals
            → PositionSizer → notional targets
            → PortfolioOptimizer → weights
            → RebalanceEngine → orders
```

The optimizer returns **weights, never orders**. Turning weights into trades is
the rebalance engine's job and turning trades into child orders is the execution
layer's. Keeping the three apart is what stops position sizing happening twice —
which is exactly what would occur if an optimizer returned quantities and the
sizer then scaled them again.

Nine optimizers share one interface: equal weight, inverse volatility, risk
parity, minimum variance, mean variance, maximum Sharpe, Kelly, target
volatility, signal weighted. Selection is holding a different pointer.

---

## Execution pipeline

```
parent order → ExecutionAlgorithm → schedule → child orders → broker
```

Six algorithms: Immediate, TWAP, VWAP, POV, Iceberg, AdaptiveLimit. Schedules
are **pure functions of simulated time** (ADR-0004) — no algorithm holds a
clock, and each is cloned per execution. That is what lets an execution be
replayed exactly.

---

## Where the numbers come from

There is exactly one definition of each statistic:

- **Sharpe, Sortino, Calmar, drawdown** — `analytics`. The experiment layer
  *reads* these; it never recomputes them.
- **P&L by economic source** — `attribution`. Distinct from attribution by
  sector or strategy, which answers a different question.
- **Operational counters** — `ops`. Orders, rejects, latencies. Never mixed
  into a performance report.

A second implementation of any of these is how two reports of the same run come
to disagree, and the module boundaries exist to prevent it.
