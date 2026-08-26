# ADR-0002 — Optional, not NaN, for absent prices

**Status:** Accepted
**Date:** 2026-08-21
**Supersedes:** the `Order` sketch in `docs/00-architecture.md` §5.8
**Source:** Phase 1 review, finding H-5

---

## Context

`docs/00-architecture.md` §5.8 sketched the order type as:

```cpp
struct Order {
    // ...
    Price limit_price;    // NaN for market
};
```

The deep-research specification instead sketched:

```cpp
struct Order {
    // ...
    std::optional<Price> limit_price;
};
```

The research is correct and the Phase 0 architecture was wrong. This ADR records
the correction before `Order` is written, so it is a decision rather than a
migration.

## The problem with NaN

`Price` is `NamedType<double, ..., Comparable>`, and `Comparable` yields
`operator<=>` returning `std::partial_ordering` for `double`. NaN is unordered
against everything, including itself. Verified:

| Expression | Result |
|---|---|
| `nan < Price{100}` | `false` |
| `nan >= Price{100}` | `false` |
| `nan == nan` | `false` |

Trichotomy fails: `!(a < b)` does not imply `a >= b`. Two marketability checks
that a reader would consider equivalent —

```cpp
if (quote.ask < order.limit_price)   { /* marketable */ }
if (quote.ask >= order.limit_price)  { /* not marketable */ }
```

— both take the `false` branch for a market order. Which behaviour you get
depends on which way the author happened to write the comparison. In Phase 8
this lands inside `BrokerSimulator`, the one component permitted to create a
`Fill`, and produces fills at undefined prices without any error.

It is also invisible to testing unless someone thinks to construct a NaN case,
because every ordinary limit order behaves correctly.

## Decision

1. **`std::optional<Price> limit_price`.** Absence is represented by absence.
   `has_value()` is the marketability precondition, and the compiler will not
   let a caller silently compare an empty optional as though it were a price.
2. **No NaN sentinel anywhere in the domain model.** Not for prices, not for
   quantities, not for basis points. Where a value may be absent, use
   `std::optional`. Where it may be invalid, return `ptl::Result`.
3. **`ptl::is_finite()`** (added in Phase 1, `core/types.hpp`) is the guard for
   *computed* values, which is a different problem: absence is modelled by
   `optional`, while a division or accumulation producing `inf`/NaN is a bug.
   Accounting and metrics assert finiteness per bar.
4. **`OrderType` remains the authority on order semantics.** `limit_price` being
   engaged is a consequence of `type == Limit`, not the encoding of it. A
   constructor-level invariant enforces the two agree, so they cannot drift.

## Consequences

**Positive.** The ambiguous comparison becomes a compile error rather than a
silent wrong branch. Order state is self-describing in a debugger and in the
JSON journal (`null` versus a garbage float). `std::optional<Price>` is 16 bytes
against 8 — irrelevant at this order volume, and measurable in Phase 13 if it
ever matters.

**Negative.** Every limit-price read needs `has_value()` or a `value_or`. That
verbosity is the point: it marks each site where absence had to be considered.

## Alternatives rejected

| Alternative | Why not |
|---|---|
| Keep NaN, add `is_market()` helper | The hazard is that a *direct* comparison silently misbehaves. A helper does not prevent anyone from writing the comparison. |
| A sentinel such as `Price{-1}` | Orderable, so comparisons return plausible answers — strictly worse than NaN, which at least fails loudly under a finiteness assertion. |
| Separate `MarketOrder` / `LimitOrder` types | Correct in principle, but forces a variant through the OMS, execution algorithms and journal for a distinction only the matching logic cares about. Revisit if order types proliferate. |

## Verification

- `test_named_type.cpp` already asserts `Price` is 8 bytes and trivially
  copyable; `std::optional<Price>` inherits neither and does not need to.
- Phase 3, when `Order` lands: a test asserting a market order cannot reach the
  limit-matching path, and that `type` and `limit_price` engagement agree.
- Phase 8: a nonmarketable limit must not fill (already scoped in the
  requirements matrix as `test_no_fill`).
