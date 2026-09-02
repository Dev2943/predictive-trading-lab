#pragma once

/// \file reports.hpp
/// Typed institutional reports and visualization datasets, serialized to JSON.
///
/// WHY THIS IS NOT `ptl::report` (Phase 10). That module renders ONE structure
/// -- `analytics::PerformanceReport` -- into CSV, JSON or Markdown. It is a
/// SERIALIZER. This module defines a FAMILY of report types, each answering a
/// different operational question on a different cadence, plus the datasets a
/// chart needs. Those are different responsibilities, and folding the second
/// into the first would make `ReportGenerator` grow a switch over report kinds
/// that every future report would have to edit.
///
/// The two are composed rather than duplicated: this module reuses
/// `report::json_escape` rather than writing a second escaper, because two
/// escaping implementations is exactly how one of them ends up subtly wrong.
///
/// DETERMINISM. Same input, same bytes -- the Phase 10 property, preserved.
/// No wall clock, ordered containers, fixed precision. A report that embedded
/// the time it was generated could never be diffed against another run.

#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "ptl/analytics/performance_analyzer.hpp"
#include "ptl/analytics/rolling.hpp"
#include "ptl/attribution/execution_quality.hpp"
#include "ptl/attribution/pnl.hpp"
#include "ptl/core/result.hpp"

namespace ptl::reporting {

enum class ReportKind : std::uint8_t {
    Daily,
    Weekly,
    Monthly,
    Execution,
    Optimization,
    Research,
    Risk,
};

[[nodiscard]] std::string_view to_string(ReportKind) noexcept;

/// One labelled numeric series, ready to plot.
///
/// Deliberately data only: no colours, no axes, no chart type. A library that
/// decided how its numbers should look would be unusable by any front end that
/// disagreed, and the brief for this layer is data, not plotting.
struct DataSeries {
    std::string name;
    std::vector<Timestamp> timestamps;
    std::vector<std::optional<double>> values;

    [[nodiscard]] bool valid() const noexcept { return timestamps.size() == values.size(); }
    [[nodiscard]] std::size_t size() const noexcept { return timestamps.size(); }
};

/// A two-dimensional grid, for a heatmap.
struct DataGrid {
    std::string name;
    std::vector<std::string> row_labels;
    std::vector<std::string> column_labels;
    /// Row-major, `row_labels.size() * column_labels.size()` entries.
    std::vector<std::optional<double>> cells;

    [[nodiscard]] bool valid() const noexcept {
        return cells.size() == row_labels.size() * column_labels.size();
    }
};

/// Everything a dashboard needs, and nothing about how to draw it.
struct VisualizationData {
    DataSeries equity_curve;
    DataSeries rolling_sharpe;
    DataSeries rolling_volatility;
    DataSeries rolling_beta;
    DataSeries rolling_drawdown;
    DataSeries rolling_var;
    DataSeries gross_exposure;
    DataSeries net_exposure;
    DataSeries turnover;

    /// Monthly returns by year and month. The classic performance heatmap.
    DataGrid monthly_returns;
    /// Factor or sector allocation over time.
    std::vector<DataSeries> factor_history;
    std::vector<DataSeries> sector_allocation;
    std::vector<DataSeries> country_allocation;

    [[nodiscard]] std::size_t series_count() const noexcept;
};

/// A report. One structure for every kind, because the CONTENTS differ but the
/// shape does not: a period, a set of metrics, a set of tables, some caveats.
/// Seven near-identical structs would be seven places to add a field.
struct Report {
    ReportKind kind{ReportKind::Daily};
    std::string report_id;
    Timestamp period_begin{kNoTimestamp};
    Timestamp period_end{kNoTimestamp};

    /// Scalar metrics, ordered by key so two runs emit identical JSON.
    std::map<std::string, double, std::less<>> metrics;
    /// String-valued fields: strategy name, run id, regime label.
    std::map<std::string, std::string, std::less<>> labels;
    /// Tabular sections, ordered by name.
    std::map<std::string, analytics::AttributionTable, std::less<>> tables;

    std::optional<attribution::PnlDecomposition> pnl;
    std::optional<attribution::FactorContribution> factors;
    std::optional<attribution::ExecutionQualitySummary> execution;
    std::optional<VisualizationData> visualization;

    /// What the report could NOT determine. First-class, because a reader who
    /// does not know the gaps will over-trust the numbers.
    std::vector<std::string> caveats;

    [[nodiscard]] std::string describe() const;
};

struct ReportConfig {
    int metric_precision = 8;
    int currency_precision = 2;
    bool include_visualization = true;
    /// Cap on points per series. A five-year minute-bar equity curve is half a
    /// million points; no chart renders that, and shipping it makes the JSON
    /// unusable. Zero disables downsampling.
    std::size_t max_series_points = 2000;
};

/// Builds reports from completed analytics. Read-only throughout.
class ReportBuilder {
public:
    explicit ReportBuilder(ReportConfig cfg = {}) : cfg_(cfg) {}

    /// Assemble a report of the requested kind from a performance report and
    /// optional attribution.
    [[nodiscard]] Result<Report> build(ReportKind, const analytics::PerformanceReport&,
                                       const attribution::PnlDecomposition* = nullptr,
                                       const attribution::ExecutionQualitySummary* = nullptr,
                                       const attribution::FactorContribution* = nullptr) const;

    /// Build the visualization datasets from a performance report and rolling
    /// series already computed by the caller.
    [[nodiscard]] Result<VisualizationData> build_visualization(
        const analytics::PerformanceReport&,
        std::span<const analytics::RollingSeries> rolling) const;

    /// Monthly return heatmap from a snapshot series.
    [[nodiscard]] static Result<DataGrid> monthly_return_grid(
        std::span<const analytics::PerformanceSnapshot>);

    /// Reduce a series to at most `limit` points by uniform stride.
    ///
    /// Stride rather than averaging: an averaged equity curve hides the
    /// drawdown troughs, which are the points a reader is looking for.
    [[nodiscard]] static DataSeries downsample(const DataSeries&, std::size_t limit);

    /// Serialize to JSON. The only output format: the brief is data for other
    /// systems, and CSV or Markdown would be Phase 10's job anyway.
    [[nodiscard]] Result<std::string> to_json(const Report&) const;

    [[nodiscard]] const ReportConfig& config() const noexcept { return cfg_; }

private:
    ReportConfig cfg_;
};

}  // namespace ptl::reporting
