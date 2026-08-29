#pragma once

/// \file statistics.hpp
/// Streaming statistics accumulator.
///
/// Welford throughout, for the reason the feature engine gives: the naive
/// sum-of-squares form cancels catastrophically when the mean is large relative
/// to the spread, and can return a negative variance. Equity values around
/// 10^6 with daily moves around 10^3 are exactly that regime.

#include <cstddef>
#include <span>
#include <string>

#include "ptl/core/types.hpp"

namespace ptl::analytics {

class StatisticsAccumulator {
public:
    /// \param downside_threshold accumulated on the fly so a Sortino needs one
    ///        pass rather than a second traversal.
    explicit StatisticsAccumulator(double downside_threshold = 0.0) noexcept
        : downside_threshold_(downside_threshold), downside_valid_(true) {}

    void update(double x) noexcept;

    [[nodiscard]] std::size_t count() const noexcept { return n_; }
    [[nodiscard]] double mean() const noexcept { return n_ > 0 ? mean_ : 0.0; }
    [[nodiscard]] double variance() const noexcept;
    [[nodiscard]] double stdev() const noexcept;
    [[nodiscard]] double min() const noexcept { return n_ > 0 ? min_ : 0.0; }
    [[nodiscard]] double max() const noexcept { return n_ > 0 ? max_ : 0.0; }
    [[nodiscard]] double sum() const noexcept { return sum_; }

    /// Sample skewness. Requires at least three observations; fewer returns
    /// zero rather than a division by zero.
    [[nodiscard]] double skewness() const noexcept;
    /// Excess kurtosis, i.e. zero for a normal distribution.
    [[nodiscard]] double excess_kurtosis() const noexcept;

    /// Downside deviation about a threshold, annualisation left to the caller.
    /// Divides by the FULL count, not the count of downside observations:
    /// dividing by the smaller count inflates the resulting Sortino.
    [[nodiscard]] double downside_deviation(double threshold = 0.0) const noexcept;

    void reset() noexcept;

private:
    std::size_t n_ = 0;
    double mean_ = 0.0;
    double m2_ = 0.0;
    double m3_ = 0.0;
    double m4_ = 0.0;
    double sum_ = 0.0;
    double min_ = 0.0;
    double max_ = 0.0;
    /// Downside accumulators, kept alongside so a Sortino needs one pass.
    double downside_sq_ = 0.0;
    double downside_threshold_ = 0.0;
    bool downside_valid_ = false;
};

/// Quantile of an unsorted sample, by nearest rank. Copies its input, so the
/// caller's data is never reordered -- an analytics function that sorted its
/// argument in place would be a mutation in disguise.
[[nodiscard]] double quantile_of(std::span<const double> values, double q);

/// Mean of the worst `q` fraction. The expected shortfall.
[[nodiscard]] double expected_shortfall(std::span<const double> values, double q = 0.05);

}  // namespace ptl::analytics
