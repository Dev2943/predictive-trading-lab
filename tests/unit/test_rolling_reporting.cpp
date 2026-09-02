#include <algorithm>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cmath>
#include <limits>
#include <numeric>
#include <vector>

#include "ptl/analytics/rolling.hpp"
#include "ptl/reporting/reports.hpp"
#include "support/ptl_catch.hpp"

using namespace ptl;
using namespace ptl::analytics;
using namespace std::chrono;

namespace {

Timestamp at(const char* iso) {
    Timestamp ts{};
    REQUIRE(parse_timestamp(iso, ts));
    return ts;
}

std::vector<Timestamp> daily(std::size_t n) {
    std::vector<Timestamp> out;
    Timestamp t = at("2024-01-02T20:00:00Z");
    for (std::size_t i = 0; i < n; ++i) {
        out.push_back(t);
        t += hours{24};
    }
    return out;
}

}  // namespace

// ---------------------------------------------------------------------------
// Rolling analytics
// ---------------------------------------------------------------------------

TEST_CASE("a rolling window is absent until it is full", "[analytics][rolling][leakage]") {
    // A "rolling 60-day Sharpe" from four observations is not one, and plotting
    // it as such produces an opening spike that is pure artefact.
    RollingConfig cfg;
    cfg.window = 10;
    const RollingAnalyzer analyzer{cfg};

    std::vector<double> returns(30, 0.0);
    for (std::size_t i = 0; i < returns.size(); ++i) {
        returns[i] = std::sin(static_cast<double>(i) * 0.4) * 0.01;
    }

    auto vol = analyzer.volatility(daily(30), returns);
    REQUIRE(vol.has_value());
    REQUIRE(vol->size() == 30);
    // The first nine have no value at all.
    for (std::size_t i = 0; i + 1 < cfg.window; ++i) {
        REQUIRE_FALSE(vol->values[i].has_value());
    }
    REQUIRE(vol->values[cfg.window - 1].has_value());
    REQUIRE(vol->observed() == 30 - cfg.window + 1);
}

TEST_CASE("incremental rolling volatility matches a direct recomputation",
          "[analytics][rolling][numerical]") {
    // The whole point of the incremental form is that it is O(n) rather than
    // O(n*w). It is only worth having if it agrees with the naive version.
    RollingConfig cfg;
    cfg.window = 20;
    cfg.periods_per_year = 252.0;
    const RollingAnalyzer analyzer{cfg};

    std::vector<double> returns(100, 0.0);
    for (std::size_t i = 0; i < returns.size(); ++i) {
        returns[i] = std::sin(static_cast<double>(i) * 0.23) * 0.02 + 0.0003;
    }

    auto rolling = analyzer.volatility(daily(100), returns);
    REQUIRE(rolling.has_value());

    // Direct recomputation of the final window.
    StatisticsAccumulator direct;
    for (std::size_t i = 100 - cfg.window; i < 100; ++i) direct.update(returns[i]);
    const double expected = direct.stdev() * std::sqrt(252.0);

    REQUIRE(rolling->values.back().has_value());
    REQUIRE(*rolling->values.back() == Catch::Approx(expected).epsilon(1e-9));
}

TEST_CASE("rolling beta recovers a known relationship", "[analytics][rolling][numerical]") {
    RollingConfig cfg;
    cfg.window = 30;
    const RollingAnalyzer analyzer{cfg};

    std::vector<double> benchmark(80, 0.0);
    std::vector<double> portfolio(80, 0.0);
    for (std::size_t i = 0; i < 80; ++i) {
        benchmark[i] = std::sin(static_cast<double>(i) * 0.31) * 0.01;
        portfolio[i] = 1.5 * benchmark[i];
    }

    auto beta = analyzer.beta(daily(80), portfolio, benchmark);
    REQUIRE(beta.has_value());
    REQUIRE(beta->latest().has_value());
    REQUIRE(*beta->latest() == Catch::Approx(1.5).epsilon(1e-6));

    auto alpha = analyzer.alpha(daily(80), portfolio, benchmark);
    REQUIRE(alpha.has_value());
    // Exactly 1.5x the benchmark means no alpha.
    REQUIRE(*alpha->latest() == Catch::Approx(0.0).margin(1e-9));
}

