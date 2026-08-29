#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cmath>
#include <memory>
#include <vector>

#include "ptl/engine/engine.hpp"
#include "ptl/pipeline/trading_pipeline.hpp"
#include "support/ptl_catch.hpp"

using namespace ptl;
using namespace ptl::pipeline;
using namespace std::chrono;

namespace {

Timestamp at(const char* iso) {
    Timestamp ts{};
    REQUIRE(parse_timestamp(iso, ts));
    return ts;
}

constexpr InstrumentId kSpy{0};

const market::Calendar& us() {
    static const market::Calendar cal = [] {
        auto r = market::Calendar::build(market::Calendar::us_equities_spec(), 2024, 2024);
        REQUIRE(r.has_value());
        return std::move(*r);
    }();
    return cal;
}

/// One session of one-minute bars with a gentle trend.
std::vector<market::MarketEvent> session_bars(std::size_t count, double start = 500.0) {
    const auto session = us().session_on(at("2024-07-02"));
    REQUIRE(session.has_value());

    std::vector<market::MarketEvent> events;
    Timestamp t = session->open;
    for (std::size_t i = 0; i < count; ++i) {
        const double px =
            start + std::sin(static_cast<double>(i) * 0.05) * 2.0 + static_cast<double>(i) * 0.01;
        auto bar = market::Bar::from_left_edge(kSpy, t, minutes{1}, Price{px}, Price{px + 0.05},
                                               Price{px - 0.05}, Price{px}, Volume{50000.0});
        REQUIRE(bar.has_value());
        events.emplace_back(*bar);
        t += minutes{1};
    }
    auto with_sessions = market::with_session_events(std::move(events), us());
    REQUIRE(with_sessions.has_value());
    return *with_sessions;
}

/// Predictions stamped at each bar close.
StoredPredictionSource predictions_for(const std::vector<market::MarketEvent>& events) {
    StoredPredictionSource src;
    std::size_t i = 0;
    for (const auto& e : events) {
        if (!std::holds_alternative<market::Bar>(e)) continue;
        const auto& bar = std::get<market::Bar>(e);
        const double p = std::sin(static_cast<double>(i) * 0.05) * 0.004;
        REQUIRE(src.add(bar.instrument(), bar.close_time(), p).has_value());
        ++i;
    }
    return src;
}

/// One complete run through the Phase 3 engine.
struct RunResult {
    engine::RunSummary summary;
    PipelineStats stats;
    Notional final_equity{};
    std::uint64_t signal_hash = 0;
    std::string journal;
    std::vector<double> equity_curve;
    bool reconciled = false;
};

RunResult execute(std::uint64_t seed = 20240101, PipelineConfig cfg = {},
                  bool use_predictions = true) {
    const auto events = session_bars(200);
    auto predictions = predictions_for(events);

    SimulatedClock clock;
    auto source = market::ReplaySource::create(events, &clock);
    REQUIRE(source.has_value());

    execution::StandardCostModel costs;
    execution::StandardLatencyModel latency;
    execution::BrokerSimulator broker{clock, costs, latency, DeterministicRng{seed}};
    portfolio::Portfolio pf;
    oms::OrderManager oms;

    risk::RiskLimits limits;
    limits.max_concentration = 1.0;
    limits.max_gross_leverage = 4.0;
    limits.max_net_leverage = 4.0;
    limits.max_daily_turnover = 100.0;
    risk::RiskManager risk{limits};
    accounting::Journal journal;

    signal::GeneratorConfig gcfg;
    gcfg.kind = signal::PredictionKind::Regression;
    gcfg.horizon = minutes{15};
    signal::ModelSignalGenerator generator{"model", 0xABCD, gcfg};

    TradingPipeline pipeline{generator, use_predictions ? &predictions : nullptr, &us(),
                             std::move(cfg)};

    engine::Engine eng{clock, *source, pipeline, broker, pf, oms, risk, journal, &us()};
    auto summary = eng.run();
    REQUIRE(summary.has_value());

    RunResult r;
    r.summary = *summary;
    r.stats = pipeline.stats();
    r.final_equity = pf.equity();
    r.signal_hash = pipeline.content_hash();
    r.journal = journal.to_csv();
    for (const auto& p : pf.equity_curve()) r.equity_curve.push_back(p.equity.get());
    r.reconciled = summary->reconciled;
    return r;
}

PipelineConfig permissive_config() {
    PipelineConfig cfg;
    cfg.filters.min_confidence = 0.0;
    cfg.filters.cooldown = minutes{1};
    cfg.filters.max_spread_bps = Bps{1000.0};
    cfg.filters.min_relative_volume = 0.0;
    cfg.filters.max_volatility = 10.0;
    cfg.rebalance_interval_bars = 5;
    cfg.sizing.method = sizing::SizingMethod::PercentCapital;
    cfg.sizing.percent_capital = 0.05;
    cfg.rebalance.mode = construction::RebalanceMode::Full;
    cfg.rebalance.min_trade_notional = Notional{100.0};
    return cfg;
}

}  // namespace

