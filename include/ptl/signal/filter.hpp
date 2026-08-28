#pragma once

/// \file filter.hpp
/// Pre-trade signal filters.
///
/// Every filter REPORTS its rejections rather than dropping signals silently.
/// A suppressed signal that vanishes without trace makes a backtest diverge
/// from live trading invisibly -- the same reasoning as the risk gate in
/// Phase 3, applied one layer earlier.

#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "ptl/core/result.hpp"
#include "ptl/market/calendar.hpp"
#include "ptl/signal/signal.hpp"

namespace ptl::signal {

enum class FilterReason : std::uint8_t {
    Passed,
    LowConfidence,
    NegativeNetEdge,
    VolatilityOutOfRange,
    InsufficientLiquidity,
    SpreadTooWide,
    OutsideTradingHours,
    CooldownActive,
    StaleFeatures,
    StalePrediction,
    NoMarketData,
};

[[nodiscard]] std::string_view to_string(FilterReason) noexcept;

/// Market context a filter needs. Supplied by the caller from state it already
/// has; a filter never reaches into the portfolio or the event stream itself.
struct FilterContext {
    Timestamp now{kNoTimestamp};
    double realized_volatility = 0.0;
    double interval_volume = 0.0;
    double average_volume = 0.0;
    Bps spread_bps{0.0};
    /// Age of the newest feature that fed the prediction.
    Duration feature_age{Duration::zero()};
    /// Age of the prediction itself.
    Duration prediction_age{Duration::zero()};
    bool has_market_data = false;
};

struct FilterConfig {
    double min_confidence = 0.55;
    bool require_positive_net_edge = true;

    double min_volatility = 0.0;
    double max_volatility = 1.0;

    /// Minimum share of average volume in the interval. Trading a name that is
    /// barely printing means the fill assumptions do not hold.
    double min_relative_volume = 0.1;
    double min_interval_volume = 0.0;

    Bps max_spread_bps{25.0};

    /// Reject inside this window after the open and before the close. The open
    /// and close auctions have different microstructure, and a model trained on
    /// continuous trading does not describe them.
    Duration open_buffer{std::chrono::minutes{5}};
    Duration close_buffer{std::chrono::minutes{5}};

    /// Minimum time between two signals on the same instrument. Prevents a
    /// noisy model from churning a position on consecutive bars.
    Duration cooldown{std::chrono::minutes{15}};

    Duration max_feature_age{std::chrono::minutes{5}};
    Duration max_prediction_age{std::chrono::minutes{15}};
};

struct FilterDecision {
    FilterReason reason{FilterReason::Passed};
    std::string detail;

    [[nodiscard]] bool passed() const noexcept { return reason == FilterReason::Passed; }
    [[nodiscard]] std::string describe() const;
};

/// Applies every filter in a fixed order and counts what it rejected.
///
/// The order is deliberate and stable: cheapest and most fundamental checks
/// first, so a rejection reason names the most basic thing that was wrong
/// rather than whichever check happened to run first.
class SignalFilterChain {
public:
    explicit SignalFilterChain(FilterConfig cfg = {}) : cfg_(cfg) {}

    /// \param calendar may be null; trading-hours filtering is then skipped and
    ///        that fact is recorded rather than silently assumed safe.
    [[nodiscard]] FilterDecision evaluate(const Signal&, const FilterContext&,
                                          const market::Calendar* calendar) const;

    /// Record an outcome. Separate from evaluate() so the decision itself stays
    /// side-effect free and therefore reproducible.
    void record(InstrumentId, const FilterDecision&, Timestamp);

    [[nodiscard]] std::size_t rejection_count() const noexcept;
    [[nodiscard]] std::size_t rejection_count(FilterReason) const noexcept;
    [[nodiscard]] std::size_t pass_count() const noexcept;
    [[nodiscard]] const std::map<std::uint8_t, std::size_t>& counts() const noexcept {
        return counts_;
    }
    [[nodiscard]] std::string summary() const;

    /// Last accepted signal per instrument, for the cooldown check.
    [[nodiscard]] Timestamp last_signal_time(InstrumentId) const noexcept;

    [[nodiscard]] const FilterConfig& config() const noexcept { return cfg_; }
    void reset() noexcept;

private:
    FilterConfig cfg_;
    std::map<std::uint8_t, std::size_t> counts_;
    std::map<std::uint32_t, Timestamp> last_accepted_;
};

}  // namespace ptl::signal
