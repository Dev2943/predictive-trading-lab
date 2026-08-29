#include <algorithm>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <limits>
#include <vector>

#include "ptl/execution/broker.hpp"
#include "ptl/report/generator.hpp"
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

constexpr InstrumentId kSpy{0};
constexpr InstrumentId kQqq{1};

accounting::Trade trade(InstrumentId instrument, double pnl, double costs = 1.0,
                        Duration hold = hours{2}) {
    accounting::Trade t;
    t.instrument = instrument;
    t.direction = Side::Buy;
    t.opened = at("2024-01-02T15:00:00Z");
    t.closed = t.opened + hold;
    t.quantity = Qty{100};
    t.entry_price = Price{500.0};
    t.exit_price = Price{500.0 + pnl / 100.0};
    t.gross_pnl = Notional{pnl};
    t.costs = Notional{costs};
    return t;
}

std::vector<portfolio::EquityPoint> daily_curve(const std::vector<double>& equity) {
    std::vector<portfolio::EquityPoint> out;
    Timestamp t = at("2024-01-02T20:00:00Z");
    for (const double e : equity) {
        portfolio::EquityPoint p;
        p.ts = t;
        p.equity = Notional{e};
        p.cash = Notional{e * 0.5};
        p.gross_exposure = Notional{e * 0.8};
        p.net_exposure = Notional{e * 0.3};
        p.turnover = Notional{(static_cast<double>(out.size()) + 1.0) * 10000.0};
        out.push_back(p);
        t += hours{24};
    }
    return out;
}

/// Fills come only from a BrokerSimulator; this drives a real one.
oms::Fill make_fill(InstrumentId instrument, double qty, double price) {
    static std::uint64_t next = 0;
    SimulatedClock clock{at("2024-01-02T15:00:00Z")};
    execution::CostConfig cc;
    cc.commission_per_share = 0.001;
    cc.minimum_commission = 0.5;
    cc.stochastic_slippage_bps = Bps{0.0};
    cc.impact_coefficient = 0.0;
    execution::StandardCostModel costs{cc};
    execution::StandardLatencyModel latency;
    execution::FillConfig fc;
    fc.max_participation_rate = 1.0;
    fc.respect_displayed_size = false;
    execution::BrokerSimulator broker{clock, costs, latency, DeterministicRng{1}, fc};

    LifecycleTimes t;
    t.decision_time = clock.now();
    auto order = oms::Order::market(oms::OrderId{++next}, instrument, Side::Buy, Qty{qty}, t);
    REQUIRE(broker.submit(*order).has_value());

    execution::MarketState st;
    st.bid = Price{price};
    st.ask = Price{price};
    st.interval_volume = Volume{1e9};
    st.has_quote = true;
    clock.advance_by(seconds{1});
    auto fills = broker.on_market(instrument, st, clock.now());
    REQUIRE(fills.has_value());
    return fills->front();
}

PerformanceReport build_report() {
    AnalyzerConfig cfg;
    cfg.run_id = "test_run";
    cfg.strategy_name = "fixture";
    cfg.metrics.periods_per_year = 252.0;
    cfg.risk.periods_per_year = 252.0;

    AttributionAnalyzer attribution;
    attribution.map_instrument(kSpy, FillAttribution{kSpy, 1, "momentum", "twap"});
    attribution.map_instrument(kQqq, FillAttribution{kQqq, 2, "reversal", "vwap"});

    const auto curve = daily_curve({100000.0, 101000.0, 99500.0, 102000.0, 103000.0});
    const std::vector<accounting::Trade> trades{trade(kSpy, 500.0), trade(kSpy, -200.0),
                                                trade(kQqq, 300.0)};
    const std::vector<oms::Fill> fills{make_fill(kSpy, 100.0, 500.0), make_fill(kQqq, 50.0, 400.0)};

    const PerformanceAnalyzer analyzer{cfg};
    auto report = analyzer.analyze(curve, trades, fills, attribution);
    REQUIRE(report.has_value());
    return *report;
}

}  // namespace

// ---------------------------------------------------------------------------
// Attribution
// ---------------------------------------------------------------------------