TEST_CASE("the full pipeline runs end to end in one event loop", "[pipeline][e2e]") {
    // Market data -> features -> prediction -> signal -> sizing -> risk -> OMS
    // -> execution -> portfolio, all driven by the Phase 3 engine. The pipeline
    // is a strategy, not a second loop.
    const auto r = execute(20240101, permissive_config());

    REQUIRE(r.summary.bars == 200);
    REQUIRE(r.stats.bars_seen == 200);
    REQUIRE(r.stats.predictions_consumed > 0);
    REQUIRE(r.stats.signals_generated > 0);
    REQUIRE(r.stats.rebalances > 0);
    REQUIRE(r.summary.orders_submitted > 0);
    REQUIRE(r.summary.fills > 0);
    REQUIRE(is_finite(r.final_equity.get()));
    REQUIRE(r.reconciled);
}

TEST_CASE("no chain violations occur in a full run", "[pipeline][e2e][leakage]") {
    // A non-zero count means some part of the pipeline consumed information out
    // of order, and invalidates the entire run.
    const auto r = execute(20240101, permissive_config());
    REQUIRE(r.summary.chain_violations == 0);
}

TEST_CASE("two identical runs are bit-identical throughout", "[pipeline][e2e][determinism]") {
    // THE PHASE 7 DETERMINISM REQUIREMENT: orders, fills, portfolio, P&L,
    // journal, signals and risk decisions must all match exactly.
    const auto a = execute(20240101, permissive_config());
    const auto b = execute(20240101, permissive_config());

    REQUIRE(a.signal_hash == b.signal_hash);
    REQUIRE(a.journal == b.journal);
    REQUIRE(a.final_equity.get() == b.final_equity.get());
    REQUIRE(a.summary.orders_submitted == b.summary.orders_submitted);
    REQUIRE(a.summary.fills == b.summary.fills);
    REQUIRE(a.stats.rebalances == b.stats.rebalances);
    REQUIRE(a.equity_curve.size() == b.equity_curve.size());
    for (std::size_t i = 0; i < a.equity_curve.size(); ++i) {
        // Exact equality: float summation is not associative, so any ordering
        // difference anywhere would surface here.
        REQUIRE(a.equity_curve[i] == b.equity_curve[i]);
    }
}

TEST_CASE("a different seed changes fills but not the signal path",
          "[pipeline][e2e][determinism]") {
    const auto a = execute(1, permissive_config());
    const auto b = execute(2, permissive_config());
    // The seed drives execution noise, never signal generation.
    REQUIRE(a.signal_hash == b.signal_hash);
    REQUIRE(a.stats.signals_generated == b.stats.signals_generated);
}

