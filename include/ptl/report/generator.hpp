#pragma once

/// \file generator.hpp
/// Deterministic report generation.
///
/// A REPORT IS A PURE FUNCTION OF A REPORT STRUCTURE. Same input, same bytes --
/// which is what lets a regression test diff two runs' reports directly, and
/// what keeps report generation from ever influencing replay.
///
/// Three properties make that true:
///
///  1. NO WALL CLOCK in the output. A generated-at timestamp would make every
///     report differ from every other, so it is supplied by the caller when it
///     is wanted at all.
///  2. ORDERED CONTAINERS everywhere. The report structure uses std::map, so
///     iteration order is the key order rather than an insertion or hash order.
///  3. FIXED PRECISION. Doubles are written with an explicit precision, so a
///     value that round-trips differently between libraries cannot change the
///     bytes.

#include <cstdint>
#include <span>
#include <string>
#include <string_view>

#include "ptl/analytics/performance_analyzer.hpp"
#include "ptl/core/result.hpp"

namespace ptl::report {

enum class Format : std::uint8_t { Csv, Json, Markdown };

[[nodiscard]] std::string_view to_string(Format) noexcept;
[[nodiscard]] std::string_view extension_of(Format) noexcept;

struct ReportConfig {
    /// Decimal places for currency-like values.
    int currency_precision = 2;
    /// Significant figures for ratios and returns.
    int ratio_precision = 6;
    /// Include the per-period snapshot tables. Off for a summary-only report.
    bool include_snapshots = true;
    bool include_underwater_curve = false;
    bool include_attribution = true;
    /// Optional generation stamp. EMPTY BY DEFAULT: a wall-clock stamp would
    /// make two reports of the same run differ, which is exactly what the
    /// determinism test forbids.
    std::string generated_at;
};

class ReportGenerator {
public:
    explicit ReportGenerator(ReportConfig cfg = {}) : cfg_(std::move(cfg)) {}

    [[nodiscard]] Result<std::string> generate(const analytics::PerformanceReport&, Format) const;

    /// The equity curve and snapshots as CSV, for a spreadsheet or a plot.
    [[nodiscard]] Result<std::string> snapshots_csv(
        std::span<const analytics::PerformanceSnapshot>) const;

    /// One attribution table as CSV.
    [[nodiscard]] Result<std::string> attribution_csv(const analytics::AttributionTable&) const;

    /// Write a report to disk. The path is returned so a caller can record it
    /// in the run manifest.
    [[nodiscard]] Result<std::string> write(const analytics::PerformanceReport&, Format,
                                            const std::string& directory) const;

    [[nodiscard]] const ReportConfig& config() const noexcept { return cfg_; }

private:
    [[nodiscard]] std::string to_csv(const analytics::PerformanceReport&) const;
    [[nodiscard]] std::string to_json(const analytics::PerformanceReport&) const;
    [[nodiscard]] std::string to_markdown(const analytics::PerformanceReport&) const;

    ReportConfig cfg_;
};

/// Escape a string for JSON. Exposed because a report containing a quote or a
/// backslash in a strategy name must not produce invalid JSON, and that
/// deserves its own test.
[[nodiscard]] std::string json_escape(std::string_view);

/// Quote a CSV field when it contains a comma, quote or newline.
[[nodiscard]] std::string csv_escape(std::string_view);

}  // namespace ptl::report