TEST_CASE("attribution reconciles to the total", "[analytics][attribution]") {
    // A table that does not sum to the reported P&L is not attribution, it is a
    // plausible-looking table.
    AttributionAnalyzer analyzer;
    analyzer.map_instrument(kSpy, FillAttribution{kSpy, 1, "momentum", "twap"});
    analyzer.map_instrument(kQqq, FillAttribution{kQqq, 2, "reversal", "vwap"});

    const std::vector<accounting::Trade> trades{trade(kSpy, 500.0, 10.0), trade(kSpy, -200.0, 5.0),
                                                trade(kQqq, 300.0, 8.0)};

    auto table = analyzer.by_instrument(trades);
    REQUIRE(table.has_value());
    REQUIRE(table->entries.size() == 2);

    // 500 - 10 - 200 - 5 + 300 - 8
    const double expected_net = (500.0 - 10.0) + (-200.0 - 5.0) + (300.0 - 8.0);
    REQUIRE(table->total_net.get() == Catch::Approx(expected_net));
    REQUIRE(table->reconciles(Notional{expected_net}));
    REQUIRE(table->residual(Notional{expected_net}).get() == Catch::Approx(0.0));
}

TEST_CASE("sector attribution groups and reports unmapped instruments",
          "[analytics][attribution]") {
    // A missing mapping must be VISIBLE, not silent.
    AttributionAnalyzer analyzer;
    analyzer.map_instrument(kSpy, FillAttribution{kSpy, 1, "momentum", "twap"});

    const std::vector<accounting::Trade> trades{trade(kSpy, 100.0), trade(kQqq, 50.0)};
    auto table = analyzer.by_sector(trades);
    REQUIRE(table.has_value());
    REQUIRE(table->entries.contains("sector#1"));
    REQUIRE(table->entries.contains("unattributed"));
    REQUIRE(table->entries.at("unattributed").trades == 1);
}

TEST_CASE("cost attribution splits into components", "[analytics][attribution]") {
    AttributionAnalyzer analyzer;
    analyzer.map_instrument(kSpy, FillAttribution{kSpy, 1, "momentum", "twap"});

    const std::vector<oms::Fill> fills{make_fill(kSpy, 100.0, 500.0)};
    auto table = analyzer.by_cost_component(fills);
    REQUIRE(table.has_value());
    REQUIRE(table->entries.contains("commission"));
    REQUIRE(table->entries.contains("slippage_vs_arrival"));
    // Costs are NEGATIVE contributions; recording them positive would make the
    // table sum to the wrong sign.
    REQUIRE(table->entries.at("commission").net_pnl.get() <= 0.0);
}

TEST_CASE("attribution ranking is deterministic", "[analytics][attribution][determinism]") {
    AttributionAnalyzer analyzer;
    analyzer.map_instrument(kSpy, FillAttribution{kSpy, 1, "a", "twap"});
    analyzer.map_instrument(kQqq, FillAttribution{kQqq, 1, "b", "twap"});

    // Two lines with IDENTICAL net P&L: the tie-break on key is what makes the
    // ordering a pure function of the inputs.
    const std::vector<accounting::Trade> trades{trade(kSpy, 100.0, 0.0), trade(kQqq, 100.0, 0.0)};
    auto a = analyzer.by_instrument(trades);
    auto b = analyzer.by_instrument(trades);
    REQUIRE(a->ranked().front().key == b->ranked().front().key);
}

// ---------------------------------------------------------------------------
// Trade analytics
// ---------------------------------------------------------------------------

TEST_CASE("trade statistics on a known fixture", "[analytics][trades]") {
    const std::vector<accounting::Trade> trades{
        trade(kSpy, 100.0, 0.0, hours{1}), trade(kSpy, 200.0, 0.0, hours{3}),
        trade(kSpy, -50.0, 0.0, hours{2}), trade(kSpy, 0.0, 0.0, hours{1})};

    const TradeAnalyzer analyzer;
    auto s = analyzer.analyze(trades);
    REQUIRE(s.has_value());
    REQUIRE(s->trades == 4);
    REQUIRE(s->wins == 2);
    REQUIRE(s->losses == 1);
    // A scratch is counted separately: folding it into wins would inflate the
    // hit rate of a strategy that mostly breaks even.
    REQUIRE(s->scratches == 1);
    REQUIRE(s->win_rate == Catch::Approx(0.5));
    REQUIRE(s->average_win.get() == Catch::Approx(150.0));
    REQUIRE(s->average_loss.get() == Catch::Approx(50.0));
    REQUIRE(s->largest_win.get() == Catch::Approx(200.0));
    REQUIRE(s->largest_loss.get() == Catch::Approx(-50.0));
    REQUIRE(s->profit_factor == Catch::Approx(6.0));
    REQUIRE(s->expectancy.get() == Catch::Approx(62.5));
    REQUIRE(s->median_holding_period == hours{2});
}

