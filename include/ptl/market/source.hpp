#pragma once

/// \file source.hpp
/// The event-stream seam shared by replay and live.
///
/// THE PARITY RULE, made concrete: "backtest and paper trading differ in
/// adapters and clocks, not strategy semantics."
///
/// Everything downstream -- features, models, signals, risk, execution,
/// accounting -- consumes IMarketDataSource and nothing else. A replay reads
/// from a vector; a live session reads from a socket. Neither is visible from
/// the far side of this interface, so there is no place for the two paths to
/// diverge, because there is only one path.

#include <memory>
#include <optional>
#include <vector>

#include "ptl/core/clock.hpp"
#include "ptl/core/result.hpp"
#include "ptl/market/calendar.hpp"
#include "ptl/market/event.hpp"

namespace ptl::market {

class IMarketDataSource {
public:
    IMarketDataSource() = default;
    virtual ~IMarketDataSource() = default;
    IMarketDataSource(const IMarketDataSource&) = delete;
    IMarketDataSource& operator=(const IMarketDataSource&) = delete;

protected:
    IMarketDataSource(IMarketDataSource&&) = default;
    IMarketDataSource& operator=(IMarketDataSource&&) = default;

public:
    /// Next event in strict chronological order, or nullopt when exhausted.
    /// A live source blocks or polls; a replay pops. The caller cannot tell.
    [[nodiscard]] virtual std::optional<MarketEvent> next() = 0;

    /// Exchange time of the next event without consuming it. kMaxTimestamp when
    /// exhausted, so a k-way merge needs no special case for an empty source.
    [[nodiscard]] virtual Timestamp peek_time() const noexcept = 0;

    [[nodiscard]] virtual std::string_view description() const noexcept = 0;
};

/// Deterministic replay over a materialised event vector.
///
/// Three guarantees, all tested:
///
///  - STRICT CHRONOLOGY. Construction fails if the input is unordered. An
///    out-of-order feed means a broken merge or unsorted data, and every
///    point-in-time guarantee downstream is void either way.
///  - THE CLOCK FOLLOWS THE DATA. Each event advances the SimulatedClock to its
///    RECEIVE time, not its exchange time. A strategy asking "what time is it?"
///    is told the earliest instant it could have known about the event it is
///    holding -- never the instant the venue acted.
///  - NO LOOKAHEAD BY CONSTRUCTION. There is no random access, no index, no way
///    to ask for event n+1. The interface only goes forward.
class ReplaySource final : public IMarketDataSource {
public:
    /// \param clock advanced as events are consumed. Borrowed; must outlive.
    [[nodiscard]] static Result<ReplaySource> create(std::vector<MarketEvent> events,
                                                     SimulatedClock* clock);

    ReplaySource(ReplaySource&&) = default;
    ReplaySource& operator=(ReplaySource&&) = default;
    ~ReplaySource() override = default;

    [[nodiscard]] std::optional<MarketEvent> next() override;
    [[nodiscard]] Timestamp peek_time() const noexcept override;
    [[nodiscard]] std::string_view description() const noexcept override { return "replay"; }

    [[nodiscard]] std::size_t size() const noexcept { return events_.size(); }
    [[nodiscard]] std::size_t consumed() const noexcept { return next_; }
    [[nodiscard]] bool exhausted() const noexcept { return next_ >= events_.size(); }

    /// Rewind for a second pass, e.g. a walk-forward fold. The clock resets to
    /// the first event, so a second run is bit-identical to the first.
    void reset();

private:
    ReplaySource() = default;

    std::vector<MarketEvent> events_;
    std::size_t next_ = 0;
    SimulatedClock* clock_ = nullptr;
    Timestamp first_{kNoTimestamp};
};

/// Interleaves session open/close events into a chronological bar stream.
///
/// Built here rather than in the strategy so that replay and live produce the
/// same sequence: a live runner derives session events from the same Calendar,
/// and neither side has to reimplement "was that the last bar of the day?".
[[nodiscard]] Result<std::vector<MarketEvent>> with_session_events(std::vector<MarketEvent> events,
                                                                   const Calendar& calendar);

/// Merge several already-sorted streams into one. Stable on ties, ordered by
/// instrument id, so the merge is a pure function of its inputs and two runs
/// cannot differ.
[[nodiscard]] Result<std::vector<MarketEvent>> merge_sorted(
    std::vector<std::vector<MarketEvent>> streams);

}  // namespace ptl::market
