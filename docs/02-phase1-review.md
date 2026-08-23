# Phase 1 Code Review

**Reviewer stance:** staff engineer, systematic trading infrastructure.
**Subject:** `predictive-trading-lab` @ `v0.1-phase1`
**Verdict:** **Ship it, with four blockers fixed first.** The foundation is sound. The
defects below are real but shallow — none require rearchitecting, and the expensive
decisions (point-in-time chain, determinism contract, module boundaries) were made
correctly.

**Disclosure:** I wrote this code. Everything below was re-derived by reading and
executing it, not recalled. Where a finding is embarrassing, it is stated plainly —
a review that flatters its own output is worthless.

---

## 0. Evidence

Findings marked ✓ were reproduced, not reasoned about.

| Check | Result |
|---|---|
| `cmake -DPTL_BUILD_BENCHMARKS=ON` | ✓ **CMake Error** — `add_subdirectory(benchmarks)`, no `CMakeLists.txt` |
| `clang-format --dry-run --Werror` over the tree | ✓ **20 of 20 files fail** |
| `to_iso8601(kNoTimestamp)` | ✓ `1677-09-21T00:12:43.145224192Z` |
| `participation(Qty{100}, Volume{0})` | ✓ `inf` |
| `to_bps(Price{100}, Price{0})` | ✓ `inf` |
| NaN limit price ordering | ✓ `nan < 100` false **and** `nan >= 100` false |
| `<fstream>` in `config.cpp` | ✓ used, **not included** — compiles by transitive luck |
| Logger record timestamp source | ✓ `WallClock{}.now()`, unconditionally |
| Empty directories | ✓ 8: `tests/{property,leakage,golden,fixtures}`, `benchmarks`, `tools`, `docs`, `data/reference` |
| `sizeof` | `EventTime` 16 · `LifecycleTimes` 64 · `ObservationInterval` 32 · `log::Field` 56 |

---

## 1. Strengths

Stated first because they are the reason the rest is worth fixing rather than replacing.

**1.1 The point-in-time chain is the right abstraction, in the right place.**
Encoding `arrival_time > decision_time` as a property of a *type* rather than a
convention inside execution code is the single best decision in the repository.
Conventions get honoured in one code path and forgotten in another; a type does not.
The strict rule being checked before the generic edge walk — so the diagnosis names
same-bar execution instead of the less informative "arrival before submission" — is
the kind of detail that only shows up under a failure, which is exactly when it
matters.

**1.2 `ObservationInterval` carries four stamps, not two.**
Most retail backtesters purge by comparing a label end against a test start. With a
15-minute horizon and 5-minute steps the labels overlap, and endpoint comparison
leaves contaminated rows in training. Carrying the interval means Phase 5 can do this
correctly without a schema migration.

**1.3 The determinism contract is real and now externally verified.**
Prohibiting `<random>` distributions is not pedantry: they are specified by their
statistical properties, not their output sequence, and libstdc++ and libc++ genuinely
differ. Golden values plus your AppleClang/arm64 confirmation means the reproducibility
claim is *tested*, not asserted. Most projects claiming determinism have never checked
across a standard library boundary.

**1.4 `Timestamp` is `sys_time<nanoseconds>`, not `system_clock::time_point`.**
The distinction looks pedantic until you notice `system_clock::duration` is
microseconds on libc++ and nanoseconds on libstdc++. Using the convenience alias
would have silently given macOS microsecond resolution and Linux nanosecond, breaking
determinism in a way that no test would have caught. This was correct before we knew
why.

**1.5 Structural, not procedural, leakage defences.**
`ptl::labels` unlinked from live binaries makes a label leak a *linker error*.
`BrokerSimulator` as sole `Fill` constructor means no other module can fabricate a
fill. These survive contact with a tired developer at 2am; code review comments do not.

