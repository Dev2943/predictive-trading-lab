#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cmath>
#include <limits>
#include <vector>

#include "ptl/attribution/execution_quality.hpp"
#include "ptl/attribution/pnl.hpp"
#include "ptl/execution/broker.hpp"
#include "support/ptl_catch.hpp"

using namespace ptl;
using namespace ptl::attribution;
using namespace std::chrono;

namespace {

Timestamp at(const char* iso) {
    Timestamp ts{};
    REQUIRE(parse_timestamp(iso, ts));
    return ts;
}

constexpr InstrumentId kSpy{0};

portfolio::EquityPoint point(const char* iso, double equity, double realized = 0.0,
                             double unrealized = 0.0, double gross = 0.0, double net = 0.0,
                             double cash = 0.0) {
    portfolio::EquityPoint p;
    p.ts = at(iso);
    p.equity = Notional{equity};
    p.realized_pnl = Notional{realized};
    p.unrealized_pnl = Notional{unrealized};
    p.gross_exposure = Notional{gross};
    p.net_exposure = Notional{net};
    p.cash = Notional{cash};
    return p;
}

/// Fills come only from a BrokerSimulator; this drives a real one so no test
/// ever fabricates a Fill.
oms::Fill make_fill(Side side, double qty, double price, const char* decision_iso,
                    double commission = 0.0) {
    static std::uint64_t next = 0;
    SimulatedClock clock{at(decision_iso)};
    execution::CostConfig cc;
    cc.commission_per_share = commission;
    cc.minimum_commission = 0.0;
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
    auto order = oms::Order::market(oms::OrderId{++next}, kSpy, side, Qty{qty}, t);
    REQUIRE(order.has_value());
    REQUIRE(broker.submit(order->with_arrival_price(Price{price})).has_value());

    execution::MarketState st;
    st.bid = Price{price};
    st.ask = Price{price};
    st.interval_volume = Volume{1e9};
    st.has_quote = true;
    clock.advance_by(seconds{1});
    auto fills = broker.on_market(kSpy, st, clock.now());
    REQUIRE(fills.has_value());
    REQUIRE(fills->size() == 1);
    return fills->front();
}

}  // namespace

// ---------------------------------------------------------------------------
// P&L decomposition
// ---------------------------------------------------------------------------