TEST_CASE("a zero-variance benchmark window yields no beta", "[analytics][rolling][edge]") {
    // Undefined, not infinite.
    RollingConfig cfg;
    cfg.window = 10;
    const RollingAnalyzer analyzer{cfg};

    const std::vector<double> flat(30, 0.0);
    std::vector<double> portfolio(30, 0.0);
    for (std::size_t i = 0; i < 30; ++i) portfolio[i] = 0.001 * static_cast<double>(i % 3);

    auto beta = analyzer.beta(daily(30), portfolio, flat);
    REQUIRE(beta.has_value());
    REQUIRE_FALSE(beta->latest().has_value());
}

TEST_CASE("rolling VaR and CVaR report losses positive", "[analytics][rolling][property]") {
    RollingConfig cfg;
    cfg.window = 20;
    cfg.var_confidence = 0.10;
    const RollingAnalyzer analyzer{cfg};

    std::vector<double> returns(60, 0.001);
    // A handful of sharp losses.
    returns[10] = -0.05;
    returns[25] = -0.08;
    returns[40] = -0.03;

    auto var = analyzer.value_at_risk(daily(60), returns);
    auto cvar = analyzer.conditional_var(daily(60), returns);
    REQUIRE(var.has_value());
    REQUIRE(cvar.has_value());

    // A loss is reported as a positive number, which is what a risk report
    // expects.
    REQUIRE(var->values[25].has_value());
    REQUIRE(*var->values[25] > 0.0);
    // CVaR is the mean of the tail, so it is at least as severe as VaR.
    REQUIRE(*cvar->values[25] >= *var->values[25]);

    // The QUANTILE CONVENTION, pinned explicitly. Twenty observations at 10%
    // must select the second-worst return, not the third: nearest rank is
    // ceil(q*n) - 1, and the floor form understates risk.
    RollingConfig pinned;
    pinned.window = 20;
    pinned.var_confidence = 0.10;
    std::vector<double> known(20, 0.01);
    known[3] = -0.09;   // worst
    known[11] = -0.04;  // second worst
    known[17] = -0.02;  // third worst
    auto exact = RollingAnalyzer{pinned}.value_at_risk(daily(20), known);
    REQUIRE(exact.has_value());
    REQUIRE(exact->values.back().has_value());
    REQUIRE(*exact->values.back() == Catch::Approx(0.04));
}

TEST_CASE("Omega uses the whole distribution", "[analytics][rolling][property]") {
    // For a skewed strategy, Sharpe and Omega can rank two books in opposite
    // orders, and Sharpe is the one that is wrong.
    const std::vector<double> symmetric{0.01, -0.01, 0.01, -0.01};
    REQUIRE(RollingAnalyzer::omega_ratio(symmetric, 0.0) == Catch::Approx(1.0));

    const std::vector<double> favourable{0.03, -0.01, 0.03, -0.01};
    REQUIRE(RollingAnalyzer::omega_ratio(favourable, 0.0) == Catch::Approx(3.0));

    // No losses at all means Omega is unbounded, which is a property of the
    // sample. Zero is the sentinel; an infinity would propagate everywhere.
    const std::vector<double> all_gains{0.01, 0.02, 0.03};
    REQUIRE(RollingAnalyzer::omega_ratio(all_gains, 0.0) == 0.0);
    REQUIRE(RollingAnalyzer::omega_ratio({}, 0.0) == 0.0);
}