**1.6 Strict config validation.**
`run.sed = 42` is a hard error. Under a permissive loader the manifest records a
setting the run never applied — a reproducibility failure disguised as a typo. The
canonical form is sorted, dotted and formatting-independent, so the hash is of the
*settings*, not the author's whitespace habits.

**1.7 Trial registry exists before it is needed.**
Most projects add trial counting after a reviewer asks "how many things did you try?"
By then the answer is unknowable. Declaring budgets `INSERT OR IGNORE` — so they
cannot be raised after seeing results — is the detail that makes the mechanism real
rather than decorative.

**1.8 Dependency discipline.**
Five dependencies, all justified, all pinned, all fetched at configure time. A fresh
clone builds with `cmake && cmake --build` and nothing else. The SQLite system-first /
vendor-fallback path means no system prerequisites at all.

---

## 2. Weaknesses and technical debt

### CRITICAL

---

#### C-1 · The logger stamps records with wall-clock time

**Evidence:** `src/log/logger.cpp:104,120` — `to_iso8601(WallClock{}.now())`, unconditionally.

**Why it matters.** This breaks a stated non-negotiable. Two byte-identical backtest
runs produce *different logs*, so the determinism guarantee can never extend to log
output. Worse, you cannot correlate a log line to a simulated instant: a record about
an order rejected at simulated 14:52:00 is stamped with whatever time it happened to
be when you ran the backtest.

**Future impact — this is the expensive part.** ADR-0001 §"Paper trading parity"
requires a durable journal of *every* incoming event, decision, feature snapshot hash,
prediction, risk decision, order, acknowledgment and fill. That journal is the
mechanism by which paper trading is compared against replayed backtest. If journal
records are wall-stamped, **the parity comparison is impossible** — you cannot diff two
runs whose every line differs. Phase 12's entire deliverable depends on this being
right, and by then the journal format will be load-bearing.

**Recommended solution.** Two timestamps per record, mirroring `EventTime`:
`sim_time` from an injected `IClock`, and `wall_time` for operational forensics. Inject
the clock at `log::init()`, defaulting to `WallClock` so nothing breaks before Phase 3.
Diff-based parity then ignores `wall_time` by field name.

**Effort: 2–3 hours**, including a test that two runs produce logs identical modulo
`wall_time`. **Do this before Phase 2** — every subsequent phase adds log call sites,
and retrofitting a timestamp field across them is strictly more expensive than adding
it now.

---

### HIGH

---

#### H-1 · CI has never run and would fail immediately

**Evidence:** ✓ all 20 source files fail `clang-format --dry-run --Werror`. The
workflow's `find` also has a precedence bug: `find ... -name '*.hpp' -o -name '*.cpp'`
binds as `(dir -name '*.hpp') -o (-name '*.cpp')`, and `benchmarks/` may not exist.

**Why it matters.** A CI configuration that has never executed is not infrastructure,
it is a screenshot. The four policy guards — vendor data, time zones, queue-position
claims, `<random>` — are the most valuable part of the workflow and are entirely
unproven. Right now they provide the *appearance* of enforcement.

**Future impact.** The libc++ failure you hit is precisely what the macOS matrix job
existed to catch. It reached you because CI never ran. Every phase adds guards; if the
first push is red, the habit of ignoring CI forms immediately and never breaks.

**Recommended solution.** Run `clang-format -i` across the tree as one mechanical
commit (reviewable as "formatting only, no semantic change"). Fix the `find`
precedence with parentheses. Push and confirm green *before* Phase 2 work begins.

**Effort: 1 hour.**

---

#### H-2 · The `benchmark` preset does not configure

**Evidence:** ✓ `CMake Error at CMakeLists.txt:71 (add_subdirectory)` — `benchmarks/`
has no `CMakeLists.txt`.

**Why it matters.** A preset advertised in `CMakePresets.json` and `SETUP.md` that
errors out is a broken promise in the developer-facing surface. Anyone evaluating the
repository who tries the benchmark preset concludes the project is untested.

