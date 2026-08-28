# ADR-0001 — Market Data Source and Entitlement

**Status:** Accepted
**Date:** 2026-08-21
**Supersedes:** nothing
**Decision source:** Decision 2 from `docs/01-research-reconciliation.md` §7

---

## Decision

Adopt a two-vendor, four-tier market-data plan. Do not purchase a recurring data subscription
before the paper-trading phase.

| Tier | Data | Vendor / entitlement | Used by | Expected cost |
|---|---|---|---|---|
| **T1** | 1-minute OHLCV bars; 9 liquid ETFs; ~3 years | Alpaca Basic, historical `feed=sip` only | Phases 2–7: ingest, accounting, bar execution, features, labels, validation, models, signals | $0 |
| **T2** | Consolidated BBO, one-minute sampling: `cbbo-1m`; same universe/range | Databento historical, usage-based | Phases 8–11: quote-aware simulation, spread features, liquidation marking, execution algorithms | Expected to fit within signup credit; **verify before download** |
| **T3** | Consolidated BBO, one-second sampling: `cbbo-1s`; 3 ETFs × ~60 sessions | Databento historical, usage-based | Phase 9: execution-resolution sensitivity experiment | Small incremental; **verify before download** |
| **T4** | Real-time consolidated SIP stream | Alpaca Algo Trader Plus, month-to-month | Phase 12: paper trading only | $99/month for 1–2 months, then cancel |

## Rationale

Alpaca Basic's IEX restriction applies to real-time / latest market data. Historical US-stock SIP
data is available through Alpaca Basic provided the query's end time is at least 15 minutes old.
This makes Alpaca Basic suitable for frozen historical 1-minute SIP OHLCV research. It is not
sufficient for real-time paper trading, which is the first phase requiring Algo Trader Plus.

The project's initial decision cadence is 5–15 minutes. Full tick quote data is unnecessary and
inefficient for that cadence. The quote-aware simulator requires valid top-of-book information at
each child-order arrival time, not every intervening quote update.

Databento's **consolidated** BBO schemas are the correct choice:

- Use `cbbo-1m`, not `bbo-1m`, for the one-minute quote tier.
- Use `cbbo-1s`, not `bbo-1s`, for the one-second sensitivity tier.
- `bbo-*` is not necessarily consolidated.
- `cbbo-*` represents consolidated top-of-book data across venues in the applicable dataset.

## Phase 2 entitlement gate

T1 depends on Alpaca Basic being entitled to historical SIP bars older than 15 minutes. Verify
before further ingest work.

```
GET https://data.alpaca.markets/v2/stocks/bars
  ?symbols=SPY
  &timeframe=1Min
  &feed=sip
  &start=2024-01-02T14:30:00Z
  &end=2024-01-02T15:00:00Z
```

Use Alpaca Basic credentials. Expected result: SIP 1-minute bars return successfully.

If this returns an entitlement/subscription error:

- Use `feed=iex` only as a temporary engineering fallback, and prominently document that it is not
  a consolidated-market execution dataset; or
- Purchase one month of Alpaca Algo Trader Plus, bulk-download the frozen historical dataset, then
  cancel.

Do not proceed beyond initial ingestion until this gate passes or a fallback is selected and
documented.

## Databento spend guard

Before any Databento historical download:

1. Call Databento's query cost estimator.
2. Print the estimated dollar cost, requested symbols, date range, schema, and expected record count.
3. Refuse to download when estimated cost exceeds `max_spend_usd`.
4. Require an explicit `--allow-paid-download` override to exceed the configured maximum.

```toml
[data.databento]
max_spend_usd = 25.00
require_explicit_paid_override = true
```

The estimate returned by Databento is authoritative. Do not hardcode a per-GB assumption or assume
the signup credit is sufficient.

## Execution-data semantics

### T2: sampled one-minute CBBO

`cbbo-1m` is a sampled consolidated top-of-book dataset, not a continuous quote stream. The
simulator must use the latest quote snapshot that existed at or before order arrival:

```
quote.ts_recv <= order.arrival_time
order.arrival_time - quote.ts_recv <= configured_max_quote_staleness
```

The simulator must **never** choose the temporally nearest quote if it occurs after the simulated
arrival time. Doing so would introduce lookahead bias.

```cpp
QuoteSnapshot quote_at_or_before(
    InstrumentId instrument,
    Timestamp arrival_time,
    Duration max_quote_staleness);
```

```toml
[execution.quote_validity]
max_staleness_t2_seconds = 60
max_staleness_t3_seconds = 2
on_stale_quote = "reject_order"
```

Research mode defaults to rejecting stale-quote orders so that data-quality limitations are visible
rather than hidden through fabricated fills.

### Sampling limitation

One-minute CBBO data bounds the normal *age* of a sampled quote observation to approximately one
minute when a record is present. It does **not** bound price error: quotes, spreads, and liquidity
can change materially between snapshots.

The T3 `cbbo-1s` subset exists to quantify the sensitivity of execution results to quote-sampling
resolution. Report at minimum:

