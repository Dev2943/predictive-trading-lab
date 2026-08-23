/// Baseline microbenchmarks for the ptl::core primitives.
///
/// Report alongside these numbers: compiler, flags, CPU, and the fact that this
/// is a research simulator rather than a latency-sensitive system. A ns/op
/// figure without that context is marketing.

#include <benchmark/benchmark.h>

#include <cstdint>
#include <string>
#include <vector>

#include "ptl/core/instrument_table.hpp"
#include "ptl/core/rng.hpp"
#include "ptl/core/time.hpp"
#include "ptl/core/types.hpp"

namespace {

/// The RNG sits behind the latency, slippage and fill-probability models, so it
/// is called several times per simulated child order.
void BM_RngNextU64(benchmark::State& state) {
    ptl::DeterministicRng rng{20240101};
    for (auto _ : state) {
        benchmark::DoNotOptimize(rng.next_u64());
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_RngNextU64);

/// Box-Muller with rejection: the number of engine draws is data-dependent, so
/// this is worth measuring separately rather than assuming 2x next_u64().
void BM_RngNormal(benchmark::State& state) {
    ptl::DeterministicRng rng{20240101};
    for (auto _ : state) {
        benchmark::DoNotOptimize(rng.normal(0.0, 1.0));
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_RngNormal);

/// Called once per order lifecycle in debug builds. If this ever shows up in a
/// profile, the answer is to gate it, not to make it clever.
void BM_ValidateChain(benchmark::State& state) {
    ptl::Timestamp t0{};
    (void)ptl::parse_timestamp("2024-01-02T14:52:00Z", t0);
    ptl::LifecycleTimes t;
    t.exchange_time = t0;
    t.receive_time = t0 + std::chrono::milliseconds{2};
    t.feature_end_time = t0 + std::chrono::seconds{60};
    t.decision_time = t.feature_end_time;
    t.submitted_time = t.decision_time;
    t.arrival_time = t.decision_time + std::chrono::milliseconds{1};
    t.fill_time = t.arrival_time;
    for (auto _ : state) {
        benchmark::DoNotOptimize(ptl::validate_chain(t));
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_ValidateChain);

/// Ingest-path only, and ingest is explicitly cold -- it runs once and caches.
/// Measured so that Phase 2 can confirm it is not accidentally in a hot loop.
void BM_ParseTimestamp(benchmark::State& state) {
    const std::string text = "2024-01-02T14:52:00.123456789Z";
    ptl::Timestamp ts{};
    for (auto _ : state) {
        benchmark::DoNotOptimize(ptl::parse_timestamp(text, ts));
    }
    state.SetBytesProcessed(state.iterations() *
                            static_cast<benchmark::IterationCount>(text.size()));
}
BENCHMARK(BM_ParseTimestamp);

/// Dense-id lookup is the reason InstrumentTable exists. This is the cost we
/// pay ONCE at ingest so the simulation loop can index a flat vector instead.
void BM_InstrumentIntern(benchmark::State& state) {
    const std::vector<std::string> universe{"SPY", "QQQ", "IWM", "DIA", "XLF",
                                            "XLK", "XLE", "TLT", "GLD"};
    ptl::InstrumentTable table;
    for (const auto& s : universe) table.intern(s);
    std::size_t i = 0;
    for (auto _ : state) {
        benchmark::DoNotOptimize(table.intern(universe[i++ % universe.size()]));
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_InstrumentIntern);

}  // namespace

// ---------------------------------------------------------------------------
// Phase 3 hot paths
// ---------------------------------------------------------------------------

#include "ptl/execution/broker.hpp"
#include "ptl/portfolio/portfolio.hpp"
#include "ptl/risk/risk_manager.hpp"

namespace {

/// Applying a fill is the innermost accounting operation: once per fill, and a
/// sweep runs millions of them.
void BM_PortfolioApplyFill(benchmark::State& state) {
    ptl::SimulatedClock clock;
    ptl::Timestamp t{};
    (void)ptl::parse_timestamp("2024-07-02T14:53:00Z", t);
    clock.reset(t);

    ptl::execution::StandardCostModel costs;
    ptl::execution::StandardLatencyModel latency;
    ptl::execution::BrokerSimulator broker{clock, costs, latency, ptl::DeterministicRng{1}};

    ptl::LifecycleTimes times;
    times.decision_time = clock.now();
    auto order = ptl::oms::Order::market(ptl::oms::OrderId{1}, ptl::InstrumentId{0}, ptl::Side::Buy,
                                         ptl::Qty{100}, times);
    (void)broker.submit(*order);

    ptl::execution::MarketState st;
    st.bid = ptl::Price{500.0};
    st.ask = ptl::Price{500.0};
    st.interval_volume = ptl::Volume{1e9};
    st.has_quote = true;
    clock.advance_by(std::chrono::seconds{1});
    auto fills = broker.on_market(ptl::InstrumentId{0}, st, clock.now());

    ptl::portfolio::Portfolio pf;
    for (auto _ : state) {
        benchmark::DoNotOptimize(pf.apply(fills->front()));
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_PortfolioApplyFill);

/// The risk gate runs once per order and touches the whole portfolio.
void BM_RiskCheck(benchmark::State& state) {
    ptl::portfolio::Portfolio pf;
    ptl::oms::OrderManager oms;
    ptl::risk::RiskManager rm;

    ptl::Timestamp t{};
    (void)ptl::parse_timestamp("2024-07-02T14:53:00Z", t);
    ptl::LifecycleTimes times;
    times.decision_time = t;
    auto order = ptl::oms::Order::market(ptl::oms::OrderId{1}, ptl::InstrumentId{0}, ptl::Side::Buy,
                                         ptl::Qty{100}, times);

    ptl::risk::RiskContext ctx;
    ctx.now = t;
    ctx.data_age = std::chrono::seconds{1};
    ctx.reference_price = ptl::Price{500.0};
    ctx.peak_equity = pf.equity();

    for (auto _ : state) {
        benchmark::DoNotOptimize(rm.check(*order, pf, oms, ctx));
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_RiskCheck);

}  // namespace
