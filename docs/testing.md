# Testing

766 tests in five categories. Each answers a different question, and the
category a test belongs in is a design statement.

```bash
ctest --test-dir build/linux-gcc-release              # all
ctest --test-dir build/... -R "\[leakage\]"           # by tag
./build/.../tests/ptl_tests "[optimization]"          # by Catch2 tag
```

## Categories

**`tests/unit/`** — one component, one behaviour. The bulk of the suite.

**`tests/property/`** — invariants over generated inputs. "No optimizer produces
a NaN weight, for any of these hostile covariance matrices" is a property; a
single hand-written case is not.

**`tests/leakage/`** — the ones that matter most. They assert that information
which should not be available *is* not: a covariance estimated to time T is
bit-identical whether or not later data sits in the caller's buffer; a walk-forward
fold cannot see past its boundary; a paper session reproduces a backtest exactly.

**`tests/golden/`** — byte-for-byte comparisons against recorded output. These
catch changes nobody intended, including changes that look like improvements.

**`tests/integration/`** — whole pipelines end to end.

## Determinism tests

Several tests assert **exact** floating-point equality (`==`, not `Approx`).
That is deliberate. Float summation is not associative, so any change in
iteration order shows up as a differing last bit. An approximate comparison
would pass through exactly the reordering these tests exist to catch.

## What a good test looks like here

The test name states the property, not the mechanics:

```cpp
TEST_CASE("a paper session reproduces a backtest exactly",
          "[paper][session][parity][determinism]")
```

and the body explains why the property matters, not what the code does:

```cpp
// THE CENTRAL CLAIM OF PHASE 14. A paper session and a backtest run the same
// engine over the same strategy; only the clock and source differ. Anything
// that diverges is a bug, not a tolerance to widen.
```

A test whose comment restates its assertions has told the next reader nothing.

## Sanitizers

```bash
cmake --preset asan-ubsan
cmake --build build/asan-ubsan
ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 \
  ctest --test-dir build/asan-ubsan
```

Worth running before every merge. Sanitizers have caught two bugs in this
repository that every other configuration passed over: an `int64` overflow in
timestamp formatting, and an uninitialized pointer that GCC happened to zero.
