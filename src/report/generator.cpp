#include "ptl/report/generator.hpp"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>

namespace ptl::report {
namespace {

[[nodiscard]] Error bad(std::string message, std::string context = {}) {
    return make_error(ErrorCode::ValidationFailed, std::move(message), std::move(context));
}

/// Format a double at a FIXED precision.
///
/// Never the default stream precision: that varies with locale and library, so
/// two runs on different machines could produce different bytes for identical
/// values -- exactly what the determinism requirement forbids.
[[nodiscard]] std::string num(double v, int precision) {
    if (!is_finite(v)) {
        // JSON has no NaN or Infinity literal, and emitting one produces a file
        // no parser will accept. Null is the honest representation of "this
        // statistic could not be computed".
        return "null";
    }
    std::ostringstream ss;
    ss << std::fixed << std::setprecision(precision) << v;
    return ss.str();
}

[[nodiscard]] std::string num_csv(double v, int precision) {
    if (!is_finite(v)) return "";
    std::ostringstream ss;
    ss << std::fixed << std::setprecision(precision) << v;
    return ss.str();
}

}  // namespace

std::string_view to_string(Format f) noexcept {
    switch (f) {
        case Format::Csv:
            return "csv";
        case Format::Json:
            return "json";
        case Format::Markdown:
            return "markdown";
    }
    return "unknown";
}

std::string_view extension_of(Format f) noexcept {
    switch (f) {
        case Format::Csv:
            return ".csv";
        case Format::Json:
            return ".json";
        case Format::Markdown:
            return ".md";
    }
    return ".txt";
}

std::string json_escape(std::string_view in) {
    std::string out;
    out.reserve(in.size() + 8);
    for (const char c : in) {
        switch (c) {
            case '"':
                out += "\\\"";
                break;
            case '\\':
                out += "\\\\";
                break;
            case '\n':
                out += "\\n";
                break;
            case '\r':
                out += "\\r";
                break;
            case '\t':
                out += "\\t";
                break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    // Control characters must be escaped or the document is
                    // invalid JSON, and a strategy name is caller-supplied text.
                    std::ostringstream ss;
                    ss << "\\u" << std::hex << std::setw(4) << std::setfill('0')
                       << static_cast<int>(static_cast<unsigned char>(c));
                    out += ss.str();
                } else {
                    out += c;
                }
        }
    }
    return out;
}

std::string csv_escape(std::string_view in) {
    const bool needs_quotes =
        in.find(',') != std::string_view::npos || in.find('"') != std::string_view::npos ||
        in.find('\n') != std::string_view::npos || in.find('\r') != std::string_view::npos;
    if (!needs_quotes) return std::string{in};

    std::string out = "\"";
    for (const char c : in) {
        if (c == '"')
            out += "\"\"";  // RFC 4180 doubling
        else
            out += c;
    }
    out += '"';
    return out;
}

