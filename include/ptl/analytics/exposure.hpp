#pragma once

/// \file exposure.hpp
/// Exposure decomposition and capacity.
///
/// Gross and net alone do not describe a book. A market-neutral pair and an
/// outright long can share a net exposure of zero and a gross of two, while
/// carrying entirely different risk. Long, short, sector and factor
/// decompositions are what make the difference visible.

#include <cstdint>
#include <map>
#include <span>
#include <string>
#include <vector>

#include "ptl/core/result.hpp"
#include "ptl/core/types.hpp"
#include "ptl/portfolio/portfolio.hpp"

namespace ptl::analytics {

/// A book's exposure at one instant, as fractions of equity.
struct ExposureSnapshot {
    Timestamp ts{kNoTimestamp};
    Notional equity{};
    Notional long_exposure{};
    Notional short_exposure{};
    Notional gross_exposure{};
    Notional net_exposure{};

    double gross_leverage = 0.0;
    double net_leverage = 0.0;
    /// Largest single-instrument weight, by absolute value.
    double max_concentration = 0.0;
    std::size_t long_positions = 0;
    std::size_t short_positions = 0;

    /// Signed notional per sector, ordered by sector id.
    std::map<std::int32_t, Notional> sector_exposure;
    /// Signed exposure per named factor.
    std::map<std::string, double, std::less<>> factor_exposure;

    [[nodiscard]] std::string describe() const;
};

struct ExposureConfig {
    /// Sector id per instrument index; absent means ungrouped.
    std::map<std::uint32_t, std::int32_t> sectors;
    /// Factor loadings per instrument: factor name to loading.
    std::map<std::uint32_t, std::map<std::string, double, std::less<>>> factor_loadings;
};

/// Computes exposure snapshots from a portfolio.
///
/// READ-ONLY. It takes a const Portfolio& and returns a value; there is no path
/// by which computing an exposure could alter a position.
class ExposureTracker {
public:
    explicit ExposureTracker(ExposureConfig cfg = {}) : cfg_(std::move(cfg)) {}

    [[nodiscard]] ExposureSnapshot snapshot(const portfolio::Portfolio&, Timestamp) const;

    /// Record a snapshot in the tracker's history.
    [[nodiscard]] Result<bool> record(const ExposureSnapshot&);

    [[nodiscard]] std::span<const ExposureSnapshot> history() const noexcept { return history_; }
    /// Peak gross leverage observed.
    [[nodiscard]] double peak_gross_leverage() const noexcept;
    [[nodiscard]] double average_gross_leverage() const noexcept;
    [[nodiscard]] const ExposureConfig& config() const noexcept { return cfg_; }

    void reset() noexcept;

private:
    ExposureConfig cfg_;
    std::vector<ExposureSnapshot> history_;
};

/// Turnover and capacity.
struct TurnoverStatistics {
    Notional total_turnover{};
    Notional average_daily_turnover{};
    double annualized_turnover = 0.0;
    /// Mean holding period implied by turnover: 1 / annualised turnover, in
    /// years. A turnover of 250x implies a mean holding period of about a day.
    double implied_holding_period_days = 0.0;

    [[nodiscard]] std::string describe() const;
};

[[nodiscard]] Result<TurnoverStatistics> compute_turnover(
    std::span<const portfolio::EquityPoint> curve, double periods_per_year);

/// A capacity estimate.
///
/// Deliberately CRUDE and labelled as such. A real capacity study needs impact
/// calibrated against executed volume; this bounds capacity by a participation
/// assumption and nothing more. Reporting it as anything firmer would be a
/// number dressed as a measurement.
struct CapacityEstimate {
    /// Assumed maximum share of daily volume the strategy may take.
    double participation_limit = 0.05;
    /// Median daily dollar volume across the universe.
    Notional median_daily_dollar_volume{};
    /// Implied maximum book size, given the turnover.
    Notional implied_capacity{};
    /// Turnover the estimate assumes.
    double annualized_turnover = 0.0;
    std::string caveat;

    [[nodiscard]] std::string describe() const;
};

[[nodiscard]] Result<CapacityEstimate> estimate_capacity(Notional median_daily_dollar_volume,
                                                         double annualized_turnover,
                                                         double participation_limit = 0.05);

}  // namespace ptl::analytics
