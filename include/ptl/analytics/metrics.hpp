#pragma once

/// \file metrics.hpp
/// Performance statistics computed from the equity curve and the trade list.
///
/// Every ratio here has a documented convention, because the same name means
/// different numbers at different desks. Annualisation, the Sortino threshold
/// and the return basis are all explicit rather than assumed.
///
/// Reporting only Sharpe is one of the named mistakes in the research. These
/// exist so the report can show turnover, tail behaviour and gross-to-net
/// degradation beside it.

#include <cstddef>
#include <span>
#include <string>
#include <vector>

#include "ptl/accounting/journal.hpp"
#include "ptl/core/types.hpp"
#include "ptl/portfolio/portfolio.hpp"

namespace ptl::analytics {

struct MetricsConfig {
    /// Periods per year for the observation frequency of the equity curve.
    /// 252 for daily; 252 * 390 for one-minute bars. Wrong here and every
    /// annualised figure is wrong by a constant factor.
    double periods_per_year = 252.0;
    /// Annualised risk-free rate, subtracted before the Sharpe ratio.
    double risk_free_rate = 0.0;
    /// Minimum acceptable return for Sortino, annualised.
    double sortino_threshold = 0.0;
    /// Log returns compound additively, which is what makes the annualisation
    /// arithmetic exact. Simple returns are available for reporting.
    bool use_log_returns = true;
};

struct PerformanceMetrics {
    std::size_t periods = 0;
    Notional initial_equity{};
    Notional final_equity{};

    double cumulative_return = 0.0;
    double annualized_return = 0.0;
    double cagr = 0.0;
    double annualized_volatility = 0.0;
    double downside_volatility = 0.0;

    double sharpe = 0.0;
    double sortino = 0.0;
    double calmar = 0.0;

    double max_drawdown = 0.0;
    Timestamp max_drawdown_start{kNoTimestamp};
    Timestamp max_drawdown_trough{kNoTimestamp};
    std::size_t max_drawdown_periods = 0;

    /// Trade-level. Computed over ROUND TRIPS, not fills: an expectancy over
    /// fills counts a partially filled entry as several separate bets.
    std::size_t trades = 0;
    std::size_t wins = 0;
    std::size_t losses = 0;
    double win_rate = 0.0;
    Notional average_win{};
    Notional average_loss{};
    double win_loss_ratio = 0.0;
    double profit_factor = 0.0;
    Notional expectancy{};

    Notional total_costs{};
    Notional total_turnover{};
    double annualized_turnover = 0.0;
    /// Fraction of gross P&L consumed by costs. The single most useful number
    /// for judging whether a signal is investable.
    double cost_to_gross_ratio = 0.0;

    double skewness = 0.0;
    double worst_period = 0.0;
    double best_period = 0.0;
    /// Mean of the worst 5% of periods.
    double expected_shortfall_5pct = 0.0;

    [[nodiscard]] std::string describe() const;
};

class MetricsEngine {
public:
    explicit MetricsEngine(MetricsConfig cfg = {}) : cfg_(cfg) {}

    [[nodiscard]] PerformanceMetrics compute(std::span<const portfolio::EquityPoint> curve,
                                             std::span<const accounting::Trade> trades) const;

    /// Period returns from an equity curve, per the configured basis.
    [[nodiscard]] std::vector<double> returns(std::span<const portfolio::EquityPoint> curve) const;

    /// Peak-to-trough, as a positive fraction.
    [[nodiscard]] static double max_drawdown(std::span<const double> equity);

    [[nodiscard]] const MetricsConfig& config() const noexcept { return cfg_; }

private:
    MetricsConfig cfg_;
};

}  // namespace ptl::analytics