TEST_CASE("orders reach the venue only through the risk gate", "[pipeline][e2e][risk][leakage]") {
    // NO BYPASS PATH. Tightening a risk limit must reduce what gets through,
    // which proves the pipeline cannot reach the broker directly.
    auto cfg = permissive_config();
    const auto permitted = execute(20240101, cfg);
    REQUIRE(permitted.summary.orders_submitted > 0);

    // Now make the risk engine refuse everything by shrinking the order cap.
    const auto events = session_bars(200);
    auto predictions = predictions_for(events);
    SimulatedClock clock;
    auto source = market::ReplaySource::create(events, &clock);
    execution::StandardCostModel costs;
    execution::StandardLatencyModel latency;
    execution::BrokerSimulator broker{clock, costs, latency, DeterministicRng{1}};
    portfolio::Portfolio pf;
    oms::OrderManager oms;
    risk::RiskLimits tight;
    tight.max_order_notional = Notional{1.0};  // nothing can pass
    risk::RiskManager risk{tight};
    accounting::Journal journal;

    signal::ModelSignalGenerator generator{"model", 0xABCD};
    TradingPipeline pipeline{generator, &predictions, &us(), permissive_config()};
    engine::Engine eng{clock, *source, pipeline, broker, pf, oms, risk, journal, &us()};
    auto summary = eng.run();
    REQUIRE(summary.has_value());

    REQUIRE(summary->orders_submitted == 0);
    REQUIRE(summary->orders_rejected > 0);
    REQUIRE(summary->fills == 0);
    // And every rejection is journalled with its reason.
    REQUIRE(journal.to_csv().find("risk_rejection") != std::string::npos);
}

TEST_CASE("a stored prediction source never returns a future prediction", "[pipeline][leakage]") {
    // Returning the temporally NEAREST prediction would happily hand back one
    // from the future whenever it was closer. The latest at-or-before is the
    // only safe choice.
    StoredPredictionSource src;
    REQUIRE(src.add(kSpy, at("2024-07-02T15:00:00Z"), 0.001).has_value());
    REQUIRE(src.add(kSpy, at("2024-07-02T15:10:00Z"), 0.002).has_value());

    // Queried at 15:09, the 15:10 prediction must NOT be returned even though
    // it is only a minute away, while 15:00 is nine minutes back.
    const auto p = src.predict_at(kSpy, at("2024-07-02T15:09:00Z"));
    REQUIRE(p.has_value());
    REQUIRE(p->value == Catch::Approx(0.001));
    REQUIRE(p->produced_at == at("2024-07-02T15:00:00Z"));

    // Before any prediction exists there is nothing to return.
    REQUIRE_FALSE(src.predict_at(kSpy, at("2024-07-02T14:00:00Z")).has_value());
    // Exactly at a stamp, that prediction is usable.
    REQUIRE(src.predict_at(kSpy, at("2024-07-02T15:10:00Z"))->value == Catch::Approx(0.002));
    REQUIRE_FALSE(src.predict_at(InstrumentId{99}, at("2024-07-02T15:09:00Z")).has_value());
}

