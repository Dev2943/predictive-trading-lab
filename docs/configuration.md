# Configuration

TOML, loaded by `ptl::config::load`. Unknown keys are **rejected**, not ignored:
a typo in a config key is a silent behaviour change, and rejecting it costs one
restart instead of one bad backtest.

```bash
./apps/ptl_version -c config/base.toml
```

## Sections

### `[run]`

| Key | Default | Notes |
|---|---|---|
| `seed` | `20240101` | Seeds every RNG. **Zero is rejected** — a run whose seed is unknown cannot be reproduced |
| `results_dir` | `results` | Must be writable |
| `tag` | *(empty)* | Free-form label carried into the run id |

### `[data]` / `[databento]`

| Key | Default | Notes |
|---|---|---|
| `max_spend_usd` | `25.0` | Hard cap on paid data. A positive value is required for live |
| `schema` | | See ADR-0001 — **entitlement unverified** |

### `[session]`

Calendar and trading session definition. UTC only; there is no timezone
database dependency anywhere in the codebase, and CI greps for the symbols that
would introduce one.

### `[holdout]`

| Key | Notes |
|---|---|
| `boundary_date` | Empty means not yet ingested |
| `unlock_justification` | Required to unlock; recorded in the experiment registry |

A holdout you can peek at is not a holdout. Unlocking is deliberately annoying.

### `[log]`

| Key | Default |
|---|---|
| `level` | `info` |
| `file` | *(stderr)* |

## Two kinds of validation

**Syntactic** — `config::load`. Malformed TOML, unknown keys, wrong types.

**Semantic** — `ops::ConfigValidator`. Values that parse perfectly and are still
wrong:

```cpp
ptl::ops::ConfigValidator::Options options;
options.require_credentials = true;      // live only
options.require_risk_limits = true;
const ptl::ops::ConfigValidator validator{options};

const auto report = validator.validate(config);
if (!report.ok()) {
    std::cerr << report.describe();
    return 1;
}
```

Examples of what only the second catches:

- `seed = 0` — parses, destroys reproducibility
- `max_order_notional = 0` — parses, rejects every order the strategy sends
- `max_gross_leverage = 1e9` — parses, is a limit that can never bind and reads
  as protection on a control report

Validation collects **every** issue before returning. An operator fixing a
config at 6am should get one list, not six consecutive failed starts.

## Credentials

Environment variables, never the TOML:

```bash
export PTL_BROKER_KEY=...
export PTL_BROKER_SECRET=...
```

`validate_credentials` checks **presence only**. It never logs a value, not even
a masked prefix — a masked credential in a log is still a credential in a log.

## Build options

| Option | Default | Notes |
|---|---|---|
| `PTL_BUILD_TESTS` | `ON` | |
| `PTL_BUILD_BENCHMARKS` | `OFF` | Fetches Google Benchmark |
| `PTL_BUILD_EXAMPLES` | `ON` | Examples are compiled, so they cannot rot |
| `PTL_BUILD_APPS` | `ON` | |
| `PTL_WERROR` | `ON` | |
| `PTL_ENABLE_TRACE` | `OFF` | Trace logging on the hot path |
| `PTL_REQUIRE_EIGEN` | `ON` | `OFF` drops the model layer. Absent Eigen is a **fatal** configure error by default |
| `PTL_FORCE_RESULT_FALLBACK` | `OFF` | `Result<T>` uses `std::optional` storage; verified in CI |
| `PTL_NATIVE_ARCH` | `OFF` | **Leave off for reproducibility** — `-march=native` changes floating-point results between machines |
