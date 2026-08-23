#pragma once

/// \file event.hpp
/// The canonical market event stream.
///
/// std::variant, not a tagged union. The Phase 0 architecture specified a
/// fixed-size POD union on cache-locality grounds; the research reconciliation
/// (row E2) overturned that as premature optimisation by our own rules. At nine
/// instruments and minute bars the event count is a few million a year, the
/// difference is unmeasurable, and a union of non-trivial members carries a
/// real lifetime hazard. Revisit in Phase 13 with a profile, not before.

#include <cstdint>
#include <string_view>
#include <variant>

#include "ptl/core/time.hpp"
#include "ptl/core/types.hpp"
#include "ptl/market/bar.hpp"
#include "ptl/market/corporate_action.hpp"
#include "ptl/market/quote.hpp"
#include "ptl/market/trade.hpp"

namespace ptl::market {

/// Session transitions, emitted by the replay source so a strategy can act on
/// them without consulting a calendar itself.
enum class SessionEventKind : std::uint8_t { Open, Close };

struct SessionEvent {
    EventTime time{};
    SessionEventKind kind{SessionEventKind::Open};
};

/// A scheduled wake-up, used by execution algorithms to slice a parent order.
struct TimerEvent {
    EventTime time{};
    std::uint64_t id{0};
};

using MarketEvent = std::variant<Bar, Quote, Trade, CorporateAction, SessionEvent, TimerEvent>;

/// The instant the venue produced the event. This is the ordering key of the
/// entire simulation.
[[nodiscard]] Timestamp exchange_time_of(const MarketEvent& e) noexcept;

/// The earliest instant a live system could have known about the event.
/// Strategy code must never consume an event before this.
[[nodiscard]] Timestamp receive_time_of(const MarketEvent& e) noexcept;

/// kInvalidInstrument for session and timer events, which are not per-symbol.
[[nodiscard]] InstrumentId instrument_of(const MarketEvent& e) noexcept;

[[nodiscard]] std::string_view kind_name(const MarketEvent& e) noexcept;

}  // namespace ptl::market