TEST_CASE("drawdown durations measure time, not observation counts", "[analytics][rolling]") {
    const auto ts = daily(6);
    // Peak at index 0, trough, then recovery at index 3.
    const std::vector<double> equity{100.0, 90.0, 80.0, 105.0, 100.0, 110.0};

    auto durations = RollingAnalyzer::drawdown_durations(ts, equity);
    REQUIRE(durations.has_value());
    REQUIRE(durations->size() == 2);
    // Three days from peak to recovery.
    REQUIRE(durations->front() == hours{72});

    // An UNRECOVERED drawdown is deliberately not appended: its duration is
    // still running, and recording it as ended understates the episode a
    // reader most needs to see.
    const std::vector<double> still_falling{100.0, 90.0, 80.0};
    auto open_episode = RollingAnalyzer::drawdown_durations(daily(3), still_falling);
    REQUIRE(open_episode.has_value());
    REQUIRE(open_episode->empty());
}

TEST_CASE("rolling analytics refuse malformed input", "[analytics][rolling][validation]") {
    RollingConfig cfg;
    cfg.window = 10;
    const RollingAnalyzer analyzer{cfg};

    REQUIRE_FALSE(analyzer.volatility({}, {}).has_value());
    // Mismatched lengths.
    REQUIRE_FALSE(analyzer.volatility(daily(5), std::vector<double>(3, 0.0)).has_value());

    // Non-monotonic timestamps.
    std::vector<Timestamp> backwards = daily(20);
    std::swap(backwards[5], backwards[6]);
    REQUIRE_FALSE(analyzer.volatility(backwards, std::vector<double>(20, 0.01)).has_value());

    // A non-finite return.
    std::vector<double> poisoned(20, 0.01);
    poisoned[3] = std::numeric_limits<double>::quiet_NaN();
    REQUIRE_FALSE(analyzer.volatility(daily(20), poisoned).has_value());

    RollingConfig tiny;
    tiny.window = 1;
    REQUIRE_FALSE(
        RollingAnalyzer{tiny}.volatility(daily(20), std::vector<double>(20, 0.01)).has_value());
}

TEST_CASE("rolling analytics do not mutate their inputs", "[analytics][rolling][leakage]") {
    // VaR sorts internally; it must not reorder the caller's series.
    RollingConfig cfg;
    cfg.window = 5;
    const RollingAnalyzer analyzer{cfg};

    std::vector<double> returns{0.03, -0.01, 0.02, -0.04, 0.01, 0.05, -0.02};
    const std::vector<double> before = returns;

    REQUIRE(analyzer.value_at_risk(daily(7), returns).has_value());
    REQUIRE(analyzer.conditional_var(daily(7), returns).has_value());
    REQUIRE(returns == before);
}

TEST_CASE("rolling analytics are deterministic", "[analytics][rolling][determinism]") {
    RollingConfig cfg;
    cfg.window = 15;
    const RollingAnalyzer analyzer{cfg};

    std::vector<double> returns(50, 0.0);
    for (std::size_t i = 0; i < 50; ++i) {
        returns[i] = std::sin(static_cast<double>(i) * 0.37) * 0.015;
    }

    auto a = analyzer.sharpe(daily(50), returns);
    auto b = analyzer.sharpe(daily(50), returns);
    REQUIRE(a.has_value());
    REQUIRE(b.has_value());
    for (std::size_t i = 0; i < a->values.size(); ++i) {
        REQUIRE(a->values[i].has_value() == b->values[i].has_value());
        // EXACT equality: float summation is not associative, so any ordering
        // difference would surface here.
        if (a->values[i].has_value()) REQUIRE(*a->values[i] == *b->values[i]);
    }
}

// ---------------------------------------------------------------------------
// Reporting
// ---------------------------------------------------------------------------

