# Predictive Trading Lab v1.0.0

A deterministic C++23 research and execution platform for systematic trading.

## What it does

The same strategy code runs against historical replay, a paper account, and a
live broker. Only the clock, the market data source, and the broker differ.
That claim is tested, not asserted: a parity test asserts a paper session
reproduces a backtest with identical orders, identical fills, exact final equity
and an identical journal.

## Highlights

- **Deterministic replay.** Fingerprints stable across every phase, compiler and
  build configuration: `config hash 30b44e5972450aad`.
- **Look-ahead safety enforced structurally.** A seven-stage timestamp chain
  validated on every order and fill; forward-looking labels not linked into live
  binaries; leakage tests that assert a covariance is bit-identical whether or
  not future data sits in the caller's buffer.
- **One source of every dollar.** `Fill`'s constructor is private to
  `BrokerSimulator`; live fills enter through a single validated ingress.
- **Nine portfolio optimizers, six execution algorithms**, each behind one
  interface, selected by holding a different pointer.
- **Observability that cannot change behaviour**, proven by dependency direction
  rather than by convention.

## Statistics

| | |
|---|---|
| C++ files | 243 |
| Lines | ~57,000 |
| Libraries | 34 |
| Tests | 766 |
| Benchmarks | 90+ |
| ADRs | 4 active |

## Verification

Verified on GCC 13 and Clang 18: Debug, RelWithDebInfo with `-Werror`,
ASan + UBSan with leak detection, C++20, C++23, and `PTL_FORCE_RESULT_FALLBACK`.
Benchmarks and applications build and run. A clean extraction configures,
builds and passes the full suite.

AppleClang is built in CI and **has not been verified locally**.

## Known limitations

Read [`docs/limitations.md`](docs/limitations.md) before using this for
anything real. The two that matter most:

- **ADR-0001's market data entitlement has never been verified against a live
  account.** The quote-aware execution work in Phases 8–9 assumes a
  subscription nobody has confirmed.
- **No live broker connection has ever been made.** The Alpaca adapter is tested
  against a scripted transport and has never spoken to a venue.

Both are honest gaps, not oversights. Closing them requires credentials.

## Future work

- Close the entitlement gate against a real account
- Guard `to_iso8601` centrally rather than at call sites
- Formal review of `nlohmann/json` as a dependency
- Interactive Brokers adapter (one `IOrderTranslator`; nothing else changes)
- Queue-position fill modelling, if order-by-order data becomes available
