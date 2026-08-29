#pragma once

/// \file performance_analyzer.hpp
/// The analytics orchestrator and its report structure.
///
/// PURE ANALYSIS. Every input is a const reference to history that already
/// exists; nothing here writes to a portfolio, an OMS, or a journal. The
/// signatures say so -- `analyze` takes const spans and returns a value, and
/// there is no non-const handle anywhere in the interface.
///
/// That matters beyond tidiness. Analytics run inside the same process as a
/// live session; a reporting routine that mutated a position would corrupt
/// trading state in a way no backtest would ever reveal.

#include <cstdint>
#include <map>
#include <span>
#include <string>
#include <vector>

#include "ptl/accounting/journal.hpp"
#include "ptl/analytics/attribution.hpp"
#include "ptl/analytics/drawdown.hpp"
#include "ptl/analytics/exposure.hpp"
#include "ptl/analytics/metrics.hpp"
#include "ptl/analytics/risk_analyzer.hpp"
#include "ptl/core/result.hpp"
#include "ptl/portfolio/portfolio.hpp"

namespace ptl::analytics {

/// A point-in-time summary of the book.
struct PerformanceSnapshot {
    Timestamp ts{kNoTimestamp};
    Notional equity{};
    Notional cash{};
    Notional realized_pnl{};
    Notional unrealized_pnl{};
    Notional cumulative_costs{};
    Notional turnover{};

    /// Return over the period this snapshot closes.
    double period_return = 0.0;
    double cumulative_return = 0.0;
    double drawdown = 0.0;

    ExposureSnapshot exposure;

    [[nodiscard]] std::string describe() const;
};

/// Snapshots grouped by calendar period.
enum class SnapshotFrequency : std::uint8_t { Daily, Monthly };

[[nodiscard]] std::string_view to_string(SnapshotFrequency) noexcept;

/// The complete evaluation of one run.
struct PerformanceReport {
    std::string run_id;
    std::string strategy_name;
    Timestamp period_begin{kNoTimestamp};
    Timestamp period_end{kNoTimestamp};

    Notional initial_equity{};
    Notional final_equity{};

    /// Phase 3 metrics, reused rather than reimplemented.
    PerformanceMetrics metrics;
    RiskMetrics risk;
    TradeStatistics trades;
    TurnoverStatistics turnover;

    double max_drawdown = 0.0;
    Timestamp max_drawdown_peak{kNoTimestamp};
    Timestamp max_drawdown_trough{kNoTimestamp};
    std::size_t longest_underwater_periods = 0;

    std::vector<PerformanceSnapshot> daily_snapshots;
    std::vector<PerformanceSnapshot> monthly_snapshots;
    std::vector<UnderwaterPoint> underwater_curve;

    /// Attribution tables, ordered by dimension name for reproducibility.
    std::map<std::string, AttributionTable, std::less<>> attribution;

    double peak_gross_leverage = 0.0;
    double average_gross_leverage = 0.0;

    /// Non-fatal observations: an unattributed line, an absent benchmark, a
    /// series too short for a statistic. Recorded rather than dropped so a
    /// reader knows what the report could NOT determine.
    std::vector<std::string> caveats;

    [[nodiscard]] std::string summary() const;
};

struct AnalyzerConfig {
    RiskConfig risk;
    ExposureConfig exposure;
    MetricsConfig metrics;
    std::string run_id;
    std::string strategy_name = "unnamed";
    /// Window for the rolling drawdown series.
    std::size_t rolling_drawdown_window = 60;
};

/// Builds a PerformanceReport from completed history.
class PerformanceAnalyzer {
public:
    explicit PerformanceAnalyzer(AnalyzerConfig cfg = {}) : cfg_(std::move(cfg)) {}

    /// \param curve     the equity curve, chronological
    /// \param trades    matched round trips
    /// \param fills     executions, for cost attribution
    /// \param benchmark optional benchmark returns, one per curve interval
    [[nodiscard]] Result<PerformanceReport> analyze(std::span<const portfolio::EquityPoint> curve,
                                                    std::span<const accounting::Trade> trades,
                                                    std::span<const oms::Fill> fills,
                                                    const AttributionAnalyzer& attribution,
                                                    std::span<const double> benchmark = {}) const;

    /// Period returns from an equity curve. Log or simple per the config.
    [[nodiscard]] std::vector<double> returns_of(std::span<const portfolio::EquityPoint>) const;

    /// Group an equity curve into calendar-period snapshots.
    ///
    /// Grouped by UTC date, consistent with every other timestamp in the
    /// system. A local-date grouping would need a tzdb the runtime does not
    /// carry (ADR-0001 Addendum A1).
    [[nodiscard]] Result<std::vector<PerformanceSnapshot>> snapshots(
        std::span<const portfolio::EquityPoint>, SnapshotFrequency) const;

    [[nodiscard]] const AnalyzerConfig& config() const noexcept { return cfg_; }

private:
    AnalyzerConfig cfg_;
};

}  // namespace ptl::analytics