namespace {

analytics::PerformanceReport fixture_report() {
    analytics::PerformanceReport report;
    report.run_id = "run42";
    report.strategy_name = "fixture";
    report.period_begin = at("2024-01-02T20:00:00Z");
    report.period_end = at("2024-03-01T20:00:00Z");
    report.initial_equity = Notional{100'000.0};
    report.final_equity = Notional{108'000.0};
    report.max_drawdown = 0.045;
    report.metrics.sharpe = 1.42;
    report.metrics.cumulative_return = 0.08;
    report.risk.annualized_volatility = 0.11;
    report.risk.beta = 0.85;
    report.turnover.annualized_turnover = 12.5;

    Timestamp t = at("2024-01-02T20:00:00Z");
    double equity = 100'000.0;
    for (int i = 0; i < 40; ++i) {
        analytics::PerformanceSnapshot snap;
        snap.ts = t;
        equity *= 1.002;
        snap.equity = Notional{equity};
        snap.period_return = 0.002;
        snap.drawdown = i % 7 == 0 ? 0.01 : 0.0;
        snap.turnover = Notional{static_cast<double>(i) * 1000.0};
        snap.exposure.gross_leverage = 0.9;
        snap.exposure.net_leverage = 0.4;
        report.daily_snapshots.push_back(snap);
        report.monthly_snapshots.push_back(snap);
        t += hours{24};
    }
    return report;
}

}  // namespace

TEST_CASE("a report is built for every kind", "[reporting]") {
    const reporting::ReportBuilder builder;
    const auto performance = fixture_report();

    for (const auto kind : {reporting::ReportKind::Daily, reporting::ReportKind::Weekly,
                            reporting::ReportKind::Monthly, reporting::ReportKind::Execution,
                            reporting::ReportKind::Optimization, reporting::ReportKind::Research,
                            reporting::ReportKind::Risk}) {
        INFO("kind: " << reporting::to_string(kind));
        auto report = builder.build(kind, performance);
        REQUIRE(report.has_value());
        REQUIRE(report->kind == kind);
        REQUIRE_FALSE(report->metrics.empty());
        // Every kind reports the equity it is describing.
        REQUIRE(report->metrics.contains("final_equity"));

        auto json = builder.to_json(*report);
        REQUIRE(json.has_value());
        REQUIRE_FALSE(json->empty());
    }
}

TEST_CASE("a risk report without factors says so", "[reporting][edge]") {
    // A reader who does not know the gaps will over-trust the numbers.
    const reporting::ReportBuilder builder;
    auto report = builder.build(reporting::ReportKind::Risk, fixture_report());
    REQUIRE(report.has_value());

    bool mentions = false;
    for (const auto& c : report->caveats) {
        if (c.find("factor decomposition") != std::string::npos) mentions = true;
    }
    REQUIRE(mentions);
}

TEST_CASE("report JSON is well formed and deterministic", "[reporting][determinism]") {
    const reporting::ReportBuilder builder;
    auto report = builder.build(reporting::ReportKind::Daily, fixture_report());
    REQUIRE(report.has_value());

    auto first = builder.to_json(*report);
    auto second = builder.to_json(*report);
    REQUIRE(first.has_value());
    // Same input, same bytes. A report embedding its generation time could
    // never be diffed against another run.
    REQUIRE(*first == *second);
    REQUIRE(first->find("generated_at") == std::string::npos);

    // Balanced braces and brackets is a cheap structural check.
    int braces = 0;
    int brackets = 0;
    bool in_string = false;
    bool escaped = false;
    for (const char c : *first) {
        if (escaped) {
            escaped = false;
            continue;
        }
        if (c == '\\') {
            escaped = true;
            continue;
        }
        if (c == '"') {
            in_string = !in_string;
            continue;
        }
        if (in_string) continue;
        if (c == '{') ++braces;
        if (c == '}') --braces;
        if (c == '[') ++brackets;
        if (c == ']') --brackets;
        REQUIRE(braces >= 0);
        REQUIRE(brackets >= 0);
    }
    REQUIRE(braces == 0);
    REQUIRE(brackets == 0);
}

TEST_CASE("non-finite metrics become JSON null", "[reporting][edge]") {
    // JSON has no NaN literal, and emitting one produces a document no parser
    // accepts.
    const reporting::ReportBuilder builder;
    auto report = builder.build(reporting::ReportKind::Daily, fixture_report());
    report->metrics["broken"] = std::numeric_limits<double>::quiet_NaN();
    report->metrics["also_broken"] = std::numeric_limits<double>::infinity();

    auto json = builder.to_json(*report);
    REQUIRE(json.has_value());
    REQUIRE(json->find("\"broken\": null") != std::string::npos);
    REQUIRE(json->find("\"also_broken\": null") != std::string::npos);
}

TEST_CASE("visualization datasets are built and downsampled", "[reporting][visualization]") {
    reporting::ReportConfig cfg;
    cfg.max_series_points = 10;
    const reporting::ReportBuilder builder{cfg};
    const auto performance = fixture_report();

    RollingConfig rolling_cfg;
    rolling_cfg.window = 5;
    const RollingAnalyzer analyzer{rolling_cfg};
    std::vector<double> returns(40, 0.002);
    auto sharpe = analyzer.sharpe(daily(40), returns);
    REQUIRE(sharpe.has_value());

    const std::vector<RollingSeries> rolling{*sharpe};
    auto viz = builder.build_visualization(performance, rolling);
    REQUIRE(viz.has_value());

    REQUIRE(viz->equity_curve.valid());
    // Downsampled to the cap.
    REQUIRE(viz->equity_curve.size() <= cfg.max_series_points + 1);
    // Matched BY NAME, not position: a caller supplying series in a different
    // order would otherwise mislabel every chart.
    REQUIRE(viz->rolling_sharpe.name == "rolling_sharpe");
    REQUIRE(viz->rolling_sharpe.size() > 0);
    REQUIRE(viz->monthly_returns.valid());
    REQUIRE(viz->monthly_returns.column_labels.size() == 12);
}

TEST_CASE("downsampling keeps the endpoints", "[reporting][visualization]") {
    // Stride rather than averaging: an averaged equity curve smooths away the
    // drawdown troughs, which are the points a reader is looking for.
    reporting::DataSeries series;
    series.name = "equity";
    Timestamp t = at("2024-01-02T20:00:00Z");
    for (int i = 0; i < 1000; ++i) {
        series.timestamps.push_back(t);
        series.values.push_back(static_cast<double>(i));
        t += hours{1};
    }

    const auto reduced = reporting::ReportBuilder::downsample(series, 50);
    REQUIRE(reduced.size() <= 51);
    REQUIRE(reduced.timestamps.front() == series.timestamps.front());
    REQUIRE(reduced.timestamps.back() == series.timestamps.back());
    REQUIRE(*reduced.values.back() == Catch::Approx(999.0));

    // A series already below the cap is untouched.
    const auto untouched = reporting::ReportBuilder::downsample(series, 5000);
    REQUIRE(untouched.size() == series.size());
}

TEST_CASE("attribution tables carry into the report JSON", "[reporting]") {
    const reporting::ReportBuilder builder;
    auto performance = fixture_report();

    analytics::AttributionTable table;
    table.dimension = "sector";
    analytics::AttributionEntry entry;
    entry.key = "sector#1";
    entry.net_pnl = Notional{1234.5};
    entry.gross_pnl = Notional{1300.0};
    entry.costs = Notional{65.5};
    entry.trades = 7;
    table.entries.emplace("sector#1", entry);
    performance.attribution.emplace("sector", table);

    auto report = builder.build(reporting::ReportKind::Daily, performance);
    REQUIRE(report.has_value());
    REQUIRE(report->tables.contains("sector"));

    auto json = builder.to_json(*report);
    REQUIRE(json->find("\"attribution\"") != std::string::npos);
    REQUIRE(json->find("sector#1") != std::string::npos);
}
