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

// ---------------------------------------------------------------------------
// Phase 4: the feature engine hot path
// ---------------------------------------------------------------------------
//
// This is the single most-executed code in the project. Total cost is
// instruments x bars x features x folds x configs, so a nanosecond here is
// multiplied by roughly 10^9 across a full sweep. It is also why the feature
// matrix is CACHED rather than recomputed per fold: that is an algorithmic
// saving worth more than any micro-optimisation below.

#include "ptl/features/bivariate.hpp"
#include "ptl/features/cross_sectional.hpp"
#include "ptl/features/intraday.hpp"
#include "ptl/features/momentum.hpp"

namespace {

void BM_RollingMean(benchmark::State& state) {
    ptl::features::RollingMean m{390};
    double x = 100.0;
    for (auto _ : state) {
        x += 0.01;
        m.update(x);
        benchmark::DoNotOptimize(m.value());
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_RollingMean);

/// Recomputed from the retained window rather than a running sum of squares.
/// The cost of that choice is measured here rather than assumed.
void BM_RollingStdev(benchmark::State& state) {
    ptl::features::RollingStdev sd{static_cast<std::size_t>(state.range(0))};
    double x = 100.0;
    for (auto _ : state) {
        x += 0.01;
        sd.update(x);
        benchmark::DoNotOptimize(sd.value());
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_RollingStdev)->Arg(20)->Arg(60)->Arg(390);

void BM_Ewma(benchmark::State& state) {
    ptl::features::Ewma e{60.0};
    double x = 100.0;
    for (auto _ : state) {
        x += 0.01;
        e.update(x);
        benchmark::DoNotOptimize(e.value());
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_Ewma);

void BM_LaggedReturn(benchmark::State& state) {
    ptl::features::LaggedReturn r{390};
    double x = 100.0;
    for (auto _ : state) {
        x += 0.01;
        r.update(x);
        benchmark::DoNotOptimize(r.value());
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_LaggedReturn);

void BM_RollingBeta(benchmark::State& state) {
    ptl::features::RollingBeta b{390};
    double m = 0.0;
    for (auto _ : state) {
        m += 1e-5;
        b.update(m, m * 1.5);
        benchmark::DoNotOptimize(b.value());
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_RollingBeta);

/// The whole per-instrument set: what actually runs once per bar per name.
void BM_FeatureSetOnBar(benchmark::State& state) {
    auto cal = ptl::market::Calendar::build(ptl::market::Calendar::us_equities_spec(), 2024, 2024);
    ptl::Timestamp day{};
    (void)ptl::parse_date("2024-07-02", day);
    const auto session = cal->session_on(day);

    ptl::features::FeatureSet fs{ptl::features::FeatureConfig{}, ptl::InstrumentId{0}};
    ptl::Timestamp t = session->open;
    double px = 500.0;

    for (auto _ : state) {
        px += 0.01;
        auto bar = ptl::market::Bar::from_left_edge(
            ptl::InstrumentId{0}, t, std::chrono::minutes{1}, ptl::Price{px}, ptl::Price{px + 0.05},
            ptl::Price{px - 0.05}, ptl::Price{px}, ptl::Volume{10000.0});
        fs.on_bar(*bar, &*session);
        benchmark::DoNotOptimize(fs.values().data());
        t += std::chrono::minutes{1};
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_FeatureSetOnBar);

/// The barrier, for a nine-name universe: once per bar for the whole universe,
/// not once per instrument.
void BM_CrossSectionalBarrier(benchmark::State& state) {
    ptl::features::CrossSectionalConfig cfg;
    for (std::uint32_t i = 0; i < 9; ++i) {
        cfg.universe.push_back(
            {static_cast<ptl::InstrumentId>(i), static_cast<std::int32_t>(i % 3), i == 0});
    }
    ptl::features::CrossSectionalStage stage{cfg};

    ptl::Timestamp t{};
    (void)ptl::parse_timestamp("2024-07-02T14:53:00Z", t);

    for (auto _ : state) {
        for (std::uint32_t i = 0; i < 9; ++i) {
            (void)stage.contribute(static_cast<ptl::InstrumentId>(i), t,
                                   1e-4 * static_cast<double>(i), 1e6);
        }
        auto rows = stage.compute();
        benchmark::DoNotOptimize(rows.has_value());
        t += std::chrono::minutes{1};
    }
    state.SetItemsProcessed(state.iterations() * 9);
}
BENCHMARK(BM_CrossSectionalBarrier);

}  // namespace