TEST_CASE("the P&L decomposition closes to the equity change", "[attribution][pnl][property]") {
    // THE POINT OF THE WHOLE MODULE. A decomposition that does not close is a
    // set of plausible numbers, not an explanation.
    const PnlAttributor attributor;
    const auto begin = point("2024-01-02T20:00:00Z", 100'000.0, 0.0, 0.0);
    const auto end = point("2024-01-03T20:00:00Z", 100'500.0, 300.0, 200.0);

    auto decomposition = attributor.decompose_period(begin, end, {});
    REQUIRE(decomposition.has_value());
    REQUIRE(decomposition->realized.get() == Catch::Approx(300.0));
    REQUIRE(decomposition->unrealized.get() == Catch::Approx(200.0));
    REQUIRE(decomposition->gross_pnl().get() == Catch::Approx(500.0));
    // No fills and no financing: net equals gross, and the identity holds.
    REQUIRE(decomposition->closes(Notional{500.0}));
    REQUIRE(decomposition->residual(Notional{500.0}).get() == Catch::Approx(0.0));
}

TEST_CASE("costs are stored negative so every field is a contribution", "[attribution][pnl]") {
    // One convention throughout spares every aggregate a special case, and a
    // mixed convention is how a sign error hides inside a table that adds up.
    const PnlAttributor attributor;
    const auto begin = point("2024-01-02T20:00:00Z", 100'000.0);
    const auto end = point("2024-01-03T20:00:00Z", 100'000.0);

    const std::vector<oms::Fill> fills{
        make_fill(Side::Buy, 100.0, 500.0, "2024-01-03T15:00:00Z", 0.01)};

    auto decomposition = attributor.decompose_period(begin, end, fills);
    REQUIRE(decomposition.has_value());
    REQUIRE(decomposition->commission.get() <= 0.0);
    REQUIRE(decomposition->total_costs().get() <= 0.0);
    // Net is gross plus (negative) costs, so it is the smaller of the two.
    REQUIRE(decomposition->net_pnl().get() <= decomposition->gross_pnl().get());
}

TEST_CASE("financing is applied from configured rates, not invented", "[attribution][pnl]") {
    // Borrow, carry and cash drag are properties of positions held over time.
    // No fill records them, and inferring them from a fill stream would be
    // fabrication.
    FinancingRates rates;
    rates.borrow_rate = 0.05;
    rates.cash_rate = 0.04;
    rates.opportunity_rate = 0.08;
    const PnlAttributor attributor{rates};

    // Gross 100k, net 0 => 50k short, which accrues borrow.
    const auto begin =
        point("2024-01-02T20:00:00Z", 200'000.0, 0.0, 0.0, 100'000.0, 0.0, 150'000.0);
    const auto end = point("2024-02-02T20:00:00Z", 200'000.0);

    auto decomposition = attributor.decompose_period(begin, end, {});
    REQUIRE(decomposition.has_value());
    // A month of borrow on 50k at 5%.
    REQUIRE(decomposition->borrow.get() < 0.0);
    REQUIRE(decomposition->borrow.get() == Catch::Approx(-50'000.0 * 0.05 / 12.0).epsilon(0.02));
    // Cash earns, so carry is positive here.
    REQUIRE(decomposition->carry.get() > 0.0);
    // 100k of the 200k equity is uninvested, so it drags.
    REQUIRE(decomposition->cash_drag.get() < 0.0);

    // With no rates configured, no financing is invented.
    const PnlAttributor silent;
    auto without = silent.decompose_period(begin, end, {});
    REQUIRE(without->borrow.get() == Catch::Approx(0.0));
    REQUIRE(without->cash_drag.get() == Catch::Approx(0.0));
}

TEST_CASE("a decomposition series covers every interval", "[attribution][pnl]") {
    const PnlAttributor attributor;
    const std::vector<portfolio::EquityPoint> curve{
        point("2024-01-02T20:00:00Z", 100'000.0, 0.0),
        point("2024-01-03T20:00:00Z", 100'500.0, 500.0),
        point("2024-01-04T20:00:00Z", 100'200.0, 200.0),
        point("2024-01-05T20:00:00Z", 101'000.0, 1000.0)};

    auto series = attributor.decompose_series(curve, {});
    REQUIRE(series.has_value());
    // One entry per interval, not per observation.
    REQUIRE(series->size() == curve.size() - 1);

    const auto total = PnlAttributor::aggregate(*series);
    // Aggregating the periods reproduces the whole-span realised P&L.
    REQUIRE(total.realized.get() == Catch::Approx(1000.0));
    REQUIRE(total.period_begin == curve.front().ts);
    REQUIRE(total.period_end == curve.back().ts);

    REQUIRE_FALSE(attributor.decompose_series(std::span{curve.data(), 1}, {}).has_value());
}

TEST_CASE("beta and alpha contributions sum to the portfolio return",
          "[attribution][pnl][numerical]") {
    // Portfolio exactly twice the benchmark: beta 2, alpha 0.
    std::vector<double> benchmark;
    std::vector<double> portfolio;
    for (int i = 0; i < 100; ++i) {
        const double b = std::sin(static_cast<double>(i) * 0.3) * 0.01;
        benchmark.push_back(b);
        portfolio.push_back(2.0 * b);
    }

    auto contribution = PnlAttributor::factor_contribution(portfolio, benchmark);
    REQUIRE(contribution.has_value());
    REQUIRE(contribution->beta == Catch::Approx(2.0).epsilon(1e-9));
    REQUIRE(contribution->alpha_contribution == Catch::Approx(0.0).margin(1e-9));
    // The identity that makes the decomposition meaningful.
    REQUIRE(contribution->closes(1e-9));
}

TEST_CASE("a zero-variance benchmark leaves everything as alpha", "[attribution][pnl][edge]") {
    // A benchmark that never moves explains nothing, so beta is zero rather
    // than infinite and the whole return is unexplained.
    const std::vector<double> portfolio{0.01, 0.02, -0.01, 0.03};
    const std::vector<double> flat{0.0, 0.0, 0.0, 0.0};

    auto contribution = PnlAttributor::factor_contribution(portfolio, flat);
    REQUIRE(contribution.has_value());
    REQUIRE(contribution->beta == Catch::Approx(0.0));
    REQUIRE(contribution->beta_contribution == Catch::Approx(0.0));
    REQUIRE(contribution->alpha_contribution == Catch::Approx(contribution->portfolio_return));
    REQUIRE(is_finite(contribution->beta));
}

TEST_CASE("a mismatched benchmark is refused not truncated", "[attribution][pnl][leakage]") {
    const std::vector<double> portfolio{0.01, 0.02, 0.03};
    const std::vector<double> benchmark{0.01, 0.02};
    auto contribution = PnlAttributor::factor_contribution(portfolio, benchmark);
    REQUIRE_FALSE(contribution.has_value());
    REQUIRE(contribution.error().message.find("does not match") != std::string::npos);
}

TEST_CASE("an inverted period is refused", "[attribution][pnl][leakage]") {
    // A period whose end precedes its beginning would produce negative
    // financing accruals and a nonsensical decomposition.
    const PnlAttributor attributor;
    const auto begin = point("2024-01-03T20:00:00Z", 100'000.0);
    const auto end = point("2024-01-02T20:00:00Z", 100'000.0);
    REQUIRE_FALSE(attributor.decompose_period(begin, end, {}).has_value());
}

// ---------------------------------------------------------------------------
// Execution quality
// ---------------------------------------------------------------------------

TEST_CASE("cost is positive for a bad fill on either side", "[attribution][execution][property]") {
    // A buy filling above its benchmark and a sell filling below both hurt.
    const auto buy_bad = ExecutionQualityAnalyzer::cost_bps(Price{101.0}, Price{100.0}, Side::Buy);
    const auto sell_bad = ExecutionQualityAnalyzer::cost_bps(Price{99.0}, Price{100.0}, Side::Sell);
    REQUIRE(buy_bad.get() > 0.0);
    REQUIRE(sell_bad.get() > 0.0);
    REQUIRE(buy_bad.get() == Catch::Approx(sell_bad.get()));

    // And negative for a good fill on either side.
    REQUIRE(ExecutionQualityAnalyzer::cost_bps(Price{99.0}, Price{100.0}, Side::Buy).get() < 0.0);
    REQUIRE(ExecutionQualityAnalyzer::cost_bps(Price{101.0}, Price{100.0}, Side::Sell).get() < 0.0);

    // A zero benchmark cannot produce a ratio, so it produces zero.
    REQUIRE(ExecutionQualityAnalyzer::cost_bps(Price{100.0}, Price{0.0}, Side::Buy).get() == 0.0);
}

TEST_CASE("implementation shortfall splits into delay and execution", "[attribution][execution]") {
    // Delay belongs to the STRATEGY, execution to the ALGORITHM. Reporting only
    // the total makes it impossible to know which to fix.
    const ExecutionQualityAnalyzer analyzer;
    const std::vector<oms::Fill> fills{make_fill(Side::Buy, 100.0, 101.0, "2024-01-03T15:00:00Z")};

    ExecutionBenchmarks benchmarks;
    benchmarks.decision_price = Price{100.0};
    benchmarks.arrival_price = Price{100.5};

    auto quality = analyzer.analyze_trade(fills, benchmarks, Qty{100.0});
    REQUIRE(quality.has_value());
    REQUIRE(quality->delay_cost.has_value());
    REQUIRE(quality->execution_cost.has_value());
    REQUIRE(quality->implementation_shortfall.has_value());

    // 100 -> 100.5 is 50 bps of delay.
    REQUIRE(quality->delay_cost->get() == Catch::Approx(50.0).epsilon(0.01));
    // The parts sum to the whole, within rounding.
    REQUIRE(
        quality->implementation_shortfall->get() ==
        Catch::Approx(quality->delay_cost->get() + quality->execution_cost->get()).epsilon(0.01));
}

TEST_CASE("absent benchmarks are not reported rather than substituted",
          "[attribution][execution][edge]") {
    // A bar-only backtest has no interval VWAP. Computing one from a
    // substitute would be a number with no provenance.
    const ExecutionQualityAnalyzer analyzer;
    const std::vector<oms::Fill> fills{make_fill(Side::Buy, 100.0, 500.0, "2024-01-03T15:00:00Z")};

    auto quality = analyzer.analyze_trade(fills, ExecutionBenchmarks{}, Qty{100.0});
    REQUIRE(quality.has_value());
    REQUIRE_FALSE(quality->implementation_shortfall.has_value());
    REQUIRE_FALSE(quality->vs_vwap.has_value());
    REQUIRE_FALSE(quality->participation_rate.has_value());
    // What IS computable is still computed.
    REQUIRE(quality->average_fill_price.get() == Catch::Approx(500.0));
    REQUIRE(quality->fill_efficiency == Catch::Approx(1.0));
}

TEST_CASE("fill efficiency detects an incomplete execution", "[attribution][execution]") {
    // Filling badly and failing to fill are distinct failures.
    const ExecutionQualityAnalyzer analyzer;
    const std::vector<oms::Fill> fills{make_fill(Side::Buy, 60.0, 500.0, "2024-01-03T15:00:00Z")};

    auto quality = analyzer.analyze_trade(fills, ExecutionBenchmarks{}, Qty{100.0});
    REQUIRE(quality.has_value());
    REQUIRE(quality->fill_efficiency == Catch::Approx(0.6));
}

TEST_CASE("participation rate uses observed market volume", "[attribution][execution]") {
    const ExecutionQualityAnalyzer analyzer;
    const std::vector<oms::Fill> fills{make_fill(Side::Buy, 500.0, 100.0, "2024-01-03T15:00:00Z")};

    ExecutionBenchmarks benchmarks;
    benchmarks.interval_volume = Volume{10000.0};

    auto quality = analyzer.analyze_trade(fills, benchmarks, Qty{500.0});
    REQUIRE(quality->participation_rate.has_value());
    REQUIRE(*quality->participation_rate == Catch::Approx(0.05));
}

TEST_CASE("MFE and MAE are measured from the fill, not the decision",
          "[attribution][execution][property]") {
    // Excursion is about the position actually held; a position not yet filled
    // cannot have suffered from a move.
    const ExecutionQualityAnalyzer analyzer;
    const std::vector<oms::Fill> fills{make_fill(Side::Buy, 100.0, 100.0, "2024-01-03T15:00:00Z")};

    ExecutionBenchmarks benchmarks;
    benchmarks.best_price = Price{110.0};
    benchmarks.worst_price = Price{95.0};

    auto quality = analyzer.analyze_trade(fills, benchmarks, Qty{100.0});
    REQUIRE(quality.has_value());
    REQUIRE(quality->max_favorable_excursion.has_value());
    REQUIRE(quality->max_adverse_excursion.has_value());
    // Both are reported as positive magnitudes.
    REQUIRE(quality->max_favorable_excursion->get() > 0.0);
    REQUIRE(quality->max_adverse_excursion->get() > 0.0);
    REQUIRE(quality->max_favorable_excursion->get() == Catch::Approx(1000.0).epsilon(0.01));
    REQUIRE(quality->max_adverse_excursion->get() == Catch::Approx(500.0).epsilon(0.01));

    // The SHORT direction, where favourable means the price FELL. A long-only
    // test would have passed with the signs inverted.
    const std::vector<oms::Fill> short_fills{
        make_fill(Side::Sell, 100.0, 100.0, "2024-01-03T15:00:00Z")};
    ExecutionBenchmarks short_benchmarks;
    short_benchmarks.best_price = Price{90.0};    // a fall helps a short
    short_benchmarks.worst_price = Price{105.0};  // a rise hurts it

    auto short_quality = analyzer.analyze_trade(short_fills, short_benchmarks, Qty{100.0});
    REQUIRE(short_quality.has_value());
    REQUIRE(short_quality->max_favorable_excursion->get() == Catch::Approx(1000.0).epsilon(0.01));
    REQUIRE(short_quality->max_adverse_excursion->get() == Catch::Approx(500.0).epsilon(0.01));
}

TEST_CASE("a trade spanning instruments or sides is refused",
          "[attribution][execution][validation]") {
    // An average price across two instruments has no meaning.
    const ExecutionQualityAnalyzer analyzer;
    const std::vector<oms::Fill> mixed_sides{
        make_fill(Side::Buy, 100.0, 500.0, "2024-01-03T15:00:00Z"),
        make_fill(Side::Sell, 100.0, 500.0, "2024-01-03T15:00:00Z")};
    REQUIRE_FALSE(
        analyzer.analyze_trade(mixed_sides, ExecutionBenchmarks{}, Qty{100.0}).has_value());
    REQUIRE_FALSE(analyzer.analyze_trade({}, ExecutionBenchmarks{}, Qty{100.0}).has_value());
}

TEST_CASE("the summary is quantity-weighted, not trade-weighted",
          "[attribution][execution][property]") {
    // A hundred-share trade and a hundred-thousand-share trade do not deserve
    // equal say in an average execution cost.
    const ExecutionQualityAnalyzer analyzer;

    TradeExecutionQuality small;
    small.quantity = Qty{100.0};
    small.implementation_shortfall = Bps{100.0};
    small.fill_count = 1;

    TradeExecutionQuality large;
    large.quantity = Qty{9900.0};
    large.implementation_shortfall = Bps{0.0};
    large.fill_count = 1;

    const std::vector<TradeExecutionQuality> trades{small, large};
    auto summary = analyzer.summarize(trades);
    REQUIRE(summary.has_value());
    REQUIRE(summary->trades == 2);
    // Trade-weighted would give 50; quantity-weighted gives 1.
    REQUIRE(summary->average_implementation_shortfall.get() == Catch::Approx(1.0));
    REQUIRE(summary->worst_implementation_shortfall.get() == Catch::Approx(100.0));
}

TEST_CASE("signal decay buckets by delay and finds a half-life",
          "[attribution][execution][property]") {
    const ExecutionQualityAnalyzer analyzer;
    const Timestamp decision = at("2024-01-03T15:00:00Z");

    std::vector<TradeExecutionQuality> trades;
    // Edge halves as delay grows: 100 bps quick, 40 bps slow.
    for (int i = 0; i < 5; ++i) {
        TradeExecutionQuality quick;
        quick.decision_time = decision;
        quick.first_fill_time = decision + seconds{10};
        quick.realized_edge = Bps{100.0};
        quick.quantity = Qty{100.0};
        trades.push_back(quick);

        TradeExecutionQuality slow;
        slow.decision_time = decision;
        slow.first_fill_time = decision + minutes{10};
        slow.realized_edge = Bps{40.0};
        slow.quantity = Qty{100.0};
        trades.push_back(slow);
    }

    const std::vector<Duration> horizons{seconds{30}, minutes{5}, minutes{30}};
    auto profile = analyzer.signal_decay(trades, horizons);
    REQUIRE(profile.has_value());
    REQUIRE(profile->mean_edge_bps[0] == Catch::Approx(100.0));
    REQUIRE(profile->mean_edge_bps[2] == Catch::Approx(40.0));
    // 40 is below half of 100, so the half-life is the bucket where it fell.
    REQUIRE(profile->half_life.has_value());
    REQUIRE(*profile->half_life == minutes{30});

    // Unordered horizons are refused rather than silently sorted.
    const std::vector<Duration> unordered{minutes{5}, seconds{30}};
    REQUIRE_FALSE(analyzer.signal_decay(trades, unordered).has_value());
    REQUIRE_FALSE(analyzer.signal_decay(trades, {}).has_value());
}
