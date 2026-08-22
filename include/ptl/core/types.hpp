#pragma once

/// \file types.hpp
/// The canonical vocabulary types. One representation per financial concept,
/// used identically by research, execution, accounting and analytics.

#include <chrono>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>

#include "ptl/core/named_type.hpp"

namespace ptl {

// --- money and quantity ---------------------------------------------------
//
// double, not fixed point. At equity minute granularity a double carries ~15
// significant digits, far more than a USD price needs, and the NamedType
// wrapper supplies the safety that fixed point is usually reached for. If the
// project ever moves to 8-decimal instruments, changing the alias below makes
// the compiler enumerate every site needing attention.
// See docs/adr/0002-price-representation.md.

using Price = NamedType<double, struct PriceTag, Arithmetic, Comparable>;
using Qty = NamedType<double, struct QtyTag, Arithmetic, Comparable>;
using Volume = NamedType<double, struct VolumeTag, Arithmetic, Comparable>;
using Notional = NamedType<double, struct NotionalTag, Arithmetic, Comparable>;
using Bps = NamedType<double, struct BpsTag, Arithmetic, Comparable>;

// Qty and Volume are both share counts but are NOT interchangeable: Qty is our
// order/position size, Volume is market activity. Participation limits relate
// the two, and doing so requires an explicit, reviewable conversion.
//
// Zero volume returns 0, not infinity. Zero-volume minutes are real in this
// universe -- XLE and TLT have them near the open and in quiet afternoons. An
// inf here would flow into a participation cap, then a fill quantity, then a
// P&L number, and a single non-finite value makes an entire Sharpe ratio NaN
// hundreds of lines away from its cause.
//
// Note the semantics carefully: 0 means "no participation is measurable",
// NOT "unlimited participation". A caller enforcing a cap must treat zero
// volume as no liquidity available, not as an unconstrained fill.
[[nodiscard]] constexpr double participation(Qty q, Volume v) noexcept {
    return v.get() == 0.0 ? 0.0 : q.get() / v.get();
}

/// True when every component of a computed value is finite. Accounting and
/// metrics assert on this: inf and NaN do not throw, they propagate silently.
[[nodiscard]] constexpr bool is_finite(double x) noexcept {
    return x == x && x != std::numeric_limits<double>::infinity() &&
           x != -std::numeric_limits<double>::infinity();
}

/// Price x Qty -> Notional. The only sanctioned cross-unit product.
[[nodiscard]] constexpr Notional notional(Price p, Qty q) noexcept {
    return Notional{p.get() * q.get()};
}

/// Apply a basis-point adjustment. `direction` is +1 to pay up, -1 to improve.
/// Centralised so "is it /10000 or /100?" is answered exactly once.
[[nodiscard]] constexpr Price apply_bps(Price p, Bps b, int direction) noexcept {
    return Price{p.get() * (1.0 + static_cast<double>(direction) * b.get() * 1e-4)};
}

/// Signed relative difference of `p` against `reference`, in basis points.
///
/// A zero reference price is a programming error, not a market condition: no
/// instrument in scope ever trades at zero. Returning 0 rather than inf keeps
/// the poison out of downstream aggregates; the debug assertion is what
/// actually finds the bug.
[[nodiscard]] constexpr Bps to_bps(Price p, Price reference) noexcept {
    if (reference.get() == 0.0) return Bps{0.0};
    return Bps{(p.get() / reference.get() - 1.0) * 1e4};
}

// --- identifiers ----------------------------------------------------------
//
// A dense u32 rather than a string. Per-instrument state becomes a flat vector
// indexed directly instead of a hash lookup, hot structs stay small, and --
// importantly -- iteration is in insertion order rather than hash order, which
// removes a determinism hazard.
enum class InstrumentId : std::uint32_t {};
inline constexpr InstrumentId kInvalidInstrument{0xFFFFFFFFu};

[[nodiscard]] constexpr std::uint32_t index_of(InstrumentId i) noexcept {
    return static_cast<std::uint32_t>(i);
}

enum class Side : std::uint8_t { Buy = 0, Sell = 1 };

/// +1 for Buy, -1 for Sell. Position deltas and signed notionals go through
/// this rather than ternaries scattered across the codebase.
[[nodiscard]] constexpr int sign_of(Side s) noexcept {
    return s == Side::Buy ? 1 : -1;
}

[[nodiscard]] constexpr Side opposite(Side s) noexcept {
    return s == Side::Buy ? Side::Sell : Side::Buy;
}

[[nodiscard]] constexpr std::string_view to_string(Side s) noexcept {
    return s == Side::Buy ? "BUY" : "SELL";
}

// --- time -----------------------------------------------------------------
//
// Always UTC, always nanoseconds. No time zones anywhere in the engine:
// libc++ lacks the C++20 tzdb, and depending on the host zone database would
// break bit-reproducibility across machines. Exchange sessions are loaded as
// precomputed UTC instants from data/reference/calendars/ (ADR-0001 A1).
using Timestamp = std::chrono::sys_time<std::chrono::nanoseconds>;
using Duration = std::chrono::nanoseconds;

inline constexpr Timestamp kNoTimestamp{Duration::min()};
inline constexpr Timestamp kMaxTimestamp{Duration::max()};

[[nodiscard]] constexpr bool is_set(Timestamp ts) noexcept {
    return ts != kNoTimestamp;
}

/// Parse "YYYY-MM-DD" or "YYYY-MM-DDTHH:MM:SS[.fffffffff]" (trailing Z
/// optional, space accepted for T) as UTC. Hand-rolled rather than
/// std::get_time: that is locale-dependent, discards sub-second precision, and
/// is roughly two orders of magnitude slower -- which matters at millions of
/// rows.
[[nodiscard]] bool parse_timestamp(std::string_view text, Timestamp& out) noexcept;

/// Strict date-only parse. Rejects anything with a time component.
///
/// parse_timestamp() accepts both "2024-09-01" and "2024-09-01T14:52:00Z",
/// which is right for event data but wrong for a configured DATE: silently
/// accepting a datetime where a date is meant hides a units error in a field
/// that must be exactly correct before anyone looks at data.
[[nodiscard]] bool parse_date(std::string_view text, Timestamp& out) noexcept;

/// Parse "HH:MM:SS" as a nanosecond offset from midnight.
///
/// Deliberately NOT a Timestamp: a session open is a time of day, not an
/// instant. Turning it into an instant requires a date and a calendar, which is
/// Phase 2's job. Keeping the type honest stops anyone treating 09:30:00 as
/// though it were a point in time.
[[nodiscard]] bool parse_time_of_day(std::string_view text, Duration& out) noexcept;

[[nodiscard]] std::string to_iso8601(Timestamp ts);
[[nodiscard]] std::string to_date_string(Timestamp ts);
[[nodiscard]] Timestamp utc_date_floor(Timestamp ts) noexcept;

}  // namespace ptl