**Recommended solution.** Either a stub `benchmarks/CMakeLists.txt` with one trivial
benchmark, or guard the `add_subdirectory` on the file existing. Prefer the stub —
Phase 13 needs the harness anyway, and a preset that runs one benchmark is honest.

**Effort: 30 minutes.**

---

#### H-3 · No README, and the design documents are not in the repository

**Evidence:** ✓ root contains `SETUP.md` only. `docs/` is empty. No `LICENSE`.

**Why it matters.** `00-architecture.md`, `01-research-reconciliation.md` and
`0001-market-data-source.md` are the strongest artifacts this project has produced.
They demonstrate exactly what distinguishes it from a backtester with an ML model —
explicit assumptions, an auditable decision trail, honest limitations. They currently
exist outside version control, which means they can drift from the code and cannot be
cited by commit.

The absent README is more damaging than it appears. A reviewer arriving at the GitHub
page sees a build system and some headers, with no statement of what the project is or
why its design is the way it is.

**Recommended solution.** Commit all three into `docs/` and `docs/adr/`. Write a
README covering: what this is, the honest project claim from ADR-0001 verbatim, the
architecture diagram, build instructions, current phase status, and limitations. Add a
LICENSE.

**Effort: 2 hours**, mostly moving existing text.

---

#### H-4 · The session model is wrong and Phase 2 is where it bites

**Evidence:** `SessionSection::tradable_bars_per_session()` returns `390 - (exclude_opening_auction ? 1 : 0)`.

Three defects in one function:

1. **Half-days are ignored.** The day after Thanksgiving, Christmas Eve and July 3rd
   close at 13:00 — 210 minutes, not 390. NYSE has several per year, and they fall
   inside any multi-year range.
2. **`exclude_closing_auction` is accepted and then ignored** by the arithmetic.
3. **390 assumes a 1-minute bar.** Nothing in the type says so.

**Why it matters.** Any feature written against this — the research spec's
390-minute lagged return, session-relative volume baselines, warmup accounting —
silently reaches across a session boundary on every half-day. A rolling window striding
across the overnight gap is the same category error as lookahead, one level down, and
it will not announce itself. It produces plausible numbers.

**Future impact.** Phase 4's minute-of-day volume baseline is keyed on session
position. If the session length is wrong on ~6 days a year, those days' features are
misaligned. This is the kind of defect that survives to the final report and quietly
degrades every result.

**Recommended solution.** Delete this function. Session length is a property of a
*date*, answered by the Phase 2 `Calendar` loaded from
`data/reference/calendars/xnys_<year>.csv`, not a compile-time constant. Config
supplies the default schedule; the calendar supplies exceptions.

**Effort: nil to delete; the calendar is already scoped Phase 2 work.** The point is
to remove it *now* so nothing is written against it.

---

#### H-5 · NaN as the limit-price sentinel breaks ordering

**Evidence:** ✓ `nan < 100` is false **and** `nan >= 100` is false. `nan == nan` is false.

`docs/00-architecture.md` specifies `Price limit_price;  // NaN for market`. The
research specification instead says `std::optional<Price> limit_price`. The research is
right and my Phase 0 was wrong.

**Why it matters.** `Comparable` yields `partial_ordering` for `double`. With NaN, the
trichotomy fails: `!(a < b)` does not imply `a >= b`. Every marketability check,
price-collar test and limit-fill comparison in Phase 8 becomes a source of silent
wrong answers, and the wrongness is branch-dependent — `if (price < limit)` and
`if (price >= limit)` disagree.

**Future impact.** A market order that accidentally takes the limit path fills at an
undefined price. This is a fill-correctness bug in the one module allowed to create
fills.

**Recommended solution.** `std::optional<Price>`, as the research specifies. Decide
now, implement when `Order` is defined in Phase 3. Optionally add a debug assertion
that no `Price` is NaN at construction.

**Effort: nil now (decision only); ~1 hour when `Order` lands.**

---