std::string ReportGenerator::to_csv(const analytics::PerformanceReport& report) const {
    std::ostringstream ss;
    ss << "metric,value\n";

    const auto row = [&ss](std::string_view key, const std::string& value) {
        ss << csv_escape(key) << ',' << csv_escape(value) << '\n';
    };
    const int cp = cfg_.currency_precision;
    const int rp = cfg_.ratio_precision;

    row("run_id", report.run_id);
    row("strategy", report.strategy_name);
    row("period_begin", to_iso8601(report.period_begin));
    row("period_end", to_iso8601(report.period_end));
    row("initial_equity", num_csv(report.initial_equity.get(), cp));
    row("final_equity", num_csv(report.final_equity.get(), cp));
    row("cumulative_return", num_csv(report.metrics.cumulative_return, rp));
    row("cagr", num_csv(report.metrics.cagr, rp));
    row("sharpe", num_csv(report.metrics.sharpe, rp));
    row("sortino", num_csv(report.metrics.sortino, rp));
    row("calmar", num_csv(report.metrics.calmar, rp));
    row("max_drawdown", num_csv(report.max_drawdown, rp));
    row("longest_underwater_periods", std::to_string(report.longest_underwater_periods));
    row("beta", num_csv(report.risk.beta, rp));
    row("alpha", num_csv(report.risk.alpha, rp));
    row("information_ratio", num_csv(report.risk.information_ratio, rp));
    row("treynor_ratio", num_csv(report.risk.treynor_ratio, rp));
    row("tracking_error", num_csv(report.risk.tracking_error, rp));
    row("annualized_volatility", num_csv(report.risk.annualized_volatility, rp));
    row("downside_deviation", num_csv(report.risk.downside_deviation, rp));
    row("value_at_risk_95", num_csv(report.risk.value_at_risk_95, rp));
    row("expected_shortfall_95", num_csv(report.risk.expected_shortfall_95, rp));
    row("trades", std::to_string(report.trades.trades));
    row("win_rate", num_csv(report.trades.win_rate, rp));
    row("profit_factor", num_csv(report.trades.profit_factor, rp));
    row("expectancy", num_csv(report.trades.expectancy.get(), cp));
    row("largest_win", num_csv(report.trades.largest_win.get(), cp));
    row("largest_loss", num_csv(report.trades.largest_loss.get(), cp));
    row("max_consecutive_losses", std::to_string(report.trades.max_consecutive_losses));
    row("annualized_turnover", num_csv(report.turnover.annualized_turnover, rp));
    row("total_costs", num_csv(report.metrics.total_costs.get(), cp));
    row("peak_gross_leverage", num_csv(report.peak_gross_leverage, rp));
    row("average_gross_leverage", num_csv(report.average_gross_leverage, rp));
    if (!cfg_.generated_at.empty()) row("generated_at", cfg_.generated_at);
    return ss.str();
}

