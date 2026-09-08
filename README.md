# Predictive Trading Lab

A deterministic C++23 research and execution platform for systematic trading.

The same strategy code runs against historical replay, a simulated paper
account, and a live broker. Only the clock, the market data source, and the
broker differ; every other component — features, models, signals, sizing,
optimization, risk, OMS, execution algorithms, portfolio, accounting,
analytics — is the identical code path.

```
Market data → Features → Model → Signals → Sizing → Optimizer → Targets
            → Rebalance → Risk → OMS → Execution algorithms → Broker
            → Portfolio → Accounting → Analytics → Reporting
```

**Status: v1.0.** 766 tests, 243 C++ files, ~57,000 lines. Verified on GCC 13
and Clang 18 across Debug, Release, ASan+UBSan, C++20 and C++23.

---

## What this is, and what it is not

**It is** the plumbing a systematic trading group needs before a strategy is
worth writing: point-in-time correct feature engineering, walk-forward
validation with an enforced holdout, a fill simulator that does not lie to you,
an order lifecycle that reconciles to the cent, and an execution path that
behaves identically in backtest and production.

**It is not** a strategy. The strategies in this repository exist to exercise
the machinery, and none of them has an edge. If you are looking for alpha, this
is the workbench, not the tool.

---

## Guarantees

These are enforced by tests, CI guards, and in several cases the type system.
They are the reason to prefer this over a notebook.

| Guarantee | How it is enforced |
|---|---|
| **Deterministic replay** — same inputs, same bytes out | Fingerprint tests; no `<random>` distributions; no wall clock on the trading path |
| **Replay / paper / live parity** | One `Engine`, one dispatch loop; a parity test asserts a paper session reproduces a backtest exactly |
| **No look-ahead** | Seven-stage timestamp chain, validated on every order and fill; `ptl::labels` is not linked into live binaries |
| **One source of every dollar** | `Fill`'s constructor is private to `BrokerSimulator`; live fills enter through one validated ingress |
| **Accounting closes** | Journal reconciles against the portfolio; the identity is asserted, not assumed |
| **Observability cannot change behaviour** | `ptl::ops` depends on the trading modules; none of them depends on it |

---

## Quick start

Requires CMake ≥ 3.24, a C++20 compiler (C++23 preferred), and Eigen 3.

```bash
git clone https://github.com/Dev2943/predictive-trading-lab
cd predictive-trading-lab

cmake --preset linux-gcc-release      # or macos-release
cmake --build build/linux-gcc-release
ctest --test-dir build/linux-gcc-release

./build/linux-gcc-release/apps/ptl_version -c config/base.toml
```

That last command prints the determinism fingerprints. If they differ from the
values in [`docs/reproducibility.md`](docs/reproducibility.md), something in
your toolchain is not reproducing our build, and that is worth understanding
before you trust a backtest from it.

See [`docs/quickstart.md`](docs/quickstart.md) for a first backtest, and
[`examples/`](examples/) for complete programs.

---

## Documentation

| Document | Contents |
|---|---|
| [Quick start](docs/quickstart.md) | Build, run a backtest, read the output |
| [Architecture](docs/architecture.md) | Module map, dependency rules, data flow |
| [Execution modes](docs/execution-modes.md) | Replay vs paper vs live, and what differs |
| [Configuration](docs/configuration.md) | Every option, with defaults and rationale |
| [Developer guide](docs/developing.md) | Layout, conventions, adding a module |
| [Testing guide](docs/testing.md) | Test categories and what each is for |
| [Benchmarks](docs/benchmarks.md) | Methodology, and how to read the numbers |
| [Reproducibility](docs/reproducibility.md) | Fingerprints, seeds, and what breaks them |
| [Contributing](CONTRIBUTING.md) | Review standards and the bar for merging |
| [ADRs](docs/adr/) | Decisions that constrain everything downstream |
| [Changelog](CHANGELOG.md) | Phase-by-phase history |

---

## Repository layout

```
include/ptl/          Public headers, one directory per module
src/                  Implementations, mirroring include/
apps/                 Command-line tools (ptl_gate, ptl_version)
examples/             Complete, compiled example programs
tests/                unit/ property/ leakage/ golden/ integration/
benchmarks/           Google Benchmark micro-benchmarks
config/               TOML configuration
docs/                 Documentation and ADRs
cmake/                Build helpers, warnings, dependency resolution
tools/                Developer scripts
```

Examples are **compiled by the build and run in CI**. An example that does not
compile is worse than no example, because it teaches an API that no longer
exists.

---

## Module map

Thirty-four libraries in five layers. Dependencies point downward only.

**Foundation** — `core` `log` `config`
Named types, `Result<T>`, deterministic RNG, UTC time, structured logging.

**Market & research** — `market` `databento` `features` `labels` `validation` `research` `models` `experiments`
Calendars, bars, quotes, point-in-time features, walk-forward splits, linear and
logistic models behind an Eigen boundary.

**Trading** — `signal` `sizing` `optimization` `construction` `oms` `risk` `execution` `algo` `portfolio` `accounting` `pipeline`
Signals to targets to orders to fills to positions. Nine portfolio optimizers,
six execution algorithms, a quote-aware fill simulator.

**Sessions** — `engine` `paper` `live` `strategy` `storage` `experiment`
One event loop; three brokers behind one shape; strategy and dataset registries.

**Observation** — `analytics` `attribution` `report` `reporting` `ops`
Performance and risk analytics, P&L attribution, typed reports, operational
telemetry. All read-only with respect to trading.

Full detail in [`docs/architecture.md`](docs/architecture.md).

---

## Known limitations

Stated plainly, because a platform that hides these is harder to trust than one
that does not.

- **The market data entitlement gate (ADR-0001) has never been verified against
  a live account.** Phases 8 and 9 assume a Databento `cbbo-1m` subscription is
  available. `schema_probe_request()` issues the documented call, but no
  authenticated request has ever been made. If the entitlement is absent, the
  quote-aware execution work rests on an untested assumption.
- **No live broker connection has ever been made.** The Alpaca adapter encodes
  request bodies correctly and is tested against a scripted transport, but has
  never spoken to a venue.
- **AppleClang is built in CI but is not verified locally**, so the numbers in
  this README come from GCC 13 and Clang 18 on Linux.
- **Fill simulation is conservative, not queue-position based** (ADR-0003). It
  will understate fill rates for passive strategies.
- **Single-threaded by design.** Throughput is bounded by one core. This is a
  deliberate trade for determinism, not an oversight.
- **`nlohmann/json` is load-bearing** for the Databento decoder and has not been
  through a formal dependency review.

See [`docs/limitations.md`](docs/limitations.md) for the full list and the
reasoning behind each.

---

## License

See [`LICENSE`](LICENSE).