#### H-6 · Silent `inf` from unguarded division

**Evidence:** ✓ `participation(Qty{100}, Volume{0})` → `inf`. ✓ `to_bps(Price{100}, Price{0})` → `inf`.

**Why it matters.** Zero-volume minutes are not hypothetical in this universe — XLE
and TLT have them near the open and in the last hour of quiet sessions. `inf`
participation propagates into a participation cap, then into a fill quantity, then into
a P&L number. `inf` and `NaN` do not throw; they propagate and then poison an average.
A single `NaN` in an equity curve makes the Sharpe `NaN`, and the failure surfaces
hundreds of lines downstream from its cause.

**Recommended solution.** Guard both. `participation` returns `0.0` for zero volume
(no volume means no participation is possible), and Phase 8's cost model treats that as
"no fill available" rather than "unlimited". `to_bps` against a zero reference is a
programming error — assert in debug, return `0` in release. Add a portfolio invariant
that every P&L number is finite, checked per bar.

**Effort: 1 hour** including tests.

---

#### H-7 · `<fstream>` used without inclusion

**Evidence:** ✓ `src/config/config.cpp` uses `std::ifstream`; `<fstream>` appears in no
include list. It compiles only because toml++ or `<sstream>` drags it in.

**Why it matters.** This is *exactly* the class of defect that just cost you a build:
code that works on one standard library and fails on another. A toml++ version bump or
a libc++ header reshuffle turns this into a compile error on someone else's machine.

**Recommended solution.** Add the include. Then consider `include-what-you-use` or a
CI job that compiles each header standalone — the systematic fix, not the one-line one.

**Effort: 1 minute for the include; 2 hours for the standalone-header CI job (defer).**

---

### MEDIUM

---

#### M-1 · `known_keys()` will become a merge-conflict hotspot

A hardcoded `std::set` in `config.cpp`, edited by every phase, divorced from the
structs it validates. Adding a field means editing two places, and forgetting the
second produces a confusing "unrecognised key" for a key you just added.

**Solution.** Each section declares its own keys next to its struct; the loader
composes them. **Effort: 2–3 hours. Worth doing before the config triples in size.**

#### M-2 · Date and time strings are never validated

`holdout.boundary_date`, `regular_open`, `regular_close` are strings that enter the
config hash unparsed. `boundary_date = "2024-13-45"` is accepted today and fails in
Phase 5. The holdout boundary is the one setting that must be correct *before* anyone
looks at data. **Solution:** parse and validate at load with existing
`parse_timestamp`. **Effort: 1 hour.**

#### M-3 · The chain-check pattern is specified but not implemented

`record_chain_violation()` is dead code. There is no `PTL_CHECK_CHAIN` macro, so the
documented "assert in debug, count in release" pattern does not exist — Phase 3 will
invent its own, inconsistently. **Solution:** provide the macro now, alongside the
type it guards. **Effort: 1 hour.**

#### M-4 · Global mutable state contradicts a stated principle

`g_chain_violations` and `g_loggers` are process-global. The research explicitly says
"no singleton global state". For logging this is a defensible pragmatic exception; for
a violation counter that invalidates a run it is not — with parallel folds in Phase 13,
per-run attribution is lost. **Solution:** counter moves onto the run context when one
exists (Phase 3); logging exception documented in an ADR. **Effort: defer, but write
the ADR now (30 min).**

#### M-5 · `INSERT OR REPLACE` silently destroys run history

Re-running an identical config overwrites the prior record including its
`created_utc` and `finished_utc`. **Solution:** plain `INSERT`; on conflict, report
"this exact run already exists" — which is *useful* information, since it means the
result is already known. **Effort: 30 minutes.**

#### M-6 · No transaction support in the registry

Phase 5 inserts one trial per fold per config. Autocommit means a disk sync per insert.
**Solution:** a `Transaction` RAII type. **Effort: 1 hour, defer to Phase 5.**

#### M-7 · `InstrumentTable` has no persistence

