#pragma once

/// \file quote_models.hpp
/// Quote conditions and trading state.
///
/// A quote is not simply a bid and an ask. It can be LOCKED (bid == ask),
/// CROSSED (bid > ask), STALE, or absent because the instrument is HALTED --
/// and each of those means something different for execution. Collapsing them
/// into "we have a quote" is how a simulator ends up filling against a
/// condition that no venue would have honoured.
///
/// ADR-0001 bounds the normal age of a sampled CBBO observation: approximately
/// one minute at T2 (`cbbo-1m`) and one second at T3 (`cbbo-1s`). Those bounds
/// are configuration here, not constants, because the two tiers coexist.

#include <cstdint>
#include <optional>
#include <string_view>

#include "ptl/core/types.hpp"
#include "ptl/market/quote.hpp"

namespace ptl::execution {

enum class QuoteCondition : std::uint8_t {
    /// Normal two-sided market: bid < ask, both positive.
    Normal,
    /// bid == ask. Legal and briefly real; a marketable order can still fill,
    /// but the spread is zero and any spread-based cost model must not divide
    /// by it.
    Locked,
    /// bid > ask. Genuinely possible ACROSS venues for microseconds, but a
    /// CONSOLIDATED snapshot should never be crossed -- if one is, the feed or
    /// our parsing is wrong, and filling against it would book a negative
    /// transaction cost.
    Crossed,
    /// Older than the tier's sampling interval allows.
    Stale,
    /// No quote has ever arrived for this instrument.
    Absent,
};

[[nodiscard]] std::string_view to_string(QuoteCondition) noexcept;

/// Whether the instrument is tradeable at all.
enum class TradingState : std::uint8_t {
    Trading,
    /// Regulatory or volatility halt. Orders rest; nothing fills.
    Halted,
    /// Before the open or after the close.
    Closed,
    /// Auction period. Continuous-trading assumptions do not describe it, so
    /// the simulator declines to fill rather than pretending they do.
    Auction,
};

[[nodiscard]] std::string_view to_string(TradingState) noexcept;

/// The sampling tier a quote came from, which sets its staleness budget.
enum class QuoteTier : std::uint8_t {
    /// cbbo-1m: one-minute sampled consolidated top of book.
    OneMinute,
    /// cbbo-1s: one-second sampled.
    OneSecond,
    /// Continuous stream (live). No sampling gap beyond feed latency.
    Continuous,
};

[[nodiscard]] std::string_view to_string(QuoteTier) noexcept;

/// Staleness budget per tier, from ADR-0001.
[[nodiscard]] Duration default_staleness_budget(QuoteTier) noexcept;

/// A quote with its assessed condition, ready for execution decisions.
struct QuoteState {
    /// std::optional, NOT a Quote by value. market::Quote has a private default
    /// constructor on purpose -- every instance goes through a validating
    /// factory -- so holding one by value would make this struct
    /// non-default-constructible. Change the CONTAINER, never the invariant,
    /// exactly as with the Phase 6 Result fallback.
    std::optional<market::Quote> quote;
    QuoteCondition condition{QuoteCondition::Absent};
    TradingState trading_state{TradingState::Closed};
    QuoteTier tier{QuoteTier::OneMinute};
    Duration age{Duration::zero()};
    bool present = false;

    /// Whether an order may execute against this state.
    ///
    /// The four refusals are deliberate and each has a distinct reason:
    /// a crossed consolidated quote indicates a data fault; a stale quote
    /// describes a market that has moved on; a halt means no venue would fill;
    /// and an absent quote is nothing to fill against.
    [[nodiscard]] bool executable() const noexcept {
        return present && trading_state == TradingState::Trading &&
               (condition == QuoteCondition::Normal || condition == QuoteCondition::Locked);
    }

    /// Why execution was refused, for the journal.
    [[nodiscard]] std::string_view refusal_reason() const noexcept;
};

}  // namespace ptl::execution
