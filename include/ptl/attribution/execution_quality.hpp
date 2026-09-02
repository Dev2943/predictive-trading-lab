#pragma once

/// \file execution_quality.hpp
/// Per-trade execution quality.
///
/// WHAT THIS ADDS THAT PHASE 10 DOES NOT. `analytics::TradeAnalyzer` reports
/// whether trades WON -- win rate, expectancy, profit factor. This reports
/// whether they were EXECUTED WELL, which is a different and separable
/// question. A strategy can be right about direction and lose money to its own
/// execution, and no win-rate statistic will ever show that.
///
/// The distinction that matters is between the DECISION price and the ARRIVAL
/// price. Everything between them is delay cost, which belongs to the strategy;
/// everything after arrival is execution cost, which belongs to the algorithm.
/// Reporting only the total makes it impossible to know which to fix.

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "ptl/core/result.hpp"
#include "ptl/core/types.hpp"
#include "ptl/oms/fill.hpp"

namespace ptl::attribution {

/// Prices a trade is measured against.
///
/// All four are needed because each isolates a different cost. Supplying only
/// some is normal -- a bar-only backtest has no interval VWAP -- so each is
/// optional and the statistics that need it are simply not reported rather
/// than silently computed from a substitute.
struct ExecutionBenchmarks {
    /// Price when the signal fired.
    std::optional<Price> decision_price{};
    /// Price when the order reached the venue.
    std::optional<Price> arrival_price{};
    /// Volume-weighted average over the execution window.
    std::optional<Price> interval_vwap{};
    /// Time-weighted average over the same window.
    std::optional<Price> interval_twap{};
    /// Volume printed in the market over the window, for participation.
    std::optional<Volume> interval_volume{};
    /// Best and worst prices reached while the position was held, for MFE/MAE.
    std::optional<Price> best_price{};
    std::optional<Price> worst_price{};
};

/// One trade's execution quality.
///
/// Costs are signed so POSITIVE IS ALWAYS A COST, for both sides. A buy filling
/// above its benchmark and a sell filling below both hurt, and one convention
/// spares every aggregate a special case.
struct TradeExecutionQuality {
    InstrumentId instrument{kInvalidInstrument};
    Side side{Side::Buy};
    Qty quantity{};
    Price average_fill_price{};
    Timestamp decision_time{kNoTimestamp};
    Timestamp first_fill_time{kNoTimestamp};
    Timestamp last_fill_time{kNoTimestamp};
    std::size_t fill_count = 0;

    /// Decision to arrival. Belongs to the STRATEGY: it is the cost of the
    /// market moving while the order was being decided and routed.
    std::optional<Bps> delay_cost{};
    /// Arrival to fill. Belongs to the ALGORITHM.
    std::optional<Bps> execution_cost{};
    /// Decision to fill: delay plus execution. The number the strategy
    /// actually experiences.
    std::optional<Bps> implementation_shortfall{};

    std::optional<Bps> vs_vwap{};
    std::optional<Bps> vs_twap{};

    /// Share of market volume this trade took.
    std::optional<double> participation_rate{};
    /// Filled quantity over quantity sought. Below one means the algorithm ran
    /// out of window or liquidity.
    double fill_efficiency = 1.0;

    /// Maximum favourable and adverse excursion while held, in basis points
    /// from the entry. MFE says how much was available; MAE says how much pain
    /// was endured to capture it.
    std::optional<Bps> max_favorable_excursion{};
    std::optional<Bps> max_adverse_excursion{};

    Duration holding_period{Duration::zero()};
    /// Realised edge in basis points, against the decision price.
    std::optional<Bps> realized_edge{};
    /// Edge the signal expected, for comparison.
    std::optional<Bps> expected_edge{};
    /// Realised minus expected. Persistent negativity means the forecast is
    /// optimistic, which is a research finding rather than an execution one.
    std::optional<Bps> edge_surprise{};

    Notional commission{};
    Notional fees{};

    [[nodiscard]] std::string describe() const;
};

/// Aggregate execution quality across trades.
struct ExecutionQualitySummary {
    std::size_t trades = 0;
    std::size_t fills = 0;

    /// Quantity-weighted, not trade-weighted. A hundred-share trade and a
    /// hundred-thousand-share trade do not deserve equal say in an average
    /// execution cost.
    Bps average_implementation_shortfall{0.0};
    Bps average_delay_cost{0.0};
    Bps average_execution_cost{0.0};
    Bps average_vs_vwap{0.0};
    Bps median_implementation_shortfall{0.0};
    Bps worst_implementation_shortfall{0.0};

    double average_participation_rate = 0.0;
    double average_fill_efficiency = 0.0;
    Duration average_holding_period{Duration::zero()};

    Notional total_commission{};
    Notional total_fees{};

    /// Trades whose realised edge fell short of expectation.
    std::size_t trades_below_expected_edge = 0;

    [[nodiscard]] std::string describe() const;
};

/// Signal decay: how expected edge erodes with delay.
///
/// The practical question this answers is how long an execution window may be
/// before the alpha it was chasing has gone. A strategy with a five-minute
/// half-life cannot be worked over an hour, however good the algorithm.
struct SignalDecayProfile {
    /// Bucket boundaries, in ascending delay order.
    std::vector<Duration> horizons;
    /// Mean realised edge for trades whose delay fell in each bucket.
    std::vector<double> mean_edge_bps;
    std::vector<std::size_t> sample_counts;

    /// Delay at which mean edge falls below half its first-bucket value.
    /// Unset when the profile never decays that far within the observed range,
    /// which is itself worth knowing.
    std::optional<Duration> half_life{};

    [[nodiscard]] std::string describe() const;
};

class ExecutionQualityAnalyzer {
public:
    /// Analyse one parent order's fills.
    ///
    /// \param fills all fills belonging to one trading decision, in any order.
    [[nodiscard]] Result<TradeExecutionQuality> analyze_trade(std::span<const oms::Fill> fills,
                                                              const ExecutionBenchmarks&,
                                                              Qty quantity_sought) const;

    [[nodiscard]] Result<ExecutionQualitySummary> summarize(
        std::span<const TradeExecutionQuality>) const;

    /// Bucket trades by decision-to-arrival delay and measure edge in each.
    [[nodiscard]] Result<SignalDecayProfile> signal_decay(std::span<const TradeExecutionQuality>,
                                                          std::span<const Duration> horizons) const;

    /// Cost in basis points of `fill` against `benchmark`, signed so positive
    /// is a cost for either side. Exposed because the sign convention is easy
    /// to invert and deserves a direct test.
    [[nodiscard]] static Bps cost_bps(Price fill, Price benchmark, Side) noexcept;
};

}  // namespace ptl::attribution