std::string ReportGenerator::to_json(const analytics::PerformanceReport& report) const {
    std::ostringstream ss;
    const int cp = cfg_.currency_precision;
    const int rp = cfg_.ratio_precision;

    ss << "{\n";
    ss << "  \"run_id\": \"" << json_escape(report.run_id) << "\",\n";
    ss << "  \"strategy\": \"" << json_escape(report.strategy_name) << "\",\n";
    ss << "  \"period_begin\": \"" << to_iso8601(report.period_begin) << "\",\n";
    ss << "  \"period_end\": \"" << to_iso8601(report.period_end) << "\",\n";
    ss << "  \"equity\": {\n";
    ss << "    \"initial\": " << num(report.initial_equity.get(), cp) << ",\n";
    ss << "    \"final\": " << num(report.final_equity.get(), cp) << "\n";
    ss << "  },\n";

    ss << "  \"performance\": {\n";
    ss << "    \"cumulative_return\": " << num(report.metrics.cumulative_return, rp) << ",\n";
    ss << "    \"cagr\": " << num(report.metrics.cagr, rp) << ",\n";
    ss << "    \"sharpe\": " << num(report.metrics.sharpe, rp) << ",\n";
    ss << "    \"sortino\": " << num(report.metrics.sortino, rp) << ",\n";
    ss << "    \"calmar\": " << num(report.metrics.calmar, rp) << ",\n";
    ss << "    \"max_drawdown\": " << num(report.max_drawdown, rp) << ",\n";
    ss << "    \"longest_underwater_periods\": " << report.longest_underwater_periods << "\n";
    ss << "  },\n";

    ss << "  \"risk\": {\n";
    ss << "    \"annualized_volatility\": " << num(report.risk.annualized_volatility, rp) << ",\n";
    ss << "    \"downside_deviation\": " << num(report.risk.downside_deviation, rp) << ",\n";
    ss << "    \"beta\": " << num(report.risk.beta, rp) << ",\n";
    ss << "    \"alpha\": " << num(report.risk.alpha, rp) << ",\n";
    ss << "    \"tracking_error\": " << num(report.risk.tracking_error, rp) << ",\n";
    ss << "    \"information_ratio\": " << num(report.risk.information_ratio, rp) << ",\n";
    ss << "    \"treynor_ratio\": " << num(report.risk.treynor_ratio, rp) << ",\n";
    ss << "    \"r_squared\": " << num(report.risk.r_squared, rp) << ",\n";
    ss << "    \"skewness\": " << num(report.risk.skewness, rp) << ",\n";
    ss << "    \"excess_kurtosis\": " << num(report.risk.excess_kurtosis, rp) << ",\n";
    ss << "    \"value_at_risk_95\": " << num(report.risk.value_at_risk_95, rp) << ",\n";
    ss << "    \"expected_shortfall_95\": " << num(report.risk.expected_shortfall_95, rp) << "\n";
    ss << "  },\n";

    ss << "  \"trades\": {\n";
    ss << "    \"count\": " << report.trades.trades << ",\n";
    ss << "    \"wins\": " << report.trades.wins << ",\n";
    ss << "    \"losses\": " << report.trades.losses << ",\n";
    ss << "    \"win_rate\": " << num(report.trades.win_rate, rp) << ",\n";
    ss << "    \"average_win\": " << num(report.trades.average_win.get(), cp) << ",\n";
    ss << "    \"average_loss\": " << num(report.trades.average_loss.get(), cp) << ",\n";
    ss << "    \"largest_win\": " << num(report.trades.largest_win.get(), cp) << ",\n";
    ss << "    \"largest_loss\": " << num(report.trades.largest_loss.get(), cp) << ",\n";
    ss << "    \"profit_factor\": " << num(report.trades.profit_factor, rp) << ",\n";
    ss << "    \"expectancy\": " << num(report.trades.expectancy.get(), cp) << ",\n";
    ss << "    \"max_consecutive_losses\": " << report.trades.max_consecutive_losses << "\n";
    ss << "  },\n";

    ss << "  \"turnover\": {\n";
    ss << "    \"annualized\": " << num(report.turnover.annualized_turnover, rp) << ",\n";
    ss << "    \"implied_holding_period_days\": "
       << num(report.turnover.implied_holding_period_days, rp) << "\n";
    ss << "  },\n";

    ss << "  \"exposure\": {\n";
    ss << "    \"peak_gross_leverage\": " << num(report.peak_gross_leverage, rp) << ",\n";
    ss << "    \"average_gross_leverage\": " << num(report.average_gross_leverage, rp) << "\n";
    ss << "  }";

    if (cfg_.include_attribution && !report.attribution.empty()) {
        ss << ",\n  \"attribution\": {\n";
        // std::map: dimensions and keys both emit in sorted order, so two runs
        // produce byte-identical JSON.
        bool first_dim = true;
        for (const auto& [dimension, table] : report.attribution) {
            if (!first_dim) ss << ",\n";
            first_dim = false;
            ss << "    \"" << json_escape(dimension) << "\": [\n";
            bool first_entry = true;
            for (const auto& [key, entry] : table.entries) {
                if (!first_entry) ss << ",\n";
                first_entry = false;
                ss << "      {\"key\": \"" << json_escape(key)
                   << "\", \"net_pnl\": " << num(entry.net_pnl.get(), cp)
                   << ", \"gross_pnl\": " << num(entry.gross_pnl.get(), cp)
                   << ", \"costs\": " << num(entry.costs.get(), cp)
                   << ", \"trades\": " << entry.trades
                   << ", \"share\": " << num(entry.contribution_share, rp) << "}";
            }
            ss << "\n    ]";
        }
        ss << "\n  }";
    }

    if (cfg_.include_snapshots && !report.monthly_snapshots.empty()) {
        ss << ",\n  \"monthly\": [\n";
        bool first = true;
        for (const auto& snap : report.monthly_snapshots) {
            if (!first) ss << ",\n";
            first = false;
            ss << "    {\"ts\": \"" << to_iso8601(snap.ts)
               << "\", \"equity\": " << num(snap.equity.get(), cp)
               << ", \"return\": " << num(snap.period_return, rp)
               << ", \"drawdown\": " << num(snap.drawdown, rp) << "}";
        }
        ss << "\n  ]";
    }

    if (!report.caveats.empty()) {
        ss << ",\n  \"caveats\": [\n";
        for (std::size_t i = 0; i < report.caveats.size(); ++i) {
            if (i != 0) ss << ",\n";
            ss << "    \"" << json_escape(report.caveats[i]) << "\"";
        }
        ss << "\n  ]";
    }
    if (!cfg_.generated_at.empty()) {
        ss << ",\n  \"generated_at\": \"" << json_escape(cfg_.generated_at) << "\"";
    }
    ss << "\n}\n";
    return ss.str();
}

