#pragma once

/// \file bar.hpp
/// An OHLCV bar that cannot be constructed in an invalid state.
///
/// The constructor is private. Every path to a Bar goes through a factory that
/// returns Result<Bar>, so a malformed bar does not exist to be passed around --
/// it is an error value at the point of parsing, where the row number is still
/// known and the message can be useful.
///
/// The single most important invariant is the timestamp pair. Alpaca stamps
/// minute bars with the LEFT EDGE of the interval: a bar stamped 14:52:00 covers
/// [14:52:00, 14:53:00) and its contents are not knowable until 14:53:00.
/// Treating that stamp as a close time is a one-minute lookahead -- the exact
/// bug class this project exists to prevent, introduced by the data layer
/// itself. There is therefore NO constructor taking a single timestamp.

#include <string>

#include "ptl/core/result.hpp"
#include "ptl/core/time.hpp"
#include "ptl/core/types.hpp"

namespace ptl::market {

class Bar {
public:
    /// Build from a left-edge stamp: the convention Alpaca and most bar vendors
    /// use. `open_time` is the START of the interval.
    [[nodiscard]] static Result<Bar> from_left_edge(InstrumentId instrument, Timestamp open_time,
                                                    Duration timeframe, Price open, Price high,
                                                    Price low, Price close, Volume volume,
                                                    Duration feed_latency = Duration::zero());

    /// Build from a right-edge stamp: some vendors label a bar with the instant
    /// it closed. Present so that a vendor using the other convention is
    /// handled by naming it, not by subtracting a timeframe at the call site
    /// and hoping every call site remembers.
    [[nodiscard]] static Result<Bar> from_right_edge(InstrumentId instrument, Timestamp close_time,
                                                     Duration timeframe, Price open, Price high,
                                                     Price low, Price close, Volume volume,
                                                     Duration feed_latency = Duration::zero());

    [[nodiscard]] InstrumentId instrument() const noexcept { return instrument_; }

    /// Start of the interval. NOT the time the bar became knowable.
    [[nodiscard]] Timestamp open_time() const noexcept { return open_time_; }

    /// End of the interval, and the earliest instant the bar's contents exist.
    [[nodiscard]] Timestamp close_time() const noexcept { return close_time_; }

    [[nodiscard]] Duration timeframe() const noexcept { return close_time_ - open_time_; }

    /// exchange_time is the bar's CLOSE, because that is when the aggregate
    /// came into existence. receive_time adds modelled feed latency on top.
    [[nodiscard]] const EventTime& time() const noexcept { return time_; }

    [[nodiscard]] Price open() const noexcept { return open_; }
    [[nodiscard]] Price high() const noexcept { return high_; }
    [[nodiscard]] Price low() const noexcept { return low_; }
    [[nodiscard]] Price close() const noexcept { return close_; }
    [[nodiscard]] Volume volume() const noexcept { return volume_; }

    /// Close times are what order events; open times would collide across
    /// instruments with different timeframes.
    [[nodiscard]] friend bool operator<(const Bar& a, const Bar& b) noexcept {
        return a.close_time_ < b.close_time_;
    }

private:
    Bar() = default;
    [[nodiscard]] static Result<Bar> make(InstrumentId, Timestamp open_time, Timestamp close_time,
                                          Price, Price, Price, Price, Volume,
                                          Duration feed_latency);

    InstrumentId instrument_{kInvalidInstrument};
    Timestamp open_time_{kNoTimestamp};
    Timestamp close_time_{kNoTimestamp};
    EventTime time_{};
    Price open_{};
    Price high_{};
    Price low_{};
    Price close_{};
    Volume volume_{};
};

}  // namespace ptl::market