TEST_CASE("consecutive win and loss streaks are tracked", "[analytics][trades][property]") {
    // A 55% hit rate with a twelve-loss streak is a different proposition from
    // the same rate with a three-loss streak.
    std::vector<accounting::Trade> trades;
    for (int i = 0; i < 3; ++i) trades.push_back(trade(kSpy, 100.0, 0.0));
    for (int i = 0; i < 5; ++i) trades.push_back(trade(kSpy, -50.0, 0.0));
    trades.push_back(trade(kSpy, 100.0, 0.0));

    auto s = TradeAnalyzer{}.analyze(trades);
    REQUIRE(s->max_consecutive_wins == 3);
    REQUIRE(s->max_consecutive_losses == 5);
}

TEST_CASE("an empty trade list is valid", "[analytics][trades][edge]") {
    auto s = TradeAnalyzer{}.analyze({});
    REQUIRE(s.has_value());
    REQUIRE(s->trades == 0);
    REQUIRE(s->win_rate == 0.0);
    REQUIRE(is_finite(s->profit_factor));
}

// ---------------------------------------------------------------------------
// Turnover and capacity
// ---------------------------------------------------------------------------

TEST_CASE("turnover annualises by elapsed time", "[analytics][exposure]") {
    // Elapsed time, not the observation count: a curve with gaps would
    // otherwise report a turnover rate it never achieved.
    const auto curve = daily_curve({100000.0, 100000.0, 100000.0, 100000.0, 100000.0});
    auto t = compute_turnover(curve, 252.0);
    REQUIRE(t.has_value());
    REQUIRE(t->total_turnover.get() == Catch::Approx(40000.0));
    REQUIRE(t->annualized_turnover > 0.0);
    REQUIRE(t->implied_holding_period_days > 0.0);

    REQUIRE_FALSE(compute_turnover(std::span<const portfolio::EquityPoint>{}, 252.0).has_value());
}

TEST_CASE("capacity carries its caveat", "[analytics][exposure][edge]") {
    // A capacity figure without its assumptions is a number people quote back
    // without them.
    auto c = estimate_capacity(Notional{50'000'000.0}, 50.0, 0.05);
    REQUIRE(c.has_value());
    REQUIRE(c->implied_capacity.get() > 0.0);
    REQUIRE(c->caveat.find("CRUDE ESTIMATE") != std::string::npos);
    REQUIRE(c->caveat.find("impact decay") != std::string::npos);

    REQUIRE_FALSE(estimate_capacity(Notional{0.0}, 50.0).has_value());
    REQUIRE_FALSE(estimate_capacity(Notional{1e6}, 0.0).has_value());
    REQUIRE_FALSE(estimate_capacity(Notional{1e6}, 50.0, 0.0).has_value());
}

// ---------------------------------------------------------------------------
// Analyzer and snapshots
// ---------------------------------------------------------------------------

TEST_CASE("the analyzer produces a complete report", "[analytics][report]") {
    const auto report = build_report();
    REQUIRE(report.strategy_name == "fixture");
    REQUIRE(report.initial_equity.get() == Catch::Approx(100000.0));
    REQUIRE(report.final_equity.get() == Catch::Approx(103000.0));
    REQUIRE(report.max_drawdown > 0.0);
    REQUIRE(report.daily_snapshots.size() == 5);
    REQUIRE(report.trades.trades == 3);
    REQUIRE(report.attribution.size() == 5);
    REQUIRE(report.attribution.contains("instrument"));
    REQUIRE(report.attribution.contains("cost_component"));
}

TEST_CASE("an absent benchmark is recorded as a caveat", "[analytics][report][edge]") {
    // A reader who does not know what the report could NOT determine will
    // over-trust it.
    const auto report = build_report();
    bool mentions_benchmark = false;
    for (const auto& c : report.caveats) {
        if (c.find("no benchmark") != std::string::npos) mentions_benchmark = true;
    }
    REQUIRE(mentions_benchmark);
}

TEST_CASE("snapshots group by calendar period", "[analytics][report]") {
    AnalyzerConfig cfg;
    const PerformanceAnalyzer analyzer{cfg};

    // Ten daily points spanning two months.
    std::vector<portfolio::EquityPoint> curve;
    Timestamp t = at("2024-01-28T20:00:00Z");
    for (int i = 0; i < 10; ++i) {
        portfolio::EquityPoint p;
        p.ts = t;
        p.equity = Notional{100000.0 + static_cast<double>(i) * 100.0};
        curve.push_back(p);
        t += hours{24};
    }

    auto daily = analyzer.snapshots(curve, SnapshotFrequency::Daily);
    REQUIRE(daily.has_value());
    REQUIRE(daily->size() == 10);

    auto monthly = analyzer.snapshots(curve, SnapshotFrequency::Monthly);
    REQUIRE(monthly.has_value());
    // January and February.
    REQUIRE(monthly->size() == 2);
    REQUIRE(monthly->back().equity.get() == Catch::Approx(100900.0));
}

