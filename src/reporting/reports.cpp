#include "ptl/reporting/reports.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <iomanip>
#include <sstream>

#include "ptl/report/generator.hpp"

namespace ptl::reporting {
namespace {

/// Fixed precision, never the stream default: the default varies with locale
/// and library, so two runs on different machines could produce different bytes
/// for identical values.
[[nodiscard]] std::string num(double v, int precision) {
    // JSON has no NaN or Infinity literal, and emitting one produces a document
    // no parser will accept. Null is the honest representation of "this could
    // not be computed".
    if (!is_finite(v)) return "null";
    std::ostringstream ss;
    ss << std::fixed << std::setprecision(precision) << v;
    return ss.str();
}

void write_series(std::ostringstream& ss, const DataSeries& series, int precision, int indent) {
    const std::string pad(static_cast<std::size_t>(indent), ' ');
    ss << pad << "{\n";
    ss << pad << "  \"name\": \"" << report::json_escape(series.name) << "\",\n";
    ss << pad << "  \"points\": [\n";
    for (std::size_t i = 0; i < series.size(); ++i) {
        if (i != 0) ss << ",\n";
        ss << pad << "    {\"t\": \"" << to_iso8601(series.timestamps[i]) << "\", \"v\": "
           << (series.values[i].has_value() ? num(*series.values[i], precision) : "null") << '}';
    }
    ss << '\n' << pad << "  ]\n";
    ss << pad << '}';
}

}  // namespace

std::string_view to_string(ReportKind k) noexcept {
    switch (k) {
        case ReportKind::Daily:
            return "daily";
        case ReportKind::Weekly:
            return "weekly";
        case ReportKind::Monthly:
            return "monthly";
        case ReportKind::Execution:
            return "execution";
        case ReportKind::Optimization:
            return "optimization";
        case ReportKind::Research:
            return "research";
        case ReportKind::Risk:
            return "risk";
    }
    return "unknown";
}

std::size_t VisualizationData::series_count() const noexcept {
    return 9 + factor_history.size() + sector_allocation.size() + country_allocation.size();
}

std::string Report::describe() const {
    std::ostringstream ss;
    ss.precision(6);
    ss << std::fixed << to_string(kind) << " report " << report_id << ' '
       << to_iso8601(period_begin) << " .. " << to_iso8601(period_end) << '\n';
    for (const auto& [key, value] : metrics) ss << "  " << key << ": " << value << '\n';
    for (const auto& [key, value] : labels) ss << "  " << key << ": " << value << '\n';
    if (!caveats.empty()) {
        ss << "  caveats:\n";
        for (const auto& c : caveats) ss << "    - " << c << '\n';
    }
    return ss.str();
}

DataSeries ReportBuilder::downsample(const DataSeries& series, std::size_t limit) {
    if (limit == 0 || series.size() <= limit) return series;

    DataSeries out;
    out.name = series.name;
    const std::size_t stride = (series.size() + limit - 1) / limit;
    // STRIDE, not averaging. An averaged equity curve smooths away the
    // drawdown troughs, which are precisely the points a reader is looking for.
    for (std::size_t i = 0; i < series.size(); i += stride) {
        out.timestamps.push_back(series.timestamps[i]);
        out.values.push_back(series.values[i]);
    }
    // The final point is always kept, so the series ends where the data does.
    if (!series.timestamps.empty() && out.timestamps.back() != series.timestamps.back()) {
        out.timestamps.push_back(series.timestamps.back());
        out.values.push_back(series.values.back());
    }
    return out;
}

Result<DataGrid> ReportBuilder::monthly_return_grid(
    std::span<const analytics::PerformanceSnapshot> snapshots) {
    DataGrid grid;
    grid.name = "monthly_returns";
    if (snapshots.empty()) return grid;

    // Keyed by (year, month) so the grid is built in calendar order regardless
    // of the input ordering.
    std::map<int, std::map<unsigned, double>> by_year;
    for (const auto& snap : snapshots) {
        if (!is_set(snap.ts)) continue;
        const auto day = std::chrono::floor<std::chrono::days>(utc_date_floor(snap.ts));
        const std::chrono::year_month_day ymd{day};
        by_year[static_cast<int>(ymd.year())][static_cast<unsigned>(ymd.month())] =
            snap.period_return;
    }

    for (const auto& [year, months] : by_year) grid.row_labels.push_back(std::to_string(year));
    for (unsigned m = 1; m <= 12; ++m) {
        grid.column_labels.push_back(m < 10 ? "0" + std::to_string(m) : std::to_string(m));
    }

    grid.cells.assign(grid.row_labels.size() * 12, std::nullopt);
    std::size_t row = 0;
    for (const auto& [year, months] : by_year) {
        for (const auto& [month, value] : months) {
            if (month >= 1 && month <= 12) {
                grid.cells[row * 12 + (month - 1)] = value;
            }
        }
        ++row;
    }
    return grid;
}

Result<VisualizationData> ReportBuilder::build_visualization(
    const analytics::PerformanceReport& performance,
    std::span<const analytics::RollingSeries> rolling) const {
    VisualizationData viz;

    // --- equity and exposure, from the snapshots -----------------------------
    viz.equity_curve.name = "equity_curve";
    viz.rolling_drawdown.name = "drawdown";
    viz.turnover.name = "turnover";
    for (const auto& snap : performance.daily_snapshots) {
        viz.equity_curve.timestamps.push_back(snap.ts);
        viz.equity_curve.values.push_back(snap.equity.get());
        viz.rolling_drawdown.timestamps.push_back(snap.ts);
        viz.rolling_drawdown.values.push_back(snap.drawdown);
        viz.turnover.timestamps.push_back(snap.ts);
        viz.turnover.values.push_back(snap.turnover.get());
    }

    viz.gross_exposure.name = "gross_exposure";
    viz.net_exposure.name = "net_exposure";
    for (const auto& snap : performance.daily_snapshots) {
        viz.gross_exposure.timestamps.push_back(snap.ts);
        viz.gross_exposure.values.push_back(snap.exposure.gross_leverage);
        viz.net_exposure.timestamps.push_back(snap.ts);
        viz.net_exposure.values.push_back(snap.exposure.net_leverage);
    }

    // --- rolling series, matched by name -------------------------------------
    // Matched rather than positional: a caller supplying them in a different
    // order would otherwise mislabel every chart, and the mistake would be
    // invisible until someone read a Sharpe axis showing volatility.
    for (const auto& series : rolling) {
        DataSeries converted;
        converted.name = series.name;
        converted.timestamps = series.timestamps;
        converted.values = series.values;

        if (series.name == "rolling_sharpe")
            viz.rolling_sharpe = std::move(converted);
        else if (series.name == "rolling_volatility")
            viz.rolling_volatility = std::move(converted);
        else if (series.name == "rolling_beta")
            viz.rolling_beta = std::move(converted);
        else if (series.name == "rolling_var")
            viz.rolling_var = std::move(converted);
        else
            viz.factor_history.push_back(std::move(converted));
    }

    auto grid = monthly_return_grid(performance.monthly_snapshots);
    if (!grid) return fail(grid.error());
    viz.monthly_returns = std::move(*grid);

    if (cfg_.max_series_points > 0) {
        for (DataSeries* series : {&viz.equity_curve, &viz.rolling_sharpe, &viz.rolling_volatility,
                                   &viz.rolling_beta, &viz.rolling_drawdown, &viz.rolling_var,
                                   &viz.gross_exposure, &viz.net_exposure, &viz.turnover}) {
            *series = downsample(*series, cfg_.max_series_points);
        }
        for (auto& series : viz.factor_history) {
            series = downsample(series, cfg_.max_series_points);
        }
    }
    return viz;
}

Result<Report> ReportBuilder::build(ReportKind kind,
                                    const analytics::PerformanceReport& performance,
                                    const attribution::PnlDecomposition* pnl,
                                    const attribution::ExecutionQualitySummary* execution,
                                    const attribution::FactorContribution* factors) const {
    Report report;
    report.kind = kind;
    report.report_id = performance.run_id.empty()
                           ? std::string{to_string(kind)}
                           : performance.run_id + "_" + std::string{to_string(kind)};
    report.period_begin = performance.period_begin;
    report.period_end = performance.period_end;

    report.labels.emplace("strategy", performance.strategy_name);
    report.labels.emplace("kind", std::string{to_string(kind)});

    // Metrics common to every kind. A daily report and a risk report disagree
    // about what matters, but both need to say how much money there is.
    report.metrics.emplace("initial_equity", performance.initial_equity.get());
    report.metrics.emplace("final_equity", performance.final_equity.get());
    report.metrics.emplace("max_drawdown", performance.max_drawdown);

    switch (kind) {
        case ReportKind::Daily:
        case ReportKind::Weekly:
        case ReportKind::Monthly:
            report.metrics.emplace("cumulative_return", performance.metrics.cumulative_return);
            report.metrics.emplace("sharpe", performance.metrics.sharpe);
            report.metrics.emplace("sortino", performance.metrics.sortino);
            report.metrics.emplace("turnover", performance.turnover.annualized_turnover);
            break;

        case ReportKind::Risk:
            report.metrics.emplace("annualized_volatility", performance.risk.annualized_volatility);
            report.metrics.emplace("downside_deviation", performance.risk.downside_deviation);
            report.metrics.emplace("value_at_risk_95", performance.risk.value_at_risk_95);
            report.metrics.emplace("expected_shortfall_95", performance.risk.expected_shortfall_95);
            report.metrics.emplace("beta", performance.risk.beta);
            report.metrics.emplace("tracking_error", performance.risk.tracking_error);
            report.metrics.emplace("skewness", performance.risk.skewness);
            report.metrics.emplace("excess_kurtosis", performance.risk.excess_kurtosis);
            report.metrics.emplace("peak_gross_leverage", performance.peak_gross_leverage);
            report.metrics.emplace("longest_underwater_periods",
                                   static_cast<double>(performance.longest_underwater_periods));
            break;

        case ReportKind::Execution:
            report.metrics.emplace("total_costs", performance.metrics.total_costs.get());
            if (execution != nullptr) {
                report.metrics.emplace("implementation_shortfall_bps",
                                       execution->average_implementation_shortfall.get());
                report.metrics.emplace("delay_cost_bps", execution->average_delay_cost.get());
                report.metrics.emplace("execution_cost_bps",
                                       execution->average_execution_cost.get());
                report.metrics.emplace("participation_rate", execution->average_participation_rate);
                report.metrics.emplace("fill_efficiency", execution->average_fill_efficiency);
            }
            break;

        case ReportKind::Optimization:
            report.metrics.emplace("annualized_turnover", performance.turnover.annualized_turnover);
            report.metrics.emplace("average_gross_leverage", performance.average_gross_leverage);
            break;

        case ReportKind::Research:
            report.metrics.emplace("cagr", performance.metrics.cagr);
            report.metrics.emplace("sharpe", performance.metrics.sharpe);
            report.metrics.emplace("trades", static_cast<double>(performance.trades.trades));
            report.metrics.emplace("win_rate", performance.trades.win_rate);
            report.metrics.emplace("profit_factor", performance.trades.profit_factor);
            report.metrics.emplace("expectancy", performance.trades.expectancy.get());
            break;
    }

    // Attribution tables carry across whole; they are already ordered by key.
    for (const auto& [name, table] : performance.attribution) {
        report.tables.emplace(name, table);
    }

    if (pnl != nullptr) report.pnl = *pnl;
    if (execution != nullptr) report.execution = *execution;
    if (factors != nullptr) report.factors = *factors;

    // Caveats propagate from the underlying analysis. A report that dropped
    // them would present the same numbers with more apparent authority than the
    // analysis that produced them.
    report.caveats = performance.caveats;
    if (kind == ReportKind::Execution && execution == nullptr) {
        report.caveats.emplace_back(
            "no execution quality summary supplied; implementation shortfall and "
            "participation are not reported rather than being estimated");
    }
    if (kind == ReportKind::Risk && factors == nullptr) {
        report.caveats.emplace_back(
            "no factor decomposition supplied; alpha and beta contributions are absent");
    }
    return report;
}

Result<std::string> ReportBuilder::to_json(const Report& report) const {
    std::ostringstream ss;
    const int mp = cfg_.metric_precision;
    const int cp = cfg_.currency_precision;

    ss << "{\n";
    ss << "  \"report_id\": \"" << report::json_escape(report.report_id) << "\",\n";
    ss << "  \"kind\": \"" << to_string(report.kind) << "\",\n";
    ss << "  \"period_begin\": \"" << to_iso8601(report.period_begin) << "\",\n";
    ss << "  \"period_end\": \"" << to_iso8601(report.period_end) << "\",\n";

    ss << "  \"labels\": {";
    bool first = true;
    for (const auto& [key, value] : report.labels) {
        if (!first) ss << ',';
        first = false;
        ss << "\n    \"" << report::json_escape(key) << "\": \"" << report::json_escape(value)
           << '"';
    }
    ss << (report.labels.empty() ? "}" : "\n  }") << ",\n";

    // std::map: keys emit in sorted order, so two runs produce byte-identical
    // JSON and a regression test can diff them directly.
    ss << "  \"metrics\": {";
    first = true;
    for (const auto& [key, value] : report.metrics) {
        if (!first) ss << ',';
        first = false;
        ss << "\n    \"" << report::json_escape(key) << "\": " << num(value, mp);
    }
    ss << (report.metrics.empty() ? "}" : "\n  }");

    if (report.pnl.has_value()) {
        const auto& p = *report.pnl;
        ss << ",\n  \"pnl\": {\n";
        ss << "    \"realized\": " << num(p.realized.get(), cp) << ",\n";
        ss << "    \"unrealized\": " << num(p.unrealized.get(), cp) << ",\n";
        ss << "    \"commission\": " << num(p.commission.get(), cp) << ",\n";
        ss << "    \"exchange_fees\": " << num(p.exchange_fees.get(), cp) << ",\n";
        ss << "    \"slippage\": " << num(p.slippage.get(), cp) << ",\n";
        ss << "    \"borrow\": " << num(p.borrow.get(), cp) << ",\n";
        ss << "    \"carry\": " << num(p.carry.get(), cp) << ",\n";
        ss << "    \"cash_drag\": " << num(p.cash_drag.get(), cp) << ",\n";
        ss << "    \"gross\": " << num(p.gross_pnl().get(), cp) << ",\n";
        ss << "    \"net\": " << num(p.net_pnl().get(), cp) << "\n  }";
    }

    if (report.factors.has_value()) {
        const auto& f = *report.factors;
        ss << ",\n  \"factors\": {\n";
        ss << "    \"beta\": " << num(f.beta, mp) << ",\n";
        ss << "    \"beta_contribution\": " << num(f.beta_contribution, mp) << ",\n";
        ss << "    \"alpha_contribution\": " << num(f.alpha_contribution, mp) << ",\n";
        ss << "    \"residual_contribution\": " << num(f.residual_contribution, mp) << "\n  }";
    }

    if (report.execution.has_value()) {
        const auto& e = *report.execution;
        ss << ",\n  \"execution\": {\n";
        ss << "    \"trades\": " << e.trades << ",\n";
        ss << "    \"fills\": " << e.fills << ",\n";
        ss << "    \"implementation_shortfall_bps\": "
           << num(e.average_implementation_shortfall.get(), mp) << ",\n";
        ss << "    \"delay_cost_bps\": " << num(e.average_delay_cost.get(), mp) << ",\n";
        ss << "    \"execution_cost_bps\": " << num(e.average_execution_cost.get(), mp) << ",\n";
        ss << "    \"participation_rate\": " << num(e.average_participation_rate, mp) << ",\n";
        ss << "    \"fill_efficiency\": " << num(e.average_fill_efficiency, mp) << "\n  }";
    }

    if (!report.tables.empty()) {
        ss << ",\n  \"attribution\": {";
        bool first_table = true;
        for (const auto& [name, table] : report.tables) {
            if (!first_table) ss << ',';
            first_table = false;
            ss << "\n    \"" << report::json_escape(name) << "\": [";
            bool first_entry = true;
            for (const auto& [key, entry] : table.entries) {
                if (!first_entry) ss << ',';
                first_entry = false;
                ss << "\n      {\"key\": \"" << report::json_escape(key)
                   << "\", \"net_pnl\": " << num(entry.net_pnl.get(), cp)
                   << ", \"gross_pnl\": " << num(entry.gross_pnl.get(), cp)
                   << ", \"costs\": " << num(entry.costs.get(), cp)
                   << ", \"share\": " << num(entry.contribution_share, mp) << '}';
            }
            ss << (table.entries.empty() ? "]" : "\n    ]");
        }
        ss << "\n  }";
    }

    if (cfg_.include_visualization && report.visualization.has_value()) {
        const auto& v = *report.visualization;
        ss << ",\n  \"visualization\": {\n    \"series\": [\n";
        const DataSeries* all[] = {&v.equity_curve,   &v.rolling_sharpe,   &v.rolling_volatility,
                                   &v.rolling_beta,   &v.rolling_drawdown, &v.rolling_var,
                                   &v.gross_exposure, &v.net_exposure,     &v.turnover};
        bool first_series = true;
        for (const auto* series : all) {
            if (series->size() == 0) continue;
            if (!first_series) ss << ",\n";
            first_series = false;
            write_series(ss, *series, mp, 6);
        }
        for (const auto& series : v.factor_history) {
            if (series.size() == 0) continue;
            if (!first_series) ss << ",\n";
            first_series = false;
            write_series(ss, series, mp, 6);
        }
        ss << "\n    ],\n";

        ss << "    \"monthly_returns\": {\n";
        ss << "      \"rows\": [";
        for (std::size_t i = 0; i < v.monthly_returns.row_labels.size(); ++i) {
            if (i != 0) ss << ", ";
            ss << '"' << report::json_escape(v.monthly_returns.row_labels[i]) << '"';
        }
        ss << "],\n      \"columns\": [";
        for (std::size_t i = 0; i < v.monthly_returns.column_labels.size(); ++i) {
            if (i != 0) ss << ", ";
            ss << '"' << report::json_escape(v.monthly_returns.column_labels[i]) << '"';
        }
        ss << "],\n      \"cells\": [";
        for (std::size_t i = 0; i < v.monthly_returns.cells.size(); ++i) {
            if (i != 0) ss << ", ";
            ss << (v.monthly_returns.cells[i].has_value() ? num(*v.monthly_returns.cells[i], mp)
                                                          : "null");
        }
        ss << "]\n    }\n  }";
    }

    if (!report.caveats.empty()) {
        ss << ",\n  \"caveats\": [";
        for (std::size_t i = 0; i < report.caveats.size(); ++i) {
            if (i != 0) ss << ',';
            ss << "\n    \"" << report::json_escape(report.caveats[i]) << '"';
        }
        ss << "\n  ]";
    }
    ss << "\n}\n";
    return ss.str();
}

}  // namespace ptl::reporting
