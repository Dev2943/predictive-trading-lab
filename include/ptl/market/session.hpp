#pragma once

/// \file session.hpp
/// A single trading session, expressed entirely in UTC instants.

#include <cstdint>
#include <string_view>

#include "ptl/core/types.hpp"

namespace ptl::market {

enum class SessionKind : std::uint8_t {
    Closed,   ///< holiday or weekend; no session exists
    Regular,  ///< full session
    HalfDay,  ///< early close (NYSE: 13:00 ET)
};

[[nodiscard]] std::string_view to_string(SessionKind k) noexcept;

/// The exchange day, resolved to absolute instants.
///
/// There is deliberately no `timezone` field and no local-time member. The
/// generator resolved the exchange's local schedule to UTC once, offline; the
/// engine only ever sees instants. That is what keeps the runtime free of a
/// tzdb dependency (ADR-0001 Addendum A1) and what makes a replay bit-identical
/// on a machine whose system zone differs.
struct Session {
    Timestamp date{kNoTimestamp};   ///< UTC midnight of the exchange date
    Timestamp open{kNoTimestamp};   ///< first instant of continuous trading
    Timestamp close{kNoTimestamp};  ///< end of continuous trading
    SessionKind kind{SessionKind::Closed};

    [[nodiscard]] constexpr bool is_open() const noexcept { return kind != SessionKind::Closed; }

    [[nodiscard]] constexpr Duration length() const noexcept { return close - open; }

    /// Half-open: [open, close). A trade stamped exactly at the close belongs to
    /// the closing auction, which is out of scope (ADR-0001 session policy).
    [[nodiscard]] constexpr bool contains(Timestamp ts) const noexcept {
        return is_open() && ts >= open && ts < close;
    }

    /// Number of whole `bar` intervals in [open, close).
    ///
    /// THIS is the replacement for the deleted tradable_bars_per_session().
    /// It is a method on a SESSION -- a specific date whose boundaries are
    /// known -- not a constant on a config struct, so a half-day answers 210
    /// where a regular day answers 390 without anyone having to remember.
    [[nodiscard]] constexpr std::int64_t bar_count(Duration bar) const noexcept {
        if (!is_open() || bar <= Duration::zero()) return 0;
        return length().count() / bar.count();
    }
};

}  // namespace ptl::market