TEST_CASE("analytics never mutate their inputs", "[analytics][report][leakage][property]") {
    // A reporting routine that mutated a position would corrupt trading state
    // in a way no backtest would reveal.
    const auto curve = daily_curve({100000.0, 101000.0, 99500.0, 102000.0});
    const std::vector<accounting::Trade> trades{trade(kSpy, 500.0)};
    const std::vector<oms::Fill> fills{make_fill(kSpy, 100.0, 500.0)};

    // Snapshot the inputs by value.
    const auto curve_before = curve;
    const auto trades_before_pnl = trades.front().gross_pnl.get();
    const auto fills_before_qty = fills.front().quantity().get();

    AttributionAnalyzer attribution;
    attribution.map_instrument(kSpy, FillAttribution{kSpy, 1, "s", "twap"});
    const PerformanceAnalyzer analyzer;
    REQUIRE(analyzer.analyze(curve, trades, fills, attribution).has_value());

    for (std::size_t i = 0; i < curve.size(); ++i) {
        REQUIRE(curve[i].equity.get() == curve_before[i].equity.get());
        REQUIRE(curve[i].ts == curve_before[i].ts);
    }
    REQUIRE(trades.front().gross_pnl.get() == trades_before_pnl);
    REQUIRE(fills.front().quantity().get() == fills_before_qty);
}

TEST_CASE("analysis is deterministic", "[analytics][report][determinism]") {
    const auto a = build_report();
    const auto b = build_report();
    REQUIRE(a.summary() == b.summary());
    REQUIRE(a.max_drawdown == b.max_drawdown);
    REQUIRE(a.metrics.sharpe == b.metrics.sharpe);
}

// ---------------------------------------------------------------------------
// Report generation
// ---------------------------------------------------------------------------

TEST_CASE("report generation is byte-identical across runs", "[report][determinism]") {
    // Same input, same bytes -- which is what lets a regression test diff two
    // runs' reports directly.
    const auto report = build_report();
    const report::ReportGenerator generator;

    for (const auto format :
         {report::Format::Csv, report::Format::Json, report::Format::Markdown}) {
        INFO("format: " << report::to_string(format));
        auto first = generator.generate(report, format);
        auto second = generator.generate(report, format);
        REQUIRE(first.has_value());
        REQUIRE(second.has_value());
        REQUIRE(*first == *second);
        REQUIRE_FALSE(first->empty());
    }
}

TEST_CASE("no wall clock leaks into a report", "[report][determinism][leakage]") {
    // A generated-at stamp would make every report differ from every other,
    // so it is supplied by the caller or absent entirely.
    const auto report = build_report();
    const report::ReportGenerator generator;
    auto json = generator.generate(report, report::Format::Json);
    REQUIRE(json.has_value());
    REQUIRE(json->find("generated_at") == std::string::npos);

    report::ReportConfig cfg;
    cfg.generated_at = "2024-01-02T00:00:00Z";
    const report::ReportGenerator stamped{cfg};
    auto with_stamp = stamped.generate(report, report::Format::Json);
    REQUIRE(with_stamp->find("generated_at") != std::string::npos);
    // Even stamped, two generations agree -- the stamp is an input, not a read
    // of the clock.
    REQUIRE(*with_stamp == *stamped.generate(report, report::Format::Json));
}

TEST_CASE("JSON is well formed and escaped", "[report][json]") {
    auto report = build_report();
    // A strategy name containing a quote and a backslash must not produce
    // invalid JSON.
    report.strategy_name = R"(quote " and \ backslash)";
    report.caveats.emplace_back("line\nbreak");

    const report::ReportGenerator generator;
    auto json = generator.generate(report, report::Format::Json);
    REQUIRE(json.has_value());
    REQUIRE(json->find(R"(\")") != std::string::npos);
    REQUIRE(json->find(R"(\\)") != std::string::npos);
    REQUIRE(json->find(R"(\n)") != std::string::npos);

    // Balanced braces is a cheap structural check.
    int depth = 0;
    bool in_string = false;
    bool escaped = false;
    for (const char c : *json) {
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
        if (c == '{') ++depth;
        if (c == '}') --depth;
        REQUIRE(depth >= 0);
    }
    REQUIRE(depth == 0);

    REQUIRE(report::json_escape("plain") == "plain");
    REQUIRE(report::json_escape("a\tb") == "a\\tb");
}