std::string ReportGenerator::to_markdown(const analytics::PerformanceReport& report) const {
    std::ostringstream ss;
    const int cp = cfg_.currency_precision;
    const int rp = cfg_.ratio_precision;

    ss << "# " << report.strategy_name << "\n\n";
    if (!report.run_id.empty()) ss << "Run `" << report.run_id << "`\n\n";
    ss << to_iso8601(report.period_begin) << " to " << to_iso8601(report.period_end) << "\n\n";

    ss << "## Headline\n\n";
    ss << "| Metric | Value |\n|---|---|\n";
    ss << "| Initial equity | " << num(report.initial_equity.get(), cp) << " |\n";
    ss << "| Final equity | " << num(report.final_equity.get(), cp) << " |\n";
    ss << "| Cumulative return | " << num(report.metrics.cumulative_return, rp) << " |\n";
    ss << "| CAGR | " << num(report.metrics.cagr, rp) << " |\n";
    ss << "| Sharpe | " << num(report.metrics.sharpe, rp) << " |\n";
    ss << "| Sortino | " << num(report.metrics.sortino, rp) << " |\n";
    ss << "| Calmar | " << num(report.metrics.calmar, rp) << " |\n";
    ss << "| Max drawdown | " << num(report.max_drawdown, rp) << " |\n";
    ss << "| Longest underwater | " << report.longest_underwater_periods << " periods |\n";
    ss << "| Annualised turnover | " << num(report.turnover.annualized_turnover, rp) << " |\n\n";

    ss << "## Risk\n\n";
    ss << "| Metric | Value |\n|---|---|\n";
    ss << "| Annualised volatility | " << num(report.risk.annualized_volatility, rp) << " |\n";
    ss << "| Downside deviation | " << num(report.risk.downside_deviation, rp) << " |\n";
    ss << "| Beta | " << num(report.risk.beta, rp) << " |\n";
    ss << "| Alpha | " << num(report.risk.alpha, rp) << " |\n";
    ss << "| Tracking error | " << num(report.risk.tracking_error, rp) << " |\n";
    ss << "| Information ratio | " << num(report.risk.information_ratio, rp) << " |\n";
    ss << "| Treynor | " << num(report.risk.treynor_ratio, rp) << " |\n";
    ss << "| VaR 95 | " << num(report.risk.value_at_risk_95, rp) << " |\n";
    ss << "| ES 95 | " << num(report.risk.expected_shortfall_95, rp) << " |\n\n";

    ss << "## Trades\n\n";
    ss << "| Metric | Value |\n|---|---|\n";
    ss << "| Trades | " << report.trades.trades << " |\n";
    ss << "| Win rate | " << num(report.trades.win_rate, rp) << " |\n";
    ss << "| Average win | " << num(report.trades.average_win.get(), cp) << " |\n";
    ss << "| Average loss | " << num(report.trades.average_loss.get(), cp) << " |\n";
    ss << "| Largest win | " << num(report.trades.largest_win.get(), cp) << " |\n";
    ss << "| Largest loss | " << num(report.trades.largest_loss.get(), cp) << " |\n";
    ss << "| Profit factor | " << num(report.trades.profit_factor, rp) << " |\n";
    ss << "| Expectancy | " << num(report.trades.expectancy.get(), cp) << " |\n";
    ss << "| Max consecutive losses | " << report.trades.max_consecutive_losses << " |\n\n";

    if (cfg_.include_attribution) {
        for (const auto& [dimension, table] : report.attribution) {
            ss << "## Attribution by " << dimension << "\n\n";
            ss << "| Key | Net P&L | Gross | Costs | Trades | Share |\n";
            ss << "|---|---|---|---|---|---|\n";
            for (const auto& entry : table.ranked()) {
                ss << "| " << entry.key << " | " << num(entry.net_pnl.get(), cp) << " | "
                   << num(entry.gross_pnl.get(), cp) << " | " << num(entry.costs.get(), cp) << " | "
                   << entry.trades << " | " << num(entry.contribution_share, rp) << " |\n";
            }
            ss << '\n';
        }
    }

    if (!report.caveats.empty()) {
        // Caveats are part of the report, not an appendix. A reader who does
        // not know what the report could NOT determine will over-trust it.
        ss << "## Caveats\n\n";
        for (const auto& c : report.caveats) ss << "- " << c << '\n';
        ss << '\n';
    }
    if (!cfg_.generated_at.empty()) {
        ss << "_Generated " << cfg_.generated_at << "_\n";
    }
    return ss.str();
}