Ids are assigned by interning order. If Phase 2 ingests symbols in a different order,
every id shifts, and any cached feature matrix keyed by id is silently wrong.
**Solution:** persist the mapping in the dataset manifest and load it rather than
rebuilding. **Effort: 2 hours — Phase 2 work, but scope it explicitly or it will be
missed.**

#### M-8 · Exception vs `Result` policy is undocumented

`SimulatedClock::advance_to` throws; config returns `Result`. Both are defensible
(programming error vs recoverable input error) but the rule is nowhere stated, so
Phase 3 will guess. **Solution:** one paragraph in an ADR. **Effort: 20 minutes.**

#### M-9 · Eight empty directories

`tests/{property,leakage,golden,fixtures}`, `benchmarks`, `tools`, `docs`,
`data/reference`. Intent-signalling is legitimate, but eight empty directories at
Phase 1 reads as aspiration. The `leakage` one is worst: `[leakage]`-tagged tests
already exist — inside `tests/unit/`. **Solution:** move the three existing leakage
tests into `tests/leakage/`, delete the rest until they have content.
**Effort: 30 minutes.**

#### M-10 · `.clang-tidy` is dead configuration

A curated check list that nothing runs. **Solution:** add a CI job, expect a backlog,
fix incrementally with `WarningsAsErrors` empty. **Effort: 2 hours, defer to Phase 3.**

---

### LOW

| # | Finding | Note |
|---|---|---|
| L-1 | `log::Field` is 56 bytes; `FieldValue` carries both `string_view` and `string` | Only matters if trace logging goes into the per-event path. Revisit with a profile in Phase 13. |
| L-2 | `to_iso8601` allocates per log record | Same. Do not fix without measurement. |
| L-3 | Nanosecond `int64` range is [1677, 2262] | ✓ verified. Fine for equities; document it. |
| L-4 | No LICENSE | Fold into H-3. |
| L-5 | RNG statistical tests dominate the 1.5M assertions | Tag `[statistical]` so the fast suite stays fast as the count grows. |
| L-6 | `Registry() = default` is public with null `db_` | Make private; `open()` is the only intended path. |
| L-7 | `to_iso8601(kNoTimestamp)` yields `1677-09-21…` | ✓ Reachable via `ChainViolation::describe()`. Cosmetic; `StringMaker` already guards the test path. |

---

## 3. Things intentionally left alone

Reviewed and deliberately **not** changed. Listing them prevents relitigation.

| Decision | Why it stands |
|---|---|
| Single-threaded, mutex-per-record logger | Determinism before parallelism. No measurement justifies more. |
| FNV-1a for RunId | Not cryptographic, does not need to be. Collision risk at this scale is nil. |
| `.tsb` over Parquet | Avoids Arrow. The `ptl::io` boundary is thin enough to swap. |
| `double` behind `NamedType` over fixed point | 15 significant digits vastly exceeds a USD price. The alias makes migration compiler-enforced. |
| Builtin log sink instead of spdlog | The facade is the deliverable; the backend is swappable and currently costs nothing. |
| No `install()`/`export()` rules | This is an application, not a library. Adding them now is speculative. |
| `unique_ptr` only for polymorphism, no `shared_ptr` | Correct, and worth defending in an interview. |
| Feature-matrix caching as the headline optimisation | Algorithmic, justified by operation count, not a profile. Stands. |
| Catch2 over doctest | Compile time is not yet a problem. Do not churn. |

---

## 4. Does this align with the long-term vision?

**Yes, with one qualification.**

The architecture is production-shaped where it counts: point-in-time semantics as
types, one runtime for replay and live, a single fill-creation site, reproducibility as
a hashed contract, and enforcement mechanisms that are structural rather than
procedural. A portfolio demo would have none of these — it would have a `Backtester`
class with a `run()` method.