TEST_CASE("non-finite values become JSON null", "[report][json][edge]") {
    // JSON has no NaN literal, and emitting one produces a file no parser
    // accepts.
    auto report = build_report();
    // metrics.sharpe, not risk.sharpe: the JSON emits Sharpe from the Phase 3
    // metrics block, and risk.sharpe is not serialised at all.
    report.metrics.sharpe = std::numeric_limits<double>::quiet_NaN();
    report.risk.beta = std::numeric_limits<double>::infinity();

    const report::ReportGenerator generator;
    auto json = generator.generate(report, report::Format::Json);
    REQUIRE(json.has_value());
    // Search for the VALUE forms, not the bare substrings: "information_ratio"
    // legitimately contains "inf", and asserting on that would fail on a
    // perfectly correct document.
    REQUIRE(json->find(": nan") == std::string::npos);
    REQUIRE(json->find(": inf") == std::string::npos);
    REQUIRE(json->find(": -inf") == std::string::npos);
    REQUIRE(json->find(": -nan") == std::string::npos);
    // Both non-finite fields became null.
    REQUIRE(json->find("\"sharpe\": null") != std::string::npos);
    REQUIRE(json->find("\"beta\": null") != std::string::npos);
}

TEST_CASE("CSV escapes separators correctly", "[report][csv]") {
    REQUIRE(report::csv_escape("plain") == "plain");
    REQUIRE(report::csv_escape("a,b") == "\"a,b\"");
    // RFC 4180 doubles an embedded quote.
    REQUIRE(report::csv_escape("say \"hi\"") == "\"say \"\"hi\"\"\"");
    REQUIRE(report::csv_escape("line\nbreak") == "\"line\nbreak\"");

    auto report = build_report();
    report.strategy_name = "has,comma";
    const report::ReportGenerator generator;
    auto csv = generator.generate(report, report::Format::Csv);
    REQUIRE(csv.has_value());
    REQUIRE(csv->find("\"has,comma\"") != std::string::npos);
    // Every row has exactly one separator outside quotes.
    REQUIRE(csv->find("metric,value") == 0);
}

TEST_CASE("snapshot and attribution CSV have stable headers", "[report][csv]") {
    const auto report = build_report();
    const report::ReportGenerator generator;

    auto snapshots = generator.snapshots_csv(report.daily_snapshots);
    REQUIRE(snapshots.has_value());
    REQUIRE(snapshots->find("ts,equity,cash") == 0);
    // Header plus one row per snapshot.
    REQUIRE(std::count(snapshots->begin(), snapshots->end(), '\n') ==
            static_cast<long>(report.daily_snapshots.size() + 1));

    auto attribution = generator.attribution_csv(report.attribution.at("instrument"));
    REQUIRE(attribution.has_value());
    REQUIRE(attribution->find("key,net_pnl") == 0);
}

TEST_CASE("Markdown includes the caveats section", "[report][markdown]") {
    const auto report = build_report();
    const report::ReportGenerator generator;
    auto md = generator.generate(report, report::Format::Markdown);
    REQUIRE(md.has_value());
    REQUIRE(md->find("# fixture") == 0);
    REQUIRE(md->find("## Headline") != std::string::npos);
    REQUIRE(md->find("## Risk") != std::string::npos);
    REQUIRE(md->find("## Trades") != std::string::npos);
    REQUIRE(md->find("## Attribution by instrument") != std::string::npos);
    REQUIRE(md->find("## Caveats") != std::string::npos);
}

TEST_CASE("a report writes to disk and returns its path", "[report][io]") {
    const auto report = build_report();
    const report::ReportGenerator generator;
    const auto dir = std::filesystem::temp_directory_path() / "ptl_reports_test";
    std::filesystem::remove_all(dir);

    auto path = generator.write(report, report::Format::Json, dir.string());
    REQUIRE(path.has_value());
    REQUIRE(std::filesystem::exists(*path));
    REQUIRE(path->find(".json") != std::string::npos);
    REQUIRE(path->find("test_run") != std::string::npos);

    // Sized read rather than the istreambuf_iterator pair. GCC 13 emits a
    // -Wnull-dereference false positive when that idiom is inlined at -O2,
    // which fails the RelWithDebInfo build under -Werror. This form also checks
    // that the open succeeded, which the iterator version silently skips.
    std::ifstream in{*path, std::ios::binary | std::ios::ate};
    REQUIRE(in);
    const auto size = in.tellg();
    REQUIRE(size > 0);
    std::string contents(static_cast<std::size_t>(size), '\0');
    in.seekg(0);
    in.read(contents.data(), size);
    REQUIRE(in.gcount() == size);
    REQUIRE(contents == *generator.generate(report, report::Format::Json));
    std::filesystem::remove_all(dir);
}