- Fill-price difference: `cbbo-1m` vs `cbbo-1s`
- Implementation-shortfall difference
- Fill-rate difference
- Spread-cost difference
- Trade-level disagreement rate
- Results by ETF, order size, execution algorithm, and volatility regime

Do not claim queue-position realism at T2 or T3. Sampled top-of-book data cannot establish queue
position, full displayed depth, cancellations, or intra-interval quote dynamics.

## Execution-algorithm requirement

The quote-aware simulator needs a valid CBBO snapshot at every **child**-order arrival, not merely
at parent-order decision and arrival.

- **Immediate:** quote at market-order arrival, plus decision/completion marks.
- **TWAP:** quote at every scheduled child-order arrival.
- **VWAP:** quote at every child arrival, plus volume observations for participation scheduling.
- **POV:** quote at every child arrival, plus incremental observed market volume.
- **End-of-horizon residual:** explicit cancellation, mark, and opportunity-cost policy.

## Bar timestamp policy

Alpaca minute bars are **left-edge** interval labels. A bar stamped 14:52:00 covers
`[14:52:00, 14:53:00)`. The information in that bar becomes available only at 14:53:00, or after the
configured data-arrival delay.

```cpp
struct Bar {
    InstrumentId instrument;
    Timestamp    open_time;
    Timestamp    close_time;
    Price        open;
    Price        high;
    Price        low;
    Price        close;
    Volume       volume;
};
```

A strategy must not use the contents of a bar before `close_time`. Treating Alpaca's left-edge
timestamp as a close timestamp is a one-minute lookahead bug.

Dedicated normalization test proving:

```
bar.open_time < bar.close_time
bar.close_time == bar.open_time + timeframe
decision_time >= bar.close_time
```

## Dataset metadata

Every normalized dataset and every backtest run must persist:

```toml
[data.t1]
provider = "alpaca"
feed = "sip"
schema = "ohlcv-1m"
timestamp_semantics = "bar_open_time"
coverage = "consolidated_sip_historical"
historical_end_delay_minutes = 15

[data.t2]
provider = "databento"
schema = "cbbo-1m"
timestamp_semantics = "interval_end_ts_recv"
coverage = "consolidated_best_bid_offer"

[data.t3]
provider = "databento"
schema = "cbbo-1s"
timestamp_semantics = "interval_end_ts_recv"
coverage = "consolidated_best_bid_offer"
```

Do not hardcode a Databento dataset identifier until the exact venue/dataset entitlement is
confirmed for the ETF universe. Persist the actual vendor dataset, schema, symbol mapping,
retrieval date, query parameters, file checksum, and source-data version in the run manifest.

## Session policy

Initial project scope uses US regular trading hours only.

```toml
[market_session]
timezone = "America/New_York"
include_regular_trading_hours = true
regular_open = "09:30:00"
regular_close = "16:00:00"
exclude_opening_auction = true
exclude_closing_auction = true
```

The 390-minute daily assumption applies only to regular trading hours. Opening/closing auctions,
extended-hours trading, and auction-specific mechanics are out of scope initially.

## Data licensing policy

Raw vendor data must not enter the public repository.

**Allowed in Git:** ingestion code · vendor adapters · query manifests · checksums · dataset schema
and metadata · tiny synthetic fixtures · derived aggregate metrics, if permitted · documentation and
reproducibility instructions.

**Forbidden in Git:** raw Alpaca data · raw Databento data · redistributable vendor-derived event
files unless explicitly permitted by the relevant license.

CI must reject known raw-data extensions/directories before staging or release.

## Rejected alternatives

| Alternative | Rejection reason |
|---|---|
| Alpaca Algo Trader Plus from day one | Pays $99/month for real-time SIP before the project uses real-time data |
| Alpaca Basic with `feed=iex` for the main research dataset | IEX-only quotes are not a consolidated US-market/NBBO representation; use only as entitlement-gate fallback |
| Databento for minute bars | Spends usage credits on data available free through Alpaca historical SIP |
| `bbo-1m` / `bbo-1s` | Not necessarily consolidated; use `cbbo-1m` / `cbbo-1s` |
| Full-tick MBP-1 or MBO for initial scope | Excessive data volume and complexity for a 5–15 minute decision cadence; would not justify queue-realism claims without additional venue-specific modeling |
| Crypto L2 as the main project | Different market structure, fee schedules, sessions, and venue behavior; keep for an optional future module |

## Review triggers

Revisit if: the Alpaca historical SIP entitlement gate fails · Databento's estimator exceeds the
configured budget or signup credit · the liquid universe exceeds ~15 symbols · decision cadence
falls below one minute · the simulator introduces passive limit-order execution as a core result ·
the project expands to L2/L3 or queue-position research · the project begins using opening/closing
auctions or extended-hours sessions · vendor pricing, licensing, schemas, or entitlement terms
change.

## Honest project claim

The system uses historical consolidated SIP minute bars and sampled consolidated top-of-book quote
data for a liquid ETF universe. The execution simulator models observable sampled bid/ask
conditions, configured latency, fees, partial fills, participation limits, and conservative impact
assumptions. It does not claim full-depth order-book reconstruction, continuous quote-path
visibility, or queue-position realism.