**The qualification is C-1.** "Backtest and paper trading differ in adapters and clocks,
not strategy semantics" is the project's central claim. A logger hard-wired to the wall
clock is a component that does *not* differ by clock, sitting on the path of the
journal that proves parity. Left alone, it means the parity claim is aspirational —
you can assert it but never demonstrate it. That is precisely the difference between
the two kinds of project.

**A second, softer concern:** H-1 and H-2 are both cases of infrastructure that
*looks* enforced but has never executed. The gap between "we have policy guards" and
"the guards have run once" is the gap between a demo and a platform. Everything else
in the review is ordinary engineering debt.

---

## 5. Refactor before Phase 2

Ordered. Total ≈ **1.5 days**.

| # | Item | Effort | Why *before* Phase 2 |
|---|---|---|---|
| 1 | H-1 · `clang-format -i` + fix CI `find`; push; confirm green | 1 h | Every later commit compounds the diff. Guards must be proven before they are trusted. |
| 2 | C-1 · Inject `IClock` into logging; add `sim_time` | 3 h | Phase 2 adds log call sites. Retrofitting a field across them costs more later, and the journal format hardens in Phase 3. |
| 3 | H-7 · Add `<fstream>` | 1 min | One line. Same defect class that just broke your build. |
| 4 | H-2 · Stub `benchmarks/CMakeLists.txt` | 30 m | A broken advertised preset. |
| 5 | H-4 · Delete `tradable_bars_per_session()` | 15 m | Removing it *before* Phase 2 guarantees nothing is written against it. |
| 6 | H-6 · Guard `participation()` and `to_bps()` | 1 h | Phase 2 validation is the first consumer. |
| 7 | H-3 · README, LICENSE, commit the three design docs | 2 h | They are the project's best artifacts and are currently outside version control. |
| 8 | M-2 · Validate date/time strings | 1 h | The holdout boundary must be correct before any data is examined — which is Phase 2. |
| 9 | M-3 · `PTL_CHECK_CHAIN` macro | 1 h | Phase 3 will otherwise invent its own. |
| 10 | M-9 · Move leakage tests; delete empty dirs | 30 m | Cheap credibility. |
| 11 | M-8, M-4 · Two short ADRs (error policy, global state) | 1 h | Decisions are cheapest to record when fresh. |

**Explicitly deferred:** M-1 (config schema) until config grows; M-5, M-6 (registry) to
Phase 5; M-7 (instrument persistence) *into* Phase 2 as scoped work; M-10 (clang-tidy)
to Phase 3; all LOW items.

---

## 6. Phase 2 roadmap — Market Data & Canonical Events

### 6.1 Prerequisites (hard gates)

1. **§5 refactors 1–6 merged and CI green.**
2. **ADR-0001 entitlement gate passes** — the `feed=sip` call with Basic credentials.
   *Nothing past ingest proceeds until this returns bars or a fallback is chosen and
   documented.* Only you can run this.
3. **Databento schema availability confirmed** for `cbbo-1m`/`cbbo-1s` over the nine
   ETFs, plus a cost estimate under `max_spend_usd`. No paid download.
4. **Holdout boundary fixed and committed** before any data is examined.

### 6.2 Implementation order

| Step | Deliverable | Definition of done |
|---|---|---|
| 2.1 | `MarketEvent` as `std::variant<Bar, Quote, Trade, CorporateAction, Timer>`, each carrying `EventTime` | Round-trip tests; `sizeof` recorded |
| 2.2 | `Calendar` + offline generator → `data/reference/calendars/xnys_<year>.csv` as UTC instants | Half-days and holidays correct for 3 years; **no tzdb symbol in `src/`** |
| 2.3 | `ptl_gate` — entitlement + schema + cost-estimate probe | Runs, writes result to the manifest |
| 2.4 | Alpaca adapter + `Manifest` (vendor, feed, schema, tz, adjustment policy, checksum) | Manifest hash enters the RunId |
| 2.5 | **Bar normalisation: left-edge → explicit `open_time`/`close_time`** | Test asserts `close_time == open_time + timeframe` and `decision_time >= close_time` |
| 2.6 | `DataValidator` | ≥8 injected defect classes detected |
| 2.7 | `.tsb` columnar writer/reader | Byte-identical round-trip; mmap read path |
| 2.8 | `InstrumentTable` persistence (M-7) | Ids stable across re-ingest; test proves it |
| 2.9 | `HistoricalMarketDataSource` — chronological k-way merge | Strict ordering; `SimulatedClock` never moves backwards |
| 2.10 | `HoldoutGuard` | Access past the boundary errors without an unlock + justification |

