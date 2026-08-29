#include <algorithm>
#include <chrono>
#include <cmath>
#include <sstream>

#include "ptl/analytics/performance_analyzer.hpp"

namespace ptl::analytics {
namespace {

[[nodiscard]] Error bad(std::string message, std::string context = {}) {
    return make_error(ErrorCode::InvalidArgument, std::move(message), std::move(context));
}

}  // namespace

std::string_view to_string(SnapshotFrequency f) noexcept {
    return f == SnapshotFrequency::Daily ? "daily" : "monthly";
}

std::string PerformanceSnapshot::describe() const {
    std::ostringstream ss;
    ss.precision(4);
    ss << std::fixed << to_iso8601(ts) << " equity " << equity.get() << " return " << period_return
       << " drawdown " << drawdown;
    return ss.str();
}

std::string PerformanceReport::summary() const {
    std::ostringstream ss;
    ss.precision(4);
    ss << std::fixed;
    ss << "=== " << strategy_name << " ===\n";
    if (!run_id.empty()) ss << "run " << run_id << '\n';
    ss << to_iso8601(period_begin) << " .. " << to_iso8601(period_end) << '\n';
    ss << "equity " << initial_equity.get() << " -> " << final_equity.get() << '\n';
    ss << '\n' << metrics.describe();
    ss << '\n' << risk.describe();
    ss << '\n' << trades.describe();
    ss << '\n' << turnover.describe();
    ss << "\ndrawdown\n";
    ss << "  maximum            " << max_drawdown << '\n';
    ss << "  peak               " << to_iso8601(max_drawdown_peak) << '\n';
    ss << "  trough             " << to_iso8601(max_drawdown_trough) << '\n';
    ss << "  longest underwater " << longest_underwater_periods << " periods\n";
    ss << "\nexposure\n";
    ss << "  peak gross leverage    " << peak_gross_leverage << '\n';
    ss << "  average gross leverage " << average_gross_leverage << '\n';
    for (const auto& [dimension, table] : attribution) {
        ss << '\n' << table.describe();
    }
    if (!caveats.empty()) {
        ss << "\ncaveats\n";
        for (const auto& c : caveats) ss << "  - " << c << '\n';
    }
    return ss.str();
}

std::vector<double> PerformanceAnalyzer::returns_of(
    std::span<const portfolio::EquityPoint> curve) const {
    std::vector<double> out;
    if (curve.size() < 2) return out;
    out.reserve(curve.size() - 1);

    for (std::size_t i = 1; i < curve.size(); ++i) {
        const double prev = curve[i - 1].equity.get();
        const double cur = curve[i].equity.get();
        if (!is_finite(prev) || !is_finite(cur) || prev <= 0.0 || cur <= 0.0) {
            // A non-positive equity makes a return undefined. Zero keeps the
            // series finite; the run is already invalid at that point, and the
            // portfolio's own identity check will have said so.
            out.push_back(0.0);
            continue;
        }
        out.push_back(cfg_.metrics.use_log_returns ? std::log(cur / prev) : (cur / prev - 1.0));
    }
    return out;
}

Result<std::vector<PerformanceSnapshot>> PerformanceAnalyzer::snapshots(
    std::span<const portfolio::EquityPoint> curve, SnapshotFrequency frequency) const {
    std::vector<PerformanceSnapshot> out;
    if (curve.empty()) return out;

    const double initial = curve.front().equity.get();
    DrawdownTracker drawdown;

    // Group by UTC calendar period, consistent with every other timestamp in
    // the system. Grouping by local date would need a tzdb the runtime does not
    // carry (ADR-0001 Addendum A1).
    Timestamp current_key = kNoTimestamp;
    const portfolio::EquityPoint* period_last = nullptr;
    double previous_equity = initial;

    const auto key_of = [frequency](Timestamp ts) {
        const auto day = utc_date_floor(ts);
        if (frequency == SnapshotFrequency::Daily) return day;
        // Month key: the first day of the month containing `ts`.
        const std::chrono::year_month_day ymd{std::chrono::floor<std::chrono::days>(day)};
        const std::chrono::year_month_day first{ymd.year(), ymd.month(), std::chrono::day{1}};
        return Timestamp{
            std::chrono::duration_cast<Duration>(std::chrono::sys_days{first}.time_since_epoch())};
    };

    const auto flush = [&]() {
        if (period_last == nullptr) return;
        PerformanceSnapshot snap;
        snap.ts = period_last->ts;
        snap.equity = period_last->equity;
        snap.cash = period_last->cash;
        snap.realized_pnl = period_last->realized_pnl;
        snap.unrealized_pnl = period_last->unrealized_pnl;
        snap.cumulative_costs = period_last->cumulative_costs;
        snap.turnover = period_last->turnover;

        const double equity = period_last->equity.get();
        snap.period_return = previous_equity > 0.0 ? equity / previous_equity - 1.0 : 0.0;
        snap.cumulative_return = initial > 0.0 ? equity / initial - 1.0 : 0.0;
        snap.drawdown = drawdown.current_drawdown();
        if (!is_finite(snap.period_return)) snap.period_return = 0.0;

        out.push_back(snap);
        previous_equity = equity;
    };

    for (const auto& point : curve) {
        if (auto d = drawdown.update(point.ts, point.equity); !d) return fail(d.error());

        const Timestamp key = key_of(point.ts);
        if (!is_set(current_key)) {
            current_key = key;
        } else if (key != current_key) {
            flush();
            current_key = key;
        }
        period_last = &point;
    }
    flush();
    return out;
}

Result<PerformanceReport> PerformanceAnalyzer::analyze(
    std::span<const portfolio::EquityPoint> curve, std::span<const accounting::Trade> trades,
    std::span<const oms::Fill> fills, const AttributionAnalyzer& attribution,
    std::span<const double> benchmark) const {
    if (curve.empty()) return fail(bad("cannot analyse an empty equity curve"));

    PerformanceReport report;
    report.run_id = cfg_.run_id;
    report.strategy_name = cfg_.strategy_name;
    report.period_begin = curve.front().ts;
    report.period_end = curve.back().ts;
    report.initial_equity = curve.front().equity;
    report.final_equity = curve.back().equity;

    // --- drawdown ------------------------------------------------------------
    DrawdownTracker drawdown;
    for (const auto& point : curve) {
        if (auto d = drawdown.update(point.ts, point.equity); !d) return fail(d.error());
    }
    report.max_drawdown = drawdown.max_drawdown();
    report.max_drawdown_peak = drawdown.max_drawdown_peak();
    report.max_drawdown_trough = drawdown.max_drawdown_trough();
    report.longest_underwater_periods = drawdown.longest_underwater_periods();
    report.underwater_curve.assign(drawdown.underwater_curve().begin(),
                                   drawdown.underwater_curve().end());

    // --- Phase 3 metrics, reused rather than reimplemented --------------------
    const MetricsEngine engine{cfg_.metrics};
    report.metrics = engine.compute(curve, trades);

    // --- risk ----------------------------------------------------------------
    const auto returns = returns_of(curve);
    if (returns.empty()) {
        report.caveats.emplace_back(
            "equity curve has fewer than two points; no return statistics could be "
            "computed");
    } else {
        if (!benchmark.empty() && benchmark.size() != returns.size()) {
            // Refusing beats truncating: a mismatched benchmark would pair each
            // return with the wrong observation and produce a beta that means
            // nothing.
            return fail(bad("benchmark length (" + std::to_string(benchmark.size()) +
                            ") does not match the return series (" +
                            std::to_string(returns.size()) + ")"));
        }
        if (benchmark.empty()) {
            report.caveats.emplace_back(
                "no benchmark supplied: beta, alpha, tracking error, information ratio "
                "and Treynor are not reported rather than being computed against an "
                "implicit zero series");
        }
        const RiskAnalyzer risk{cfg_.risk};
        auto risk_metrics = risk.analyze(returns, benchmark, report.max_drawdown);
        if (!risk_metrics) return fail(risk_metrics.error());
        report.risk = *risk_metrics;
    }

    // --- trades --------------------------------------------------------------
    const TradeAnalyzer trade_analyzer;
    auto trade_stats = trade_analyzer.analyze(trades);
    if (!trade_stats) return fail(trade_stats.error());
    report.trades = *trade_stats;
    if (trades.empty()) {
        report.caveats.emplace_back("no matched round trips: trade statistics are empty");
    }

    // --- turnover ------------------------------------------------------------
    if (curve.size() >= 2) {
        auto turnover = compute_turnover(curve, cfg_.metrics.periods_per_year);
        if (!turnover) return fail(turnover.error());
        report.turnover = *turnover;
    }

    // --- snapshots -----------------------------------------------------------
    auto daily = snapshots(curve, SnapshotFrequency::Daily);
    if (!daily) return fail(daily.error());
    report.daily_snapshots = std::move(*daily);

    auto monthly = snapshots(curve, SnapshotFrequency::Monthly);
    if (!monthly) return fail(monthly.error());
    report.monthly_snapshots = std::move(*monthly);

    // --- exposure ------------------------------------------------------------
    // Derived from the equity curve's own gross and net columns: the analyzer
    // is not handed a live Portfolio, precisely so it cannot touch one.
    double peak_gross = 0.0;
    double sum_gross = 0.0;
    std::size_t counted = 0;
    for (const auto& point : curve) {
        const double equity = point.equity.get();
        if (equity <= 0.0 || !is_finite(equity)) continue;
        const double leverage = point.gross_exposure.get() / equity;
        if (!is_finite(leverage)) continue;
        peak_gross = std::max(peak_gross, leverage);
        sum_gross += leverage;
        ++counted;
    }
    report.peak_gross_leverage = peak_gross;
    report.average_gross_leverage = counted > 0 ? sum_gross / static_cast<double>(counted) : 0.0;

    // --- attribution ---------------------------------------------------------
    // Ordered by dimension name, so two runs produce identical tables.
    if (auto t = attribution.by_instrument(trades)) {
        report.attribution.emplace("instrument", std::move(*t));
    }
    if (auto t = attribution.by_sector(trades)) {
        report.attribution.emplace("sector", std::move(*t));
    }
    if (auto t = attribution.by_strategy(fills)) {
        report.attribution.emplace("strategy", std::move(*t));
    }
    if (auto t = attribution.by_algorithm(fills)) {
        report.attribution.emplace("algorithm", std::move(*t));
    }
    if (auto t = attribution.by_cost_component(fills)) {
        report.attribution.emplace("cost_component", std::move(*t));
    }

    // A table that does not reconcile is reported as a caveat rather than
    // silently presented: attribution that does not sum to the P&L is a
    // plausible-looking table, not attribution.
    const Notional reported_net = report.final_equity - report.initial_equity;
    const auto instrument_table = report.attribution.find("instrument");
    if (instrument_table != report.attribution.end() &&
        !instrument_table->second.reconciles(reported_net, 1.0)) {
        std::ostringstream ss;
        ss.precision(2);
        ss << std::fixed
           << "instrument attribution does not reconcile to the equity change: residual "
           << instrument_table->second.residual(reported_net).get()
           << ". Unrealised P&L on open positions is not attributed to a closed trade.";
        report.caveats.push_back(ss.str());
    }
    return report;
}

}  // namespace ptl::analytics
