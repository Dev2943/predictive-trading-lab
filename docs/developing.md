# Developer guide

## Conventions that are not negotiable

**Named types over primitives.** `Price`, `Qty`, `Notional`, `Bps`,
`InstrumentId` are distinct types. This is not ceremony: it has caught real
argument-order errors that raw `double`s would have compiled silently, including
a `Quote::create(bid, ask, bid_size, ask_size)` call where the signature
interleaves price and size.

**`Result<T>`, not exceptions**, on any path that can fail from data. Errors
carry a message and context, and `[[nodiscard]]` makes ignoring one a warning.

**No wall clock on the trading path.** Take time from the event or the injected
`IClock`. A component that reads `system_clock::now()` cannot be replayed.

**No `<random>` distributions.** Their implementations differ between standard
libraries, so the same seed gives different numbers on different platforms. Use
`DeterministicRng`. CI greps for the banned symbols.

**UTC only.** No timezone database, anywhere. CI greps for `zoned_time` and
friends.

**Round-trip exact serialization** for anything a checksum covers. Writing a
double at six decimals and hashing its raw bits means the checksum refuses its
own output for almost every real value — a bug this repository has shipped and
fixed twice, in two different modules.

## Adding a module

1. `include/ptl/yourmodule/`, `src/yourmodule/`
2. `add_library(ptl_yourmodule ...)` plus a `ptl::yourmodule` alias
3. Link **only** what you use. The dependency direction in
   [architecture.md](architecture.md) is checkable, and keeping it that way is
   how it stays true.
4. Tests in the category that matches the question they answer
5. Benchmarks for anything on a hot path

Before writing: **audit for what already exists.** Roughly a third of the work
proposed across this project's later phases was already implemented, and the
right move each time was to extend rather than to build a second implementation
that could disagree with the first.

## Comments

Explain **why**, never what. A comment restating the code it sits above has told
the next reader nothing:

```cpp
// BAD:  increment the counter
++count_;

// GOOD: Bit patterns, so a one-ulp difference on restore is caught rather
//       than rounded away.
hash_bytes(h, &cash, sizeof(cash));
```

The comments worth writing are the ones that record a decision someone would
otherwise reverse: why a limit is checked here and not there, why a value is
refused rather than clamped, what breaks if the order changes.

## Before opening a PR

```bash
find include src apps tests benchmarks \( -name '*.hpp' -o -name '*.cpp' \) \
  -print0 | xargs -0 clang-format --dry-run --Werror

cmake --preset asan-ubsan && cmake --build build/asan-ubsan
ctest --test-dir build/asan-ubsan

./build/.../apps/ptl_version -c config/base.toml   # fingerprints unchanged?
```

A fingerprint change is either a bug or a deliberate decision that belongs in an
ADR. It is never incidental.