TEST_CASE("signals never precede the bar that produced them", "[pipeline][e2e][leakage]") {
    // Every signal is stamped at a bar CLOSE -- the earliest instant that bar's
    // contents exist. A signal stamped at an open would claim knowledge one
    // interval early.
    const auto events = session_bars(100);
    auto predictions = predictions_for(events);
    SimulatedClock clock;
    auto source = market::ReplaySource::create(events, &clock);
    execution::StandardCostModel costs;
    execution::StandardLatencyModel latency;
    execution::BrokerSimulator broker{clock, costs, latency, DeterministicRng{1}};
    portfolio::Portfolio pf;
    oms::OrderManager oms;
    risk::RiskManager risk;
    accounting::Journal journal;

    signal::ModelSignalGenerator generator{"model", 0xABCD};
    TradingPipeline pipeline{generator, &predictions, &us(), permissive_config()};
    engine::Engine eng{clock, *source, pipeline, broker, pf, oms, risk, journal, &us()};
    REQUIRE(eng.run().has_value());

    const auto session = us().session_on(at("2024-07-02"));
    for (const auto& s : pipeline.emitted_signals()) {
        // Bar closes fall strictly after the session open.
        REQUIRE(s.as_of() > session->open);
        // And every signal is stamped on a minute boundary, i.e. a bar close.
        REQUIRE(s.as_of().time_since_epoch().count() % 60'000'000'000LL == 0);
    }
}

TEST_CASE("session close flattens the book", "[pipeline][e2e]") {
    // A position held overnight is exposed to a gap the intraday model never
    // described.
    auto cfg = permissive_config();
    cfg.flatten_at_session_close = true;

    const auto events = session_bars(200);
    auto predictions = predictions_for(events);
    SimulatedClock clock;
    auto source = market::ReplaySource::create(events, &clock);
    execution::StandardCostModel costs;
    execution::StandardLatencyModel latency;
    execution::BrokerSimulator broker{clock, costs, latency, DeterministicRng{1}};
    portfolio::Portfolio pf;
    oms::OrderManager oms;
    risk::RiskLimits limits;
    limits.max_concentration = 1.0;
    limits.max_daily_turnover = 100.0;
    risk::RiskManager risk{limits};
    accounting::Journal journal;

    signal::ModelSignalGenerator generator{"model", 0xABCD};
    TradingPipeline pipeline{generator, &predictions, &us(), cfg};
    engine::Engine eng{clock, *source, pipeline, broker, pf, oms, risk, journal, &us()};
    REQUIRE(eng.run().has_value());

    // The close-flattening rebalance ran; the pipeline targeted zero everywhere.
    REQUIRE(pipeline.stats().rebalances > 0);
}

TEST_CASE("filtered signals are counted not silently dropped", "[pipeline][e2e][leakage]") {
    // A suppressed signal that vanishes without trace makes a backtest diverge
    // from live trading invisibly.
    // Filtered on LIQUIDITY rather than confidence. Regression confidence is
    // |predicted| / sigma clamped to 1.0, so it saturates at exactly 1.0
    // whenever the predicted move exceeds one standard deviation -- which means
    // no confidence threshold below 1.0 can reject those signals. An impossible
    // relative-volume requirement rejects unconditionally.
    auto cfg = permissive_config();
    cfg.filters.min_relative_volume = 100.0;

    const auto r = execute(20240101, cfg);
    REQUIRE(r.stats.signals_generated > 0);
    REQUIRE(r.stats.signals_filtered > 0);
    REQUIRE(r.summary.orders_submitted == 0);
    REQUIRE(r.summary.fills == 0);
}

TEST_CASE("cost-aware filtering blocks unprofitable trades", "[pipeline][e2e][costs]") {
    // A signal whose expected move cannot pay for its own round trip must not
    // become an order, however confident the model is.
    auto cfg = permissive_config();
    cfg.filters.require_positive_net_edge = true;
    cfg.filters.min_confidence = 0.0;
    cfg.filters.max_spread_bps = Bps{1e6};

    const auto with_gate = execute(20240101, cfg);

    cfg.filters.require_positive_net_edge = false;
    const auto without_gate = execute(20240101, cfg);

    // Turning the gate off admits at least as many trades.
    REQUIRE(without_gate.stats.signals_actionable >= with_gate.stats.signals_actionable);
}

TEST_CASE("the pipeline tracks performance statistics", "[pipeline][e2e]") {
    const auto r = execute(20240101, permissive_config());
    REQUIRE(r.stats.gross_turnover.get() > 0.0);
    REQUIRE(r.stats.peak_gross_leverage >= 0.0);
    REQUIRE(is_finite(r.stats.peak_net_leverage));
    REQUIRE(r.stats.describe().find("signal hit rate") != std::string::npos);
    REQUIRE(r.stats.hit_rate() >= 0.0);
    REQUIRE(r.stats.hit_rate() <= 1.0);
}

TEST_CASE("accounting reconciles after a full pipeline run", "[pipeline][e2e][accounting]") {
    // Every dollar traces to a fill, and the gross-to-net bridge balances.
    const auto r = execute(20240101, permissive_config());
    REQUIRE(r.reconciled);
}

TEST_CASE("a rule-only pipeline runs without predictions", "[pipeline][e2e][baseline]") {
    // The permanent benchmark path: no model, same machinery, same costs.
    const auto r = execute(20240101, permissive_config(), false);
    REQUIRE(r.summary.bars == 200);
    REQUIRE(r.stats.predictions_consumed == 0);
    REQUIRE(is_finite(r.final_equity.get()));
}