**2.5 is the highest-risk item in the phase.** Treating Alpaca's left-edge stamp as a
close stamp is a one-minute lookahead — the exact bug this project exists to prevent,
introduced by the data layer itself.

### 6.3 Testing strategy

- **Unit:** each type, parser and validator against hand-computed fixtures.
- **`tests/leakage/`:** bar-timestamp semantics; event ordering; holdout guard.
- **Property:** any bar sequence is chronological after normalisation; ingest is
  idempotent; `.tsb` round-trips.
- **Golden:** a committed 3-symbol × 2-session synthetic fixture whose normalised
  output is byte-compared. This is the regression net for every later phase.
- **CI:** the tzdb guard becomes load-bearing at 2.2 — verify it *fails* when you
  deliberately introduce `zoned_time`. An unproven guard is not a guard (H-1's lesson).

### 6.4 Commit boundaries

One commit per numbered step, each independently green. Plus:
`chore: apply clang-format` · `fix(ci): ...` · `feat(log): sim-time` ·
`docs: architecture, reconciliation, ADR-0001` — the §5 refactors, landed first and
separately.

### 6.5 Branch strategy

`main` protected, CI required. `phase-2/<step>` short-lived branches off `main`,
squash-merged. Tag `v0.2-phase2` when 2.10 is green. Given a single developer, the
value is not review — it is that each branch must pass CI independently, so a
bisect later lands on a working commit.

### 6.6 Risks

| Risk | Watch for | Mitigation |
|---|---|---|
| **Entitlement gate fails** | Subscription error on `feed=sip` | ADR-0001 §5 fallbacks. Decide and document, do not quietly switch to IEX. |
| **Left-edge/close-edge confusion** | Any `Bar` with a single `timestamp` field | Make the field impossible to get wrong: no single-timestamp constructor. |
| **Half-day sessions** | Ingest of Nov/Dec data | Calendar-driven from the start (H-4). |
| **Zero-volume minutes** | `inf` anywhere | H-6 guards plus a finiteness invariant. |
| **Timestamp semantics drift between vendors** | Alpaca left-edge vs Databento nanosecond | Manifest records `timestamp_semantics` per tier; validator asserts it. |
| **Scope creep into features** | Writing a rolling mean "while we are here" | Phase 2 ends at a validated, ordered event stream. Features are Phase 4. |
| **Ingest is slow and you optimise it** | Profiling the CSV parser | Ingest runs once and caches. It is explicitly cold. Do not. |

---

## 7. Summary

| Priority | Count | Blocking Phase 2 |
|---|---|---|
| CRITICAL | 1 | 1 |
| HIGH | 7 | 6 |
| MEDIUM | 10 | 4 |
| LOW | 7 | 0 |

**Phase 1 is a solid foundation.** The hard, expensive decisions were made correctly,
and the determinism claim is now verified across two standard libraries and two
architectures — which is more than most such projects can say.

The defects cluster into two honest patterns. First, **infrastructure that was written
but never executed** (CI, the benchmark preset) — the fix is to run it, and the lesson
is that unexecuted infrastructure is decorative. Second, **placeholder implementations
that will be silently wrong rather than loudly broken** (session length, NaN sentinels,
unguarded division) — these are the dangerous ones, because they produce plausible
numbers.

One and a half days of work clears the path.
