#pragma once

/// \file quote.hpp
/// A top-of-book snapshot, likewise impossible to construct invalid.
///
/// This is a SAMPLED snapshot (Databento cbbo-1m/cbbo-1s), not a continuous
/// stream. The simulator must use the latest snapshot at or before order
/// arrival and never the temporally nearest one -- see ADR-0001. Nothing in
/// this type can enforce that; it is the consumer's obligation, and
/// `staleness_at()` exists to make it checkable.

#include "ptl/core/result.hpp"
#include "ptl/core/time.hpp"
#include "ptl/core/types.hpp"

namespace ptl::market {

class Quote {
public:
    [[nodiscard]] static Result<Quote> create(InstrumentId instrument, Timestamp exchange_time,
                                              Price bid, Qty bid_size, Price ask, Qty ask_size,
                                              Duration feed_latency = Duration::zero());

    [[nodiscard]] InstrumentId instrument() const noexcept { return instrument_; }
    [[nodiscard]] const EventTime& time() const noexcept { return time_; }
    [[nodiscard]] Price bid() const noexcept { return bid_; }
    [[nodiscard]] Price ask() const noexcept { return ask_; }
    [[nodiscard]] Qty bid_size() const noexcept { return bid_size_; }
    [[nodiscard]] Qty ask_size() const noexcept { return ask_size_; }

    [[nodiscard]] Price mid() const noexcept { return Price{(bid_.get() + ask_.get()) * 0.5}; }
    [[nodiscard]] Price spread() const noexcept { return ask_ - bid_; }
    [[nodiscard]] Bps spread_bps() const noexcept { return to_bps(ask_, mid()) * 2.0; }

    /// How old this snapshot is at `ts`. Negative means the snapshot is in the
    /// FUTURE relative to ts, which a consumer must treat as unusable: using it
    /// is lookahead.
    [[nodiscard]] Duration staleness_at(Timestamp ts) const noexcept {
        return ts - time_.receive_time;
    }

    [[nodiscard]] friend bool operator<(const Quote& a, const Quote& b) noexcept {
        return a.time_.exchange_time < b.time_.exchange_time;
    }

private:
    Quote() = default;

    InstrumentId instrument_{kInvalidInstrument};
    EventTime time_{};
    Price bid_{};
    Price ask_{};
    Qty bid_size_{};
    Qty ask_size_{};
};

}  // namespace ptl::market
