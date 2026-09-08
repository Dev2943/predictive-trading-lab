# Contributing

## The bar

A change is ready when it would survive review by someone who will be paged at
3am if it is wrong.

Concretely:

- Every new behaviour has a test whose **name states the property**
- Anything on a hot path has a benchmark
- Comments explain **why**, not what
- The determinism fingerprints are unchanged, or the change is justified in an ADR
- No new dependency without discussion

## Before you open a PR

```bash
find include src apps tests benchmarks \( -name '*.hpp' -o -name '*.cpp' \) \
  -print0 | xargs -0 clang-format --dry-run --Werror

cmake --preset asan-ubsan && cmake --build build/asan-ubsan
ctest --test-dir build/asan-ubsan

./build/.../apps/ptl_version -c config/base.toml
```

## Things that will be rejected

**A second implementation of something that exists.** Two definitions of Sharpe
is how two reports of the same run come to disagree. Audit first; extend if you
can; explain why if you cannot.

**Silent downgrades.** If a venue cannot express an order, refuse it — do not
send something adjacent. If a value is invalid, say so — do not clamp it and
continue.

**Wall clock or `<random>` on the trading path.** CI greps for both.

**A test that asserts what the code does** rather than what it must guarantee.
Those tests pass forever and catch nothing.

**Lossy serialization behind an exact checksum.** This repository has shipped
that bug twice; the second time was in the fix for the first.

## Commit messages

```
feat(module): what it enables
fix(module): what was wrong, in the imperative
perf(module): before -> after, with numbers
```

## ADRs

Write one when a decision constrains future work: an invariant, a trade-off with
a real alternative, a boundary others must respect. Do not write one for a
choice that could be reversed in an afternoon. There are five, and that is about
right for a project this size.
