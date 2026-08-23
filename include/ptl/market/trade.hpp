#pragma once

/// \file trade.hpp
/// A printed trade.

#include <optional>

#include "ptl/core/result.hpp"
#include "ptl/core/time.hpp"
#include "ptl/core/types.hpp"

namespace ptl::market {

class Trade {
public:
    /// `aggressor` is optional and defaults to absent. Trade signing requires
    /// a contemporaneous quote and a rule (Lee-Ready or similar); inventing a
    /// side here would be a silent modelling assumption dressed up as data.
    [[nodiscard]] static Result<Trade> create(InstrumentId instrument, Timestamp exchange_time,
                                              Price price, Qty size,
                                              std::optional<Side> aggressor = std::nullopt,
                                              Duration feed_latency = Duration::zero());

    [[nodiscard]] InstrumentId instrument() const noexcept { return instrument_; }
    [[nodiscard]] const EventTime& time() const noexcept { return time_; }
    [[nodiscard]] Price price() const noexcept { return price_; }
    [[nodiscard]] Qty size() const noexcept { return size_; }
    [[nodiscard]] std::optional<Side> aggressor() const noexcept { return aggressor_; }
    [[nodiscard]] Notional notional_value() const noexcept { return notional(price_, size_); }

    [[nodiscard]] friend bool operator<(const Trade& a, const Trade& b) noexcept {
        return a.time_.exchange_time < b.time_.exchange_time;
    }

private:
    Trade() = default;

    InstrumentId instrument_{kInvalidInstrument};
    EventTime time_{};
    Price price_{};
    Qty size_{};
    std::optional<Side> aggressor_{};
};

}  // namespace ptl::market
