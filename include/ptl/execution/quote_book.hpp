#pragma once

/// \file quote_book.hpp
/// Current top-of-book per instrument, with condition assessment.
///
/// NOT AN ORDER BOOK. It holds one CBBO snapshot per instrument and nothing
/// more -- no depth, no order identity, no queue. ADR-0003 explains why: with
/// sampled top-of-book data, anything resembling a queue estimate would be an
/// assumption presented as a measurement. The name says "book" because it is
/// the book of quotes we have, not a reconstruction of the venue's.
///
/// Updates are strictly monotonic per instrument. A quote older than the one
/// already held is REFUSED, not merged: out-of-order quotes mean the feed or
/// the merge is broken, and quietly accepting them would let a stale price
/// overwrite a current one.

#include <cstdint>
#include <map>
#include <optional>
#include <vector>

#include "ptl/core/result.hpp"
#include "ptl/execution/quote_models.hpp"
#include "ptl/market/quote.hpp"

namespace ptl::execution {

struct QuoteBookConfig {
    QuoteTier tier{QuoteTier::OneMinute};
    /// Zero means "use the tier default" from ADR-0001.
    Duration staleness_budget{Duration::zero()};
    /// Refuse a crossed consolidated quote outright rather than storing it.
    /// A crossed CBBO is a data fault, and keeping it would let a later
    /// staleness check pass on a quote that should never have been admitted.
    bool reject_crossed = true;
};

struct QuoteBookStats {
    std::size_t updates_accepted = 0;
    std::size_t updates_rejected_out_of_order = 0;
    std::size_t updates_rejected_crossed = 0;
    std::size_t locked_observed = 0;
    std::size_t stale_reads = 0;
    std::size_t halted_reads = 0;
};

class QuoteBook {
public:
    explicit QuoteBook(QuoteBookConfig cfg = {});

    /// Apply a quote.
    ///
    /// \returns an error when the quote precedes the one already held, or when
    ///          it is crossed and rejection is enabled.
    [[nodiscard]] Result<bool> update(const market::Quote&);

    /// Mark an instrument halted or resumed. Halts arrive out of band from the
    /// quote stream, so they are set explicitly rather than inferred from a
    /// gap -- a gap and a halt look identical in sampled data, and guessing
    /// would produce phantom halts on every quiet minute.
    void set_trading_state(InstrumentId, TradingState) noexcept;

    /// The state AS OF `now`, with staleness assessed against that instant.
    ///
    /// `now` is required rather than optional: a "current" quote has no meaning
    /// without the instant it is current at, and defaulting to wall-clock time
    /// would break replay.
    [[nodiscard]] QuoteState state_at(InstrumentId, Timestamp now) const noexcept;

    /// The stored quote regardless of staleness, for diagnostics.
    [[nodiscard]] std::optional<market::Quote> raw_quote(InstrumentId) const noexcept;

    [[nodiscard]] Timestamp last_update(InstrumentId) const noexcept;
    [[nodiscard]] std::size_t size() const noexcept { return entries_.size(); }
    [[nodiscard]] const QuoteBookStats& stats() const noexcept { return stats_; }
    [[nodiscard]] const QuoteBookConfig& config() const noexcept { return cfg_; }
    [[nodiscard]] Duration staleness_budget() const noexcept { return budget_; }

    void reset() noexcept;

private:
    struct Entry {
        std::optional<market::Quote> quote;
        Timestamp received{kNoTimestamp};
        TradingState trading_state{TradingState::Trading};
        bool present = false;
    };

    QuoteBookConfig cfg_;
    Duration budget_{};
    // std::map: iteration order is part of the determinism contract.
    std::map<std::uint32_t, Entry> entries_;
    mutable QuoteBookStats stats_;
};

}  // namespace ptl::execution