Result<std::string> ReportGenerator::generate(const analytics::PerformanceReport& report,
                                              Format format) const {
    switch (format) {
        case Format::Csv:
            return to_csv(report);
        case Format::Json:
            return to_json(report);
        case Format::Markdown:
            return to_markdown(report);
    }
    return fail(bad("unhandled report format"));
}

Result<std::string> ReportGenerator::snapshots_csv(
    std::span<const analytics::PerformanceSnapshot> snapshots) const {
    std::ostringstream ss;
    ss << "ts,equity,cash,realized_pnl,unrealized_pnl,cumulative_costs,turnover,"
          "period_return,cumulative_return,drawdown\n";
    const int cp = cfg_.currency_precision;
    const int rp = cfg_.ratio_precision;

    for (const auto& s : snapshots) {
        ss << to_iso8601(s.ts) << ',' << num_csv(s.equity.get(), cp) << ','
           << num_csv(s.cash.get(), cp) << ',' << num_csv(s.realized_pnl.get(), cp) << ','
           << num_csv(s.unrealized_pnl.get(), cp) << ',' << num_csv(s.cumulative_costs.get(), cp)
           << ',' << num_csv(s.turnover.get(), cp) << ',' << num_csv(s.period_return, rp) << ','
           << num_csv(s.cumulative_return, rp) << ',' << num_csv(s.drawdown, rp) << '\n';
    }
    return ss.str();
}

Result<std::string> ReportGenerator::attribution_csv(
    const analytics::AttributionTable& table) const {
    std::ostringstream ss;
    ss << "key,net_pnl,gross_pnl,costs,turnover,trades,fills,share\n";
    const int cp = cfg_.currency_precision;
    const int rp = cfg_.ratio_precision;

    // Ranked, so the most significant lines appear first and the ordering is a
    // pure function of the data.
    for (const auto& entry : table.ranked()) {
        ss << csv_escape(entry.key) << ',' << num_csv(entry.net_pnl.get(), cp) << ','
           << num_csv(entry.gross_pnl.get(), cp) << ',' << num_csv(entry.costs.get(), cp) << ','
           << num_csv(entry.turnover.get(), cp) << ',' << entry.trades << ',' << entry.fills << ','
           << num_csv(entry.contribution_share, rp) << '\n';
    }
    return ss.str();
}

Result<std::string> ReportGenerator::write(const analytics::PerformanceReport& report,
                                           Format format, const std::string& directory) const {
    auto content = generate(report, format);
    if (!content) return fail(content.error());

    std::error_code ec;
    std::filesystem::create_directories(directory, ec);

    const std::string name = (report.run_id.empty() ? std::string{"report"} : report.run_id) +
                             std::string{extension_of(format)};
    const std::filesystem::path path = std::filesystem::path{directory} / name;

    std::ofstream out{path, std::ios::binary | std::ios::trunc};
    if (!out)
        return fail(make_error(ErrorCode::IoError, "cannot open report for write", path.string()));
    out << *content;
    if (!out) return fail(make_error(ErrorCode::IoError, "report write failed", path.string()));
    return path.string();
}

}  // namespace ptl::report