---
---

# Addendum A — Implementation follow-ups

*Added after acceptance. These do not change the decision above; they are second-order consequences
that must be resolved during implementation. Each names the phase that owns it.*

## A1 — `timezone` is provenance metadata, never a runtime dependency (Phase 1)

`[market_session] timezone = "America/New_York"` is correct to record, but it must not become a
runtime `std::chrono` time-zone lookup. Verified on this toolchain: `__cpp_lib_chrono` is `201611`
even under GCC 13, i.e. no C++20 `tzdb`. libc++ shipped it only in LLVM 19, so on AppleClang it is
effectively unavailable — which is why `docs/00-architecture.md` committed to UTC-only.

**Resolution.** The field is consumed by an **offline** calendar-generation tool that emits
`data/reference/calendars/xnys_<year>.csv` as pairs of UTC instants (session open, session close),
checked in as reference data and covered by the manifest checksum. The engine reads those instants
and never performs a zone conversion.

**Enforcement.** CI grep rejecting `zoned_time`, `tzdb`, `current_zone`, `locate_zone` anywhere in
`src/` or `include/`. Failing that check is a build failure, not a warning.

## A2 — The session is 389 bars, not 390, once the opening auction is excluded (Phase 2, Phase 4)

Left-edge minute bars over `[09:30:00, 16:00:00)` are stamped 09:30 … 15:59 — **390 bars**.
`exclude_opening_auction = true` drops the 09:30 bar, because the opening auction print lands in it.
That leaves **389 tradable bars per regular session**.

Three consequences:

1. **The research's "390-minute lagged return" silently spans the overnight gap** if implemented as
   a hardcoded 390-bar lookback against a 389-bar session. The lookback must be defined as
   *"since the first tradable bar of the current session"*, not as a fixed integer.
2. **The overnight gap is a different feature and must not be blended in.** Prior-close-to-open
   returns have different dynamics from intraday returns, are not tradable by an intraday strategy,
   and would dominate a pooled regression. **Recommendation:** two explicit features —
   `intraday_session_return` (first tradable bar → current, gap-free) and `overnight_gap_return`
   (prior session close → first tradable price). Never one combined 390-lag. Needs your sign-off in
   Phase 4.
3. **Warmup accounting** for any session-spanning feature is 389, and features must reset or
   explicitly carry state across the session boundary. A rolling window that silently strides across
   the overnight gap is the same category error as (2), one level down.

Closing-auction handling needs a matching decision: the closing print is usually stamped at or after
16:00:00 and therefore already outside the range, but oversized prints occasionally land in the
15:59 bar. Phase 2's validator should flag 15:59 bars whose volume exceeds a configurable multiple
of the session median rather than assuming exclusion worked.

## A3 — Sub-minute child arrivals silently degrade T2 (Phase 9)

Your requirement that every *child* arrival needs a CBBO snapshot exposes a resolution mismatch. A
TWAP with more slices than the horizon has minutes puts multiple children inside one `cbbo-1m`
sampling interval, so they all price off the **same** snapshot. That is not a crash and not a
lookahead — it is a silent loss of the very resolution the algo comparison is measuring, and it
would systematically flatter TWAP/VWAP against Immediate.

**Resolution.** `ExecutionComparator` computes `quotes_per_child = distinct_snapshots / child_count`
per arm. Any arm with `quotes_per_child < 1.0` is **refused publication** at T2 and must be re-run
against T3. The ratio is printed in the execution-quality report for every arm regardless.

## A4 — Stale-quote rejections must not be attributed to the strategy (Phase 8, Phase 11)

`on_stale_quote = "reject_order"` is the right default, but a rejected order is not a zero-fill —
it is an *absent* order. If rejections flow into unfilled quantity, opportunity cost from a data gap
gets charged to the strategy. Sparse minutes in the less-liquid names (XLE, TLT) and halt periods
would then show up as underperformance that is really a coverage artefact.

**Resolution.** A distinct `RejectCode::StaleQuote`, bucketed and reported **separately** from
genuine unfilled residual, plus a data-quality block in every execution report: rejected-order count,
affected notional, and the distribution by symbol and time-of-day. If that block is non-trivial, the
run's execution conclusions are provisional and must say so.

## A5 — Confirm `cbbo-*` availability for the ETF universe in the Phase 2 gate (Phase 2)

Your instruction not to hardcode a Databento dataset identifier is correct, and it has a
prerequisite: **confirm the chosen US equities dataset actually serves `cbbo-1m` and `cbbo-1s` for
these nine symbols over the target date range** before T2/T3 are assumed to exist as scoped. Schema
availability varies by dataset.

**Resolution.** Extend the Phase 2 gate to two checks, both run before any ingest work proceeds:

1. Alpaca historical SIP entitlement (the `GET .../bars` call above).
2. Databento schema/date-range availability for the universe, via the metadata endpoints, followed
   by a cost estimate under `max_spend_usd`.

Neither check downloads paid data. Both are recorded in the run manifest with their timestamps.
