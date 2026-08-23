# Setting up Phase 1 on macOS (Apple Silicon)

Target environment: **Apple Silicon Mac, macOS, AppleClang.** The repository also
builds under Linux GCC and Clang; CI exercises all three.

---

## 1. Prerequisites

```bash
# Xcode command line tools (compiler, SDK, system SQLite)
xcode-select --install

# Build tools
brew install cmake ninja

# Optional, not needed until Phase 6 (models)
brew install eigen
```

Check what you have:

```bash
clang++ --version     # AppleClang 15+ recommended; 14 works (see §5)
cmake --version       # 3.24 or newer required; tested through 4.4
ninja --version
```

Nothing else is required. toml++, CLI11 and Catch2 are fetched automatically at
configure time. SQLite is taken from the macOS SDK if present, and vendored
automatically if not.

---

## 2. Put the tree in place and initialise git

Run these **one at a time**, not as a chained one-liner. If the archive is not
where you think it is, `tar` and `cd` fail but a chained `git init` still runs --
in whatever directory you happen to be in. Initialising a repository in your home
folder is harmless but annoying to undo.

```bash
mkdir -p ~/dev && cd ~/dev
tar xzf ~/Downloads/predictive-trading-lab-phase1.tar.gz

cd predictive-trading-lab
pwd          # confirm you are inside the project before the next line
ls CMakeLists.txt

git init
git add -A
git commit -m "Phase 1: repository discipline, core types, point-in-time primitives"
```

If you did accidentally create a repository somewhere unintended, nothing is
damaged -- `git add` copies file contents into `.git` and never modifies your
files. Confirm the location, then remove it:

```bash
cd ~ && git rev-parse --show-toplevel     # confirm it says what you expect
rm -rf ~/.git
```

Git is not optional decoration here. `cmake/GitVersion.cmake` embeds the commit
SHA and dirty flag into the binary, and the SHA is one of the four inputs to the
RunId. The build works without a repository, but every run will report its
commit as `unknown`, which defeats the point.

---

## 3. Build and test

```bash
cmake --preset macos-debug
cmake --build --preset macos-debug
ctest --preset macos-debug
```

Expected tail:

```
100% tests passed, 0 tests failed out of 59
```

The first configure takes a few minutes while dependencies are fetched and
Catch2 is compiled. Subsequent builds are seconds.

Other presets:

| Preset | Purpose |
|---|---|
| `macos-debug` | Debug, trace logging compiled in |
| `macos-release` | RelWithDebInfo, host-CPU tuning |
| `asan-ubsan` | AddressSanitizer + UndefinedBehaviorSanitizer |
| `coverage` | Coverage instrumentation |
| `benchmark` | Release, Google Benchmark enabled (Phase 13) |

Note on `macos-release`: it sets `PTL_NATIVE_ARCH=ON`, which probes for
`-march=native` and falls back to `-mcpu=native`. On Apple Silicon the former is
rejected by AppleClang and the latter is used; the probe handles this so the
preset works unchanged on both Intel and M-series.

---

## 4. Run the probe

```bash
./build/macos-debug/apps/ptl_version -c config/base.toml
```

You should see something close to this, with your own compiler and commit:

```
predictive-trading-lab 0.1.0

build
  git                    a1b2c3d (clean)
  compiler               AppleClang 16.0.0.16000026
  build type             Debug

toolchain capability probes
  std::expected          yes
  std::format            yes
  chrono tzdb            absent (as expected; engine is UTC-only)

point-in-time chain self-check
  well-ordered chain     accepted
  same-bar execution     rejected: arrival_time must be strictly after
                         decision_time: ... [same-bar execution: a decision
                         cannot fill at the price that produced it]

configuration
  config hash            30b44e5972450aad
  run id                 <differs: your git SHA feeds the RunId>
  tradable bars/session  389
  rng[0..2] from seed    d05ef55272cdfb14 2e2f422341add64e 1c120f3d1ce63170
```

**Two values to check carefully.**

`config hash` must be exactly `30b44e5972450aad`. It is computed from the
canonical form of `config/base.toml` and nothing else, so it is identical on
every machine. A different value means the config file was modified.

`rng[0..2]` must be exactly `d05ef55272cdfb14 2e2f422341add64e 1c120f3d1ce63170`.
This is the cross-platform determinism check: the same three numbers appear
under AppleClang on arm64 and under GCC on x86-64. If they differ, results
generated on your Mac would not reproduce anywhere else, and every RunId the
project has recorded is meaningless. `test_rng.cpp` asserts the same values, so
`ctest` catches it too.

`run id` will differ from any other machine, because it hashes the git SHA.
That is intended.

Try the registry and the strict loader:

