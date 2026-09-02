#pragma once

/// \file rolling.hpp
/// Rolling risk analytics and visualization datasets.
///
/// WHAT THIS ADDS THAT PHASE 10 DOES NOT. `RiskAnalyzer` produces ONE number
/// per statistic over a whole sample. That answers "was this strategy good?"
/// but not "when did it stop working?" -- and the second question is the one
/// that gets asked when a live book starts losing money.
///
/// A Sharpe of 1.2 over three years is consistent with a strategy that earned
/// 2.0 for two years and -0.5 for the last one. Only a rolling series shows
/// that, and by then the point-in-time number is actively misleading.
///
/// COMPLEXITY. Rolling volatility and rolling beta use INCREMENTAL updates:
/// each step adds one observation and removes one, so a full series is O(n)
/// rather than the O(n*w) of recomputing each window. At institutional
/// horizons -- minute bars over years -- that is the difference between a
/// report that runs and one that does not. Rolling quantiles (VaR) genuinely
/// need order statistics and are O(n log w) via a sorted window.

#include <cstddef>
#include <deque>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "ptl/analytics/statistics.hpp"
#include "ptl/core/result.hpp"
#include "ptl/core/types.hpp"

namespace ptl::analytics {

/// A time-aligned series of one statistic.
///
/// Values before the window is full are ABSENT rather than computed from a
/// partial window. A "rolling 60-day Sharpe" computed from four observations
/// is not a rolling 60-day Sharpe, and plotting it as one produces a chart
/// whose opening spike is pure artefact.
struct RollingSeries {
    std::string name;
    std::size_t window = 0;
    std::vector<Timestamp> timestamps;
    /// Parallel to `timestamps`; nullopt where the window was not yet full.
    std::vector<std::optional<double>> values;

    [[nodiscard]] std::size_t size() const noexcept { return timestamps.size(); }
    [[nodiscard]] std::size_t observed() const noexcept;
    [[nodiscard]] std::optional<double> latest() const noexcept;
    [[nodiscard]] std::string describe() const;
};

struct RollingConfig {
    std::size_t window = 60;
    double periods_per_year = 252.0;
    double risk_free_rate = 0.0;
    /// Confidence for VaR and CVaR, as a tail probability.
    double var_confidence = 0.05;
};

/// Computes rolling statistics over aligned return series.
///
/// PURE. Every method takes const spans and returns a value; the inputs are
/// never reordered or modified. An analytics function that sorted its argument
/// in place would be a mutation in disguise, and the caller's series is
/// usually the equity curve itself.
class RollingAnalyzer {
public:
    explicit RollingAnalyzer(RollingConfig cfg = {}) : cfg_(cfg) {}

    [[nodiscard]] Result<RollingSeries> volatility(std::span<const Timestamp>,
                                                   std::span<const double> returns) const;
    [[nodiscard]] Result<RollingSeries> sharpe(std::span<const Timestamp>,
                                               std::span<const double> returns) const;
    /// Rolling beta and alpha against a benchmark of equal length.
    [[nodiscard]] Result<RollingSeries> beta(std::span<const Timestamp>,
                                             std::span<const double> returns,
                                             std::span<const double> benchmark) const;
    [[nodiscard]] Result<RollingSeries> alpha(std::span<const Timestamp>,
                                              std::span<const double> returns,
                                              std::span<const double> benchmark) const;
    /// Historical VaR: the loss quantile within the window, reported POSITIVE
    /// for a loss, which is the convention a risk report expects.
    [[nodiscard]] Result<RollingSeries> value_at_risk(std::span<const Timestamp>,
                                                      std::span<const double> returns) const;
    /// Conditional VaR, the mean of the tail beyond VaR.
    [[nodiscard]] Result<RollingSeries> conditional_var(std::span<const Timestamp>,
                                                        std::span<const double> returns) const;

    [[nodiscard]] const RollingConfig& config() const noexcept { return cfg_; }

    /// Omega ratio: probability-weighted gains over losses about a threshold.
    ///
    /// Absent from Phase 10, and worth having because it uses the WHOLE return
    /// distribution rather than its first two moments. For a strategy with
    /// meaningful skew -- an option seller, say -- Sharpe and Omega can rank
    /// two books in opposite orders, and Sharpe is the one that is wrong.
    [[nodiscard]] static double omega_ratio(std::span<const double> returns,
                                            double threshold = 0.0) noexcept;

    /// Drawdown durations in TIME, not observation counts.
    ///
    /// Phase 10's DrawdownTracker counts periods underwater; this converts to
    /// wall-clock duration, which is what a drawdown is actually judged by.
    [[nodiscard]] static Result<std::vector<Duration>> drawdown_durations(
        std::span<const Timestamp>, std::span<const double> equity);

private:
    RollingConfig cfg_;
};

}  // namespace ptl::analytics
