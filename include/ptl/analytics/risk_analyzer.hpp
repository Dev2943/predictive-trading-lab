#pragma once

/// \file risk_analyzer.hpp
/// Benchmark-relative risk metrics.
///
/// Everything here needs a BENCHMARK. Beta, alpha, information ratio, Treynor
/// and tracking error are all statements about a strategy relative to something
/// else, and computing them against an implicit zero benchmark -- as a
/// surprising amount of software does -- silently turns them into different
/// statistics with the same names.
///
/// The benchmark series is therefore a required argument everywhere, and a
/// length mismatch is an error rather than a silent truncation.

#include <cstddef>
#include <span>
#include <string>

#include "ptl/analytics/statistics.hpp"
#include "ptl/core/result.hpp"
#include "ptl/core/types.hpp"

namespace ptl::analytics {

struct RiskConfig {
    /// Periods per year for the return frequency. 252 daily; 252*390 minute.
    double periods_per_year = 252.0;
    /// Annualised risk-free rate.
    double risk_free_rate = 0.0;
    /// Minimum acceptable return for Sortino, annualised.
    double sortino_threshold = 0.0;
};

/// Risk statistics relative to a benchmark.
struct RiskMetrics {
    std::size_t periods = 0;

    double annualized_return = 0.0;
    double annualized_volatility = 0.0;
    double downside_deviation = 0.0;

    double sharpe = 0.0;
    double sortino = 0.0;
    double calmar = 0.0;

    /// Sensitivity to the benchmark: cov(r, b) / var(b).
    double beta = 0.0;
    /// Annualised excess return after removing the beta-explained part. This is
    /// the number a benchmark-relative strategy is actually judged on.
    double alpha = 0.0;
    /// Annualised standard deviation of the return difference.
    double tracking_error = 0.0;
    /// Active return over tracking error. Distinct from Sharpe, which measures
    /// against cash rather than against the benchmark.
    double information_ratio = 0.0;
    /// Excess return per unit of BETA rather than per unit of volatility.
    /// Undefined at zero beta, where it is reported as zero.
    double treynor_ratio = 0.0;
    /// Fraction of variance the benchmark explains.
    double r_squared = 0.0;

    double correlation_to_benchmark = 0.0;
    double skewness = 0.0;
    double excess_kurtosis = 0.0;
    double value_at_risk_95 = 0.0;
    double expected_shortfall_95 = 0.0;
    double best_period = 0.0;
    double worst_period = 0.0;

    [[nodiscard]] std::string describe() const;
};

class RiskAnalyzer {
public:
    explicit RiskAnalyzer(RiskConfig cfg = {}) : cfg_(cfg) {}

    /// \param returns   strategy period returns
    /// \param benchmark benchmark period returns, same length. Empty means no
    ///        benchmark, in which case beta, alpha, tracking error, information
    ///        ratio and Treynor are left at zero rather than computed against
    ///        an implicit zero series.
    [[nodiscard]] Result<RiskMetrics> analyze(std::span<const double> returns,
                                              std::span<const double> benchmark = {},
                                              double max_drawdown = 0.0) const;

    /// Beta of `returns` on `benchmark`. Exposed because the convention is easy
    /// to invert and deserves a direct test.
    [[nodiscard]] static double beta_of(std::span<const double> returns,
                                        std::span<const double> benchmark);

    [[nodiscard]] static double tracking_error_of(std::span<const double> returns,
                                                  std::span<const double> benchmark,
                                                  double periods_per_year);

    [[nodiscard]] const RiskConfig& config() const noexcept { return cfg_; }

private:
    RiskConfig cfg_;
};

}  // namespace ptl::analytics