```bash
# writes a run record, then inspect it
./build/macos-debug/apps/ptl_version -c config/base.toml --registry results/registry.sqlite
sqlite3 results/registry.sqlite "SELECT run_id, compiler, build_type, seed FROM runs;"

# an override changes the RunId
./build/macos-debug/apps/ptl_version -c config/base.toml --set run.seed=999

# a typo is a hard error, not a silent default
./build/macos-debug/apps/ptl_version -c config/base.toml --set run.sed=42
#   -> config error: unrecognised configuration key(s): run.sed   (exit 2)
```

---

## 5. If something goes wrong

**`No CMAKE_CXX_COMPILER could be found`**
Command line tools are not installed or not selected:
`xcode-select --install`, then `sudo xcode-select --reset`.

**Configure prints `C++23 unavailable on this toolchain; building as C++20`**
Expected on older Xcode, and harmless. C++20 is the project baseline; nothing
requires C++23 except `std::expected`, and `ptl::Result` falls back
automatically. CI builds both paths on every commit precisely so this is not a
surprise.

**`std::expected  no (using ptl::Result fallback)` in the probe output**
Also fine, same reason. To exercise this path deliberately on a newer toolchain:
`cmake -S . -B build/fb -DPTL_FORCE_RESULT_FALLBACK=ON`.

**`CMake Warning (deprecated) at .../cli11-src/CMakeLists.txt:1`**
Cosmetic, and not from this project: CLI11 declares an old
`cmake_minimum_required` that CMake 4.x flags. It is an author warning aimed at
CLI11's maintainers. Silence it with `-Wno-dev` if it bothers you. The build is
verified on CMake 3.28 and 4.4.

**`Could NOT find SQLite3`**
Harmless. The build detects this and vendors the SQLite amalgamation from
GitHub automatically. Configure output will say
`SQLite3: system not found, vendoring amalgamation`.

**FetchContent fails / network restricted**
The build needs GitHub access on first configure for toml++, CLI11 and Catch2.
Behind a proxy, set `HTTPS_PROXY` before configuring. Once fetched, the
`build/<preset>/_deps` cache means subsequent configures are offline.

**Leak detection on the `asan-ubsan` preset**
LeakSanitizer is not available on macOS, and ASan on Darwin does not enable leak
detection by default. Configure prints a note saying so. Run the leak-checking
job on Linux CI rather than assuming your local ASan run covered it.

**Compile error in `catch_tostring.hpp` about `time_point<..., nanoseconds>`**
Fixed. `system_clock::duration` is microseconds on libc++ (AppleClang) and
nanoseconds on libstdc++ (GCC), so Catch2's stringifier for our nanosecond
`Timestamp` compiles on Linux and fails on macOS.
`tests/support/ptl_catch.hpp` supplies an explicit `StringMaker<ptl::Timestamp>`
specialisation, which is more specialised than Catch2's partial one and is
therefore selected on every platform -- Catch2's broken version is never
instantiated. No third-party source is modified.
`tests/unit/test_catch_integration.cpp` asserts the selection actually happens.

**Tests pass when run directly but `ctest` shows one giant failing entry**
That was a real bug, fixed: a Catch2 test name containing `[` reads as a tag
spec. If you add tests, keep `[`, `]` and `,` out of test *names* — they belong
in the second argument, which is the tag list.

---

## 6. What is actually here

```
include/ptl/core/     compiler probes, strong types, timestamp chain, clock,
                      RNG, instrument interning, Result
include/ptl/log/      structured JSONL logging facade
include/ptl/config/   strict TOML loader, canonical form, RunId
include/ptl/experiments/  SQLite run + trial registry
apps/ptl_version/     build and configuration probe
tests/unit/           59 test cases, ~1.5M assertions
.github/workflows/    build matrix + four policy guards
```

The four CI policy guards fail the build if anyone commits vendor data, uses a
runtime time zone, introduces a `queue_position` field, or reaches for a
`<random>` distribution. They run first and take seconds. You can run them
locally:

```bash
grep -rnE 'zoned_time|std::chrono::tzdb|current_zone' include/ src/ apps/ \
  | grep -vE ':[0-9]+: *(//|///|/\*|\*)'
# no output = clean
```

---

## 7. Next

Phase 2 is the market data layer: canonical `MarketEvent` as a `std::variant`,
the session calendar generator, the Alpaca ingest tool, and the validator.

Before it can do anything useful it needs the **ADR-0001 entitlement gate** run
against your own Alpaca Basic credentials:

```
GET https://data.alpaca.markets/v2/stocks/bars
  ?symbols=SPY&timeframe=1Min&feed=sip
  &start=2024-01-02T14:30:00Z&end=2024-01-02T15:00:00Z
```

If that returns bars, the free tier covers the whole research pipeline. If it
returns a subscription error, the fallback options are in ADR-0001 §5.
