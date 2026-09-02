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

// ---------------------------------------------------------------------------
// Phase 5: research workflow
// ---------------------------------------------------------------------------
//
// Fold generation runs once per configuration, but a parameter sweep multiplies
// it by the grid size, and purging is O(train x 1) interval tests per fold.
// Evaluation runs once per fold per metric.

#include "ptl/labels/label.hpp"
#include "ptl/research/evaluation.hpp"
#include "ptl/validation/walk_forward.hpp"

namespace {

std::vector<ptl::ObservationInterval> bench_intervals(std::size_t n, std::size_t horizon) {
    std::vector<ptl::ObservationInterval> out;
    out.reserve(n);
    ptl::Timestamp origin{};
    (void)ptl::parse_timestamp("2024-01-02T14:00:00Z", origin);
    for (std::size_t i = 0; i < n; ++i) {
        ptl::ObservationInterval iv;
        iv.sample_start_time = origin;
        iv.feature_end_time = origin + std::chrono::minutes{static_cast<long>(i)};
        iv.label_start_time = iv.feature_end_time;
        iv.label_end_time = origin + std::chrono::minutes{static_cast<long>(i + horizon)};
        out.push_back(iv);
    }
    return out;
}

/// Fold generation with interval-overlap purging over a realistic sample.
void BM_WalkForwardSplit(benchmark::State& state) {
    const auto n = static_cast<std::size_t>(state.range(0));
    const auto ivs = bench_intervals(n, 15);

    ptl::validation::WalkForwardConfig cfg;
    cfg.train_size = n / 4;
    cfg.validation_size = n / 20;
    cfg.test_size = n / 20;
    cfg.step = n / 20;
    cfg.embargo = 100;
    cfg.min_train_rows = 10;
    const ptl::validation::WalkForwardValidator v{cfg};

    for (auto _ : state) {
        auto folds = v.split(ivs);
        benchmark::DoNotOptimize(folds.has_value());
    }
    state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(n));
}
BENCHMARK(BM_WalkForwardSplit)->Arg(10000)->Arg(100000);

/// Label construction over one instrument's series.
void BM_LabelBuild(benchmark::State& state) {
    std::vector<ptl::labels::PricePoint> prices;
    ptl::Timestamp t{};
    (void)ptl::parse_timestamp("2024-01-02T14:00:00Z", t);
    for (std::size_t i = 0; i < 100000; ++i) {
        prices.push_back(
            {t, ptl::InstrumentId{0}, ptl::Price{100.0 + std::sin(static_cast<double>(i) * 0.01)}});
        t += std::chrono::minutes{1};
    }

    ptl::labels::LabelConfig cfg;
    cfg.kind = ptl::labels::LabelKind::VolNormalisedReturn;
    cfg.horizon = 15;
    const ptl::labels::LabelBuilder builder{cfg};

    for (auto _ : state) {
        auto labels = builder.build(prices);
        benchmark::DoNotOptimize(labels.has_value());
    }
    state.SetItemsProcessed(state.iterations() * 100000);
}
BENCHMARK(BM_LabelBuild);

/// Spearman rank IC: the primary evaluation statistic, computed per fold.
void BM_SpearmanIC(benchmark::State& state) {
    const auto n = static_cast<std::size_t>(state.range(0));
    std::vector<double> pred(n);
    std::vector<double> actual(n);
    for (std::size_t i = 0; i < n; ++i) {
        pred[i] = std::sin(static_cast<double>(i) * 0.017);
        actual[i] = std::cos(static_cast<double>(i) * 0.013);
    }
    for (auto _ : state) {
        benchmark::DoNotOptimize(ptl::research::Evaluator::spearman(pred, actual));
    }
    state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(n));
}
BENCHMARK(BM_SpearmanIC)->Arg(1000)->Arg(50000);

}  // namespace

// ---------------------------------------------------------------------------
// Phase 6: model fit and inference
// ---------------------------------------------------------------------------
//
// Fitting runs once per fold per configuration -- a sweep multiplies it by the
// grid size. Inference runs once per observation in every test fold, and again
// per bar in a live session, so its latency is the one that reaches production.

#if defined(PTL_HAVE_MODELS)

#include "ptl/models/pipeline.hpp"

namespace {

struct BenchData {
    ptl::models::TrainingData training;
    ptl::models::DesignMatrix batch;
};

BenchData make_bench_data(std::size_t n, std::size_t p, bool binary_targets) {
    ptl::models::DesignMatrix X{n, p};
    std::vector<double> y;
    y.reserve(n);
    ptl::DeterministicRng rng{20240101};

    for (std::size_t r = 0; r < n; ++r) {
        double signal = 0.0;
        for (std::size_t c = 0; c < p; ++c) {
            const double v = rng.normal(0.0, 1.0);
            X.set(r, c, v);
            signal += v * (static_cast<double>(c % 5) - 2.0) * 0.1;
        }
        y.push_back(binary_targets ? (signal > 0.0 ? 1.0 : 0.0) : signal + rng.normal(0.0, 0.1));
    }

    BenchData d;
    d.batch = X;
    d.training.features = std::move(X);
    d.training.targets = std::move(y);
    return d;
}

void BM_OlsFit(benchmark::State& state) {
    const auto n = static_cast<std::size_t>(state.range(0));
    const auto p = static_cast<std::size_t>(state.range(1));
    auto data = make_bench_data(n, p, false);

    ptl::models::LinearConfig cfg;
    cfg.compute_diagnostics = false;  // a sweep only needs the predictions
    for (auto _ : state) {
        ptl::models::LinearRegression m{cfg};
        benchmark::DoNotOptimize(m.fit(data.training).has_value());
    }
    state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(n));
}
BENCHMARK(BM_OlsFit)->Args({5000, 25})->Args({50000, 25})->Args({50000, 60});

void BM_RidgeFit(benchmark::State& state) {
    const auto n = static_cast<std::size_t>(state.range(0));
    auto data = make_bench_data(n, 25, false);
    ptl::models::LinearConfig cfg;
    cfg.l2_penalty = 1.0;
    cfg.compute_diagnostics = false;
    for (auto _ : state) {
        ptl::models::LinearRegression m{cfg};
        benchmark::DoNotOptimize(m.fit(data.training).has_value());
    }
    state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(n));
}
BENCHMARK(BM_RidgeFit)->Arg(5000)->Arg(50000);

/// With diagnostics on: the extra eigen-decomposition and VIF solve are what a
/// report pays for, and the gap against BM_OlsFit is the price of them.
void BM_OlsFitWithDiagnostics(benchmark::State& state) {
    auto data = make_bench_data(50000, 25, false);
    ptl::models::LinearConfig cfg;
    cfg.compute_diagnostics = true;
    for (auto _ : state) {
        ptl::models::LinearRegression m{cfg};
        benchmark::DoNotOptimize(m.fit(data.training).has_value());
    }
    state.SetItemsProcessed(state.iterations() * 50000);
}
BENCHMARK(BM_OlsFitWithDiagnostics);

void BM_LogisticFit(benchmark::State& state) {
    const auto n = static_cast<std::size_t>(state.range(0));
    auto data = make_bench_data(n, 25, true);
    for (auto _ : state) {
        ptl::models::LogisticRegression m;
        benchmark::DoNotOptimize(m.fit(data.training).has_value());
    }
    state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(n));
}
BENCHMARK(BM_LogisticFit)->Arg(5000)->Arg(50000);

/// Single-observation latency: the number that reaches a live session.
void BM_PredictLatency(benchmark::State& state) {
    auto data = make_bench_data(5000, 25, false);
    ptl::models::LinearRegression m;
    (void)m.fit(data.training);
    const auto row = data.batch.row(0);
    for (auto _ : state) {
        benchmark::DoNotOptimize(m.predict(row).has_value());
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_PredictLatency);

void BM_PredictBatch(benchmark::State& state) {
    auto data = make_bench_data(static_cast<std::size_t>(state.range(0)), 25, false);
    ptl::models::LinearRegression m;
    (void)m.fit(data.training);
    for (auto _ : state) {
        auto out = m.predict_batch(data.batch);
        benchmark::DoNotOptimize(out.has_value());
    }
    state.SetItemsProcessed(state.iterations() * state.range(0));
}
BENCHMARK(BM_PredictBatch)->Arg(1000)->Arg(50000);

void BM_LogisticPredictLatency(benchmark::State& state) {
    auto data = make_bench_data(5000, 25, true);
    ptl::models::LogisticRegression m;
    (void)m.fit(data.training);
    const auto row = data.batch.row(0);
    for (auto _ : state) {
        benchmark::DoNotOptimize(m.predict_proba(row).has_value());
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_LogisticPredictLatency);

/// Standardisation runs once per fold over the training rows and again over the
/// test rows, so it is on the critical path of every sweep.
void BM_ScalerFitTransform(benchmark::State& state) {
    auto data = make_bench_data(50000, 25, false);
    for (auto _ : state) {
        ptl::models::StandardScaler s;
        auto copy = data.batch;
        benchmark::DoNotOptimize(s.fit(copy).has_value());
        benchmark::DoNotOptimize(s.transform(copy).has_value());
    }
    state.SetItemsProcessed(state.iterations() * 50000);
}
BENCHMARK(BM_ScalerFitTransform);

}  // namespace

#endif  // PTL_HAVE_MODELS

// ---------------------------------------------------------------------------
// Phase 7: signal, sizing, construction and the full trading pipeline
// ---------------------------------------------------------------------------
//
// These run once per instrument per bar (signal, sizing) or once per rebalance
// (construction, orders). The per-bar figure is the one that reaches a live
// session: it bounds how long the strategy may take before the next event.

#include "ptl/construction/rebalance.hpp"
#include "ptl/engine/engine.hpp"
#include "ptl/market/source.hpp"
#include "ptl/pipeline/trading_pipeline.hpp"
#include "ptl/signal/filter.hpp"
#include "ptl/signal/generator.hpp"
#include "ptl/sizing/sizer.hpp"

namespace {

[[nodiscard]] ptl::Timestamp bench_time() {
    ptl::Timestamp t{};
    (void)ptl::parse_timestamp("2024-07-02T15:00:00Z", t);
    return t;
}

[[nodiscard]] ptl::signal::GeneratorInput bench_input(double prediction) {
    ptl::signal::GeneratorInput in;
    in.as_of = bench_time();
    in.instrument = ptl::InstrumentId{0};
    in.prediction = prediction;
    in.prediction_time = in.as_of;
    in.volatility = 0.001;
    in.reference_price = ptl::Price{500.0};
    in.costs.half_spread = 0.0001;
    return in;
}

void BM_SignalGeneration(benchmark::State& state) {
    const ptl::signal::ModelSignalGenerator gen{"model", 0xABCD};
    const auto input = bench_input(0.004);
    for (auto _ : state) {
        auto s = gen.generate(input);
        benchmark::DoNotOptimize(s.has_value());
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_SignalGeneration);

/// An ensemble pays for every member on every bar.
void BM_EnsembleSignalGeneration(benchmark::State& state) {
    ptl::signal::EnsembleSignalGenerator ens{"ens", 0xFFFF,
                                             ptl::signal::EnsembleMethod::ConfidenceWeighted};
    for (int i = 0; i < 5; ++i) {
        (void)ens.add(std::make_shared<ptl::signal::ModelSignalGenerator>(
                          "m", static_cast<std::uint64_t>(i + 1)),
                      1.0);
    }
    const auto input = bench_input(0.004);
    for (auto _ : state) {
        auto s = ens.generate(input);
        benchmark::DoNotOptimize(s.has_value());
    }
    state.SetItemsProcessed(state.iterations() * 5);
}
BENCHMARK(BM_EnsembleSignalGeneration);

void BM_SignalFilter(benchmark::State& state) {
    auto cal = ptl::market::Calendar::build(ptl::market::Calendar::us_equities_spec(), 2024, 2024);
    const ptl::signal::SignalFilterChain chain;

    auto sig = ptl::signal::Signal::create(bench_time(), ptl::InstrumentId{0},
                                           ptl::signal::Direction::Long, 0.005, 0.8,
                                           std::chrono::minutes{15}, 0xABCD);
    ptl::signal::FilterContext ctx;
    ctx.now = bench_time();
    ctx.realized_volatility = 0.01;
    ctx.interval_volume = 100000.0;
    ctx.average_volume = 100000.0;
    ctx.spread_bps = ptl::Bps{2.0};
    ctx.has_market_data = true;

    for (auto _ : state) {
        benchmark::DoNotOptimize(chain.evaluate(*sig, ctx, &*cal).passed());
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_SignalFilter);

void BM_PositionSizing(benchmark::State& state) {
    ptl::sizing::SizingConfig cfg;
    cfg.method = ptl::sizing::SizingMethod::VolatilityTarget;
    const ptl::sizing::PositionSizer sizer{cfg};

    auto sig = ptl::signal::Signal::create(bench_time(), ptl::InstrumentId{0},
                                           ptl::signal::Direction::Long, 0.005, 0.8,
                                           std::chrono::minutes{15}, 0xABCD);
    ptl::sizing::SizingContext ctx;
    ctx.now = bench_time();
    ctx.equity = ptl::Notional{1'000'000.0};
    ctx.reference_price = ptl::Price{500.0};
    ctx.volatility = 0.001;

    for (auto _ : state) {
        auto d = sizer.size(*sig, ctx);
        benchmark::DoNotOptimize(d.has_value());
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_PositionSizing);

/// Rebalance planning over a universe: the cost scales with instrument count.
void BM_RebalancePlan(benchmark::State& state) {
    const auto n = static_cast<std::uint32_t>(state.range(0));
    ptl::portfolio::Portfolio pf;
    for (std::uint32_t i = 0; i < n; ++i) {
        pf.mark(static_cast<ptl::InstrumentId>(i), ptl::Price{500.0}, ptl::Price{500.0});
    }

    ptl::construction::RebalanceConfig cfg;
    cfg.mode = ptl::construction::RebalanceMode::Full;
    cfg.max_turnover_per_rebalance = 0.0;  // unlimited, for benchmarking
    const ptl::construction::RebalanceEngine engine{cfg};

    for (auto _ : state) {
        ptl::construction::TargetPortfolio targets{bench_time()};
        for (std::uint32_t i = 0; i < n; ++i) {
            ptl::construction::TargetPosition t;
            t.instrument = static_cast<ptl::InstrumentId>(i);
            t.target_quantity = ptl::Qty{10.0};
            t.reference_price = ptl::Price{500.0};
            (void)targets.set(t);
        }
        auto plan = engine.plan(targets, pf);
        benchmark::DoNotOptimize(plan.has_value());
    }
    state.SetItemsProcessed(state.iterations() * state.range(0));
}
BENCHMARK(BM_RebalancePlan)->Arg(9)->Arg(100);

void BM_OrderGeneration(benchmark::State& state) {
    ptl::portfolio::Portfolio pf;
    for (std::uint32_t i = 0; i < 9; ++i) {
        pf.mark(static_cast<ptl::InstrumentId>(i), ptl::Price{500.0}, ptl::Price{500.0});
    }
    ptl::construction::RebalanceConfig cfg;
    cfg.mode = ptl::construction::RebalanceMode::Full;
    const ptl::construction::RebalanceEngine engine{cfg};

    ptl::construction::TargetPortfolio targets{bench_time()};
    for (std::uint32_t i = 0; i < 9; ++i) {
        ptl::construction::TargetPosition t;
        t.instrument = static_cast<ptl::InstrumentId>(i);
        t.target_quantity = ptl::Qty{10.0};
        t.reference_price = ptl::Price{500.0};
        (void)targets.set(t);
    }
    auto plan = engine.plan(targets, pf);

    std::uint64_t next = 0;
    for (auto _ : state) {
        auto orders =
            engine.to_orders(*plan, bench_time(), [&next] { return ptl::oms::OrderId{++next}; });
        benchmark::DoNotOptimize(orders.has_value());
    }
    state.SetItemsProcessed(state.iterations() * 9);
}
BENCHMARK(BM_OrderGeneration);

/// THE NUMBER THAT REACHES A LIVE SESSION: end-to-end latency per bar, through
/// signal, filter, sizing, rebalance, risk, OMS and the venue.
void BM_FullPipelinePerBar(benchmark::State& state) {
    auto cal = ptl::market::Calendar::build(ptl::market::Calendar::us_equities_spec(), 2024, 2024);
    ptl::Timestamp day{};
    (void)ptl::parse_date("2024-07-02", day);
    const auto session = cal->session_on(day);

    std::vector<ptl::market::MarketEvent> events;
    ptl::Timestamp t = session->open;
    for (std::size_t i = 0; i < 380; ++i) {
        const double px = 500.0 + std::sin(static_cast<double>(i) * 0.05) * 2.0;
        auto bar = ptl::market::Bar::from_left_edge(
            ptl::InstrumentId{0}, t, std::chrono::minutes{1}, ptl::Price{px}, ptl::Price{px + 0.05},
            ptl::Price{px - 0.05}, ptl::Price{px}, ptl::Volume{50000.0});
        events.emplace_back(*bar);
        t += std::chrono::minutes{1};
    }
    auto with_sessions = ptl::market::with_session_events(std::move(events), *cal);

    ptl::pipeline::StoredPredictionSource predictions;
    std::size_t idx = 0;
    for (const auto& e : *with_sessions) {
        if (!std::holds_alternative<ptl::market::Bar>(e)) continue;
        const auto& bar = std::get<ptl::market::Bar>(e);
        (void)predictions.add(bar.instrument(), bar.close_time(),
                              std::sin(static_cast<double>(idx++) * 0.05) * 0.004);
    }

    ptl::pipeline::PipelineConfig pcfg;
    pcfg.filters.min_confidence = 0.0;
    pcfg.filters.cooldown = std::chrono::minutes{1};
    pcfg.rebalance_interval_bars = 5;

    for (auto _ : state) {
        ptl::SimulatedClock clock;
        auto source = ptl::market::ReplaySource::create(*with_sessions, &clock);
        ptl::execution::StandardCostModel costs;
        ptl::execution::StandardLatencyModel latency;
        ptl::execution::BrokerSimulator broker{clock, costs, latency, ptl::DeterministicRng{1}};
        ptl::portfolio::Portfolio pf;
        ptl::oms::OrderManager oms;
        ptl::risk::RiskManager risk;
        ptl::accounting::Journal journal;
        ptl::signal::ModelSignalGenerator generator{"model", 0xABCD};
        ptl::pipeline::TradingPipeline pipeline{generator, &predictions, &*cal, pcfg};

        ptl::engine::Engine eng{clock, *source, pipeline, broker, pf, oms, risk, journal, &*cal};
        auto summary = eng.run();
        benchmark::DoNotOptimize(summary.has_value());
    }
    state.SetItemsProcessed(state.iterations() * 380);
}
BENCHMARK(BM_FullPipelinePerBar);

}  // namespace

// ---------------------------------------------------------------------------
// Phase 8: quote processing, decoding and quote-aware execution
// ---------------------------------------------------------------------------
//
// Quote updates are the highest-frequency operation in the system: at T3
// (cbbo-1s) that is one per instrument per second, and a live feed is faster
// still. The target is sub-microsecond, and these measure whether it is met.

#include "ptl/databento/decoder.hpp"
#include "ptl/execution/quote_router.hpp"

namespace {

[[nodiscard]] ptl::market::Quote bench_quote(ptl::InstrumentId id, ptl::Timestamp ts, double bid,
                                             double ask) {
    auto q = ptl::market::Quote::create(id, ts, ptl::Price{bid}, ptl::Qty{500}, ptl::Price{ask},
                                        ptl::Qty{500});
    return *q;
}

void BM_QuoteBookUpdate(benchmark::State& state) {
    ptl::execution::QuoteBook book;
    ptl::Timestamp t{};
    (void)ptl::parse_timestamp("2024-07-02T15:00:00Z", t);
    double px = 500.0;

    for (auto _ : state) {
        t += std::chrono::milliseconds{1};
        px += 0.0001;
        benchmark::DoNotOptimize(
            book.update(bench_quote(ptl::InstrumentId{0}, t, px - 0.01, px + 0.01)).has_value());
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_QuoteBookUpdate);

/// Reading the current state, including staleness and condition assessment.
void BM_QuoteBookRead(benchmark::State& state) {
    ptl::execution::QuoteBook book;
    ptl::Timestamp t{};
    (void)ptl::parse_timestamp("2024-07-02T15:00:00Z", t);
    book.set_trading_state(ptl::InstrumentId{0}, ptl::execution::TradingState::Trading);
    (void)book.update(bench_quote(ptl::InstrumentId{0}, t, 499.99, 500.01));

    const ptl::Timestamp now = t + std::chrono::seconds{1};
    for (auto _ : state) {
        benchmark::DoNotOptimize(book.state_at(ptl::InstrumentId{0}, now).executable());
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_QuoteBookRead);

/// Quote to MarketState, the conversion on every execution decision.
void BM_QuoteRouting(benchmark::State& state) {
    ptl::execution::QuoteBook book;
    ptl::Timestamp t{};
    (void)ptl::parse_timestamp("2024-07-02T15:00:00Z", t);
    book.set_trading_state(ptl::InstrumentId{0}, ptl::execution::TradingState::Trading);
    (void)book.update(bench_quote(ptl::InstrumentId{0}, t, 499.99, 500.01));

    const ptl::execution::QuoteRouter router{book};
    const ptl::Timestamp now = t + std::chrono::seconds{1};
    for (auto _ : state) {
        benchmark::DoNotOptimize(router.route(ptl::InstrumentId{0}, now).executable);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_QuoteRouting);

/// Quote-aware matching: the full execution path per quote update.
void BM_QuoteAwareExecution(benchmark::State& state) {
    ptl::Timestamp t0{};
    (void)ptl::parse_timestamp("2024-07-02T15:00:00Z", t0);

    for (auto _ : state) {
        state.PauseTiming();
        ptl::SimulatedClock clock{t0};
        ptl::execution::StandardCostModel costs;
        ptl::execution::StandardLatencyModel latency;
        ptl::execution::BrokerSimulator broker{clock, costs, latency, ptl::DeterministicRng{1}};
        ptl::execution::QuoteBook book;
        book.set_trading_state(ptl::InstrumentId{0}, ptl::execution::TradingState::Trading);
        ptl::LifecycleTimes times;
        times.decision_time = clock.now();
        auto order = ptl::oms::Order::market(ptl::oms::OrderId{1}, ptl::InstrumentId{0},
                                             ptl::Side::Buy, ptl::Qty{100}, times);
        (void)broker.submit(*order);
        (void)book.update(bench_quote(ptl::InstrumentId{0}, t0, 499.99, 500.01));
        clock.advance_by(std::chrono::seconds{1});
        const ptl::execution::QuoteRouter router{book};
        const auto routed = router.route(ptl::InstrumentId{0}, clock.now());
        state.ResumeTiming();

        auto fills = broker.on_quote_update(ptl::InstrumentId{0}, routed, clock.now());
        benchmark::DoNotOptimize(fills.has_value());
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_QuoteAwareExecution);

/// Decoder throughput over a realistic multi-record payload.
void BM_QuoteDecoder(benchmark::State& state) {
    const auto n = static_cast<std::size_t>(state.range(0));
    ptl::InstrumentTable instruments;
    ptl::market::SymbolMapper mapper{instruments};
    ptl::market::SymbolMapping m;
    m.raw_symbol = "SPY";
    m.vendor_id = 1001;
    m.valid_from = ptl::Timestamp{ptl::Duration::zero()};
    m.valid_to = ptl::kMaxTimestamp;
    (void)mapper.add(m);

    ptl::Timestamp t{};
    (void)ptl::parse_timestamp("2024-07-02T15:00:00Z", t);

    std::string payload = "[";
    for (std::size_t i = 0; i < n; ++i) {
        if (i != 0) payload += ',';
        const auto ns = (t + std::chrono::seconds{static_cast<long>(i)}).time_since_epoch().count();
        payload += R"({"instrument_id":1001,"ts_recv":)" + std::to_string(ns) +
                   R"(,"bid_px":499990000000,"ask_px":500010000000,)"
                   R"("bid_sz":500,"ask_sz":500})";
    }
    payload += ']';

    ptl::databento::Decoder decoder{mapper};
    for (auto _ : state) {
        decoder.reset();
        auto quotes = decoder.decode_quotes(payload);
        benchmark::DoNotOptimize(quotes.has_value());
    }
    state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(n));
}
BENCHMARK(BM_QuoteDecoder)->Arg(100)->Arg(10000);

/// Replay throughput over an interleaved quote and bar stream.
void BM_QuoteReplayThroughput(benchmark::State& state) {
    ptl::Timestamp t0{};
    (void)ptl::parse_timestamp("2024-07-02T15:00:00Z", t0);

    std::vector<ptl::market::MarketEvent> events;
    for (std::size_t i = 0; i < 20000; ++i) {
        const auto ts = t0 + std::chrono::seconds{static_cast<long>(i)};
        events.emplace_back(bench_quote(ptl::InstrumentId{0}, ts, 499.99, 500.01));
    }

    for (auto _ : state) {
        ptl::SimulatedClock clock;
        auto source = ptl::market::ReplaySource::create(events, &clock);
        std::size_t seen = 0;
        while (auto e = source->next()) ++seen;
        benchmark::DoNotOptimize(seen);
    }
    state.SetItemsProcessed(state.iterations() * 20000);
}
BENCHMARK(BM_QuoteReplayThroughput);

}  // namespace

// ---------------------------------------------------------------------------
// Phase 9: execution algorithms and scheduling
// ---------------------------------------------------------------------------
//
// Planning happens once per parent; child generation happens once per parent
// per market event, which at minute bars across nine names is the figure that
// bounds how long execution may take before the next event.

#include "ptl/algo/executor.hpp"

namespace {

[[nodiscard]] ptl::Timestamp algo_t0() {
    ptl::Timestamp t{};
    (void)ptl::parse_timestamp("2024-07-02T15:00:00Z", t);
    return t;
}

[[nodiscard]] ptl::algo::ExecutionRequest algo_request(double qty = 10000.0) {
    ptl::LifecycleTimes times;
    times.decision_time = algo_t0();
    auto parent = ptl::oms::Order::market(ptl::oms::OrderId{1}, ptl::InstrumentId{0},
                                          ptl::Side::Buy, ptl::Qty{qty}, times);
    ptl::algo::ExecutionRequest r{
        *parent, algo_t0(), algo_t0() + std::chrono::hours{1}, ptl::algo::ExecutionPolicy{},
        60,      {}};
    r.policy.min_clip_size = ptl::Qty{1.0};
    return r;
}

[[nodiscard]] ptl::algo::ExecutionContext algo_context() {
    ptl::algo::ExecutionContext ctx;
    ctx.now = algo_t0() + std::chrono::minutes{30};
    ctx.market.state.bid = ptl::Price{499.90};
    ctx.market.state.ask = ptl::Price{500.10};
    ctx.market.state.bid_size = ptl::Qty{1e6};
    ctx.market.state.ask_size = ptl::Qty{1e6};
    ctx.market.state.has_quote = true;
    ctx.market.executable = true;
    ctx.interval_volume = ptl::Volume{1e6};
    ctx.session_open = algo_t0() - std::chrono::hours{1};
    ctx.session_close = algo_t0() + std::chrono::hours{5};
    return ctx;
}

void BM_TwapSchedule(benchmark::State& state) {
    const auto slices = static_cast<std::size_t>(state.range(0));
    for (auto _ : state) {
        auto s = ptl::algo::ExecutionSchedule::twap(ptl::Qty{10000}, algo_t0(),
                                                    algo_t0() + std::chrono::hours{1}, slices);
        benchmark::DoNotOptimize(s.has_value());
    }
    state.SetItemsProcessed(state.iterations() * state.range(0));
}
BENCHMARK(BM_TwapSchedule)->Arg(10)->Arg(390);

void BM_VwapSchedule(benchmark::State& state) {
    const auto slices = static_cast<std::size_t>(state.range(0));
    const auto profile = ptl::algo::VolumeProfile::uniform(slices);
    for (auto _ : state) {
        auto s = ptl::algo::ExecutionSchedule::vwap(ptl::Qty{10000}, algo_t0(),
                                                    algo_t0() + std::chrono::hours{1}, profile);
        benchmark::DoNotOptimize(s.has_value());
    }
    state.SetItemsProcessed(state.iterations() * state.range(0));
}
BENCHMARK(BM_VwapSchedule)->Arg(10)->Arg(390);

void BM_PovChildGeneration(benchmark::State& state) {
    const ptl::algo::ParticipationAlgorithm pov{0.10};
    const auto request = algo_request();
    auto schedule = pov.plan(request);
    const auto ctx = algo_context();
    ptl::algo::ExecutionProgress progress;
    progress.remaining = ptl::Qty{10000};

    for (auto _ : state) {
        auto child = pov.next_child(request, *schedule, progress, ctx);
        benchmark::DoNotOptimize(child.has_value());
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_PovChildGeneration);

void BM_IcebergRefresh(benchmark::State& state) {
    const ptl::algo::IcebergAlgorithm iceberg{ptl::Qty{100.0}};
    const auto request = algo_request();
    auto schedule = iceberg.plan(request);
    const auto ctx = algo_context();
    ptl::algo::ExecutionProgress progress;
    progress.remaining = ptl::Qty{10000};

    for (auto _ : state) {
        auto child = iceberg.next_child(request, *schedule, progress, ctx);
        benchmark::DoNotOptimize(child.has_value());
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_IcebergRefresh);

/// The schedule-driven path: cumulative target lookup plus every policy cap.
void BM_TwapChildGeneration(benchmark::State& state) {
    const ptl::algo::TwapAlgorithm twap;
    const auto request = algo_request();
    auto schedule = twap.plan(request);
    const auto ctx = algo_context();
    ptl::algo::ExecutionProgress progress;
    progress.remaining = ptl::Qty{10000};

    for (auto _ : state) {
        auto child = twap.next_child(request, *schedule, progress, ctx);
        benchmark::DoNotOptimize(child.has_value());
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_TwapChildGeneration);

/// Cumulative target lookup alone, which every schedule-driven clip performs.
void BM_ScheduleTargetLookup(benchmark::State& state) {
    auto schedule = ptl::algo::ExecutionSchedule::twap(ptl::Qty{10000}, algo_t0(),
                                                       algo_t0() + std::chrono::hours{1}, 390);
    const auto probe = algo_t0() + std::chrono::minutes{37};
    for (auto _ : state) {
        benchmark::DoNotOptimize(schedule->target_by(probe).get());
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_ScheduleTargetLookup);

void BM_ExecutionProgressUpdate(benchmark::State& state) {
    ptl::algo::ExecutionProgress progress;
    progress.remaining = ptl::Qty{1e9};
    for (auto _ : state) {
        progress.filled = progress.filled + ptl::Qty{1.0};
        progress.filled_notional = progress.filled_notional + ptl::Notional{500.0};
        benchmark::DoNotOptimize(progress.completion_ratio(ptl::Qty{1e9}));
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_ExecutionProgressUpdate);

/// A sink that accepts everything, for measuring the executor rather than risk.
class BenchSink final : public ptl::engine::OrderSink {
public:
    [[nodiscard]] ptl::oms::OrderId next_order_id() override {
        return static_cast<ptl::oms::OrderId>(++next_);
    }
    [[nodiscard]] ptl::Result<ptl::oms::OrderId> submit(const ptl::oms::Order& o) override {
        return o.id();
    }
    [[nodiscard]] ptl::Result<bool> cancel(ptl::oms::OrderId) override { return true; }

private:
    std::uint64_t next_ = 1000;
};

/// One executor pass: what runs per instrument per market event.
void BM_ExecutorOnMarket(benchmark::State& state) {
    const auto n = static_cast<std::size_t>(state.range(0));
    ptl::algo::Executor executor{std::make_unique<ptl::algo::TwapAlgorithm>()};
    BenchSink sink;

    for (std::size_t i = 0; i < n; ++i) {
        ptl::LifecycleTimes times;
        times.decision_time = algo_t0();
        auto parent =
            ptl::oms::Order::market(static_cast<ptl::oms::OrderId>(i + 1), ptl::InstrumentId{0},
                                    ptl::Side::Buy, ptl::Qty{10000}, times);
        ptl::algo::ExecutionRequest r{
            *parent, algo_t0(), algo_t0() + std::chrono::hours{1}, ptl::algo::ExecutionPolicy{},
            60,      {}};
        r.policy.min_clip_size = ptl::Qty{1.0};
        (void)executor.submit(r, static_cast<ptl::oms::OrderId>(i + 1), ptl::Price{500.0});
    }

    const auto ctx = algo_context();
    for (auto _ : state) {
        auto emitted = executor.on_market(ptl::InstrumentId{0}, ctx, sink);
        benchmark::DoNotOptimize(emitted.has_value());
    }
    state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(n));
}
BENCHMARK(BM_ExecutorOnMarket)->Arg(1)->Arg(9);

/// End-to-end: plan a parent and work it to completion.
void BM_ExecutionEndToEnd(benchmark::State& state) {
    for (auto _ : state) {
        ptl::algo::Executor executor{std::make_unique<ptl::algo::TwapAlgorithm>()};
        BenchSink sink;
        (void)executor.submit(algo_request(), ptl::oms::OrderId{1}, ptl::Price{500.0});

        auto ctx = algo_context();
        for (int minute = 0; minute < 60; ++minute) {
            ctx.now = algo_t0() + std::chrono::minutes{minute};
            (void)executor.on_market(ptl::InstrumentId{0}, ctx, sink);
        }
        benchmark::DoNotOptimize(executor.content_hash());
    }
    state.SetItemsProcessed(state.iterations() * 60);
}
BENCHMARK(BM_ExecutionEndToEnd);

}  // namespace

// ---------------------------------------------------------------------------
// Phase 10: analytics and reporting
// ---------------------------------------------------------------------------
//
// Drawdown and statistics updates run once per equity observation, so they sit
// on the live path if a risk check consults them. Report generation runs once
// per run and is measured because a slow report discourages producing one.

#include "ptl/report/generator.hpp"

namespace {

[[nodiscard]] std::vector<ptl::portfolio::EquityPoint> bench_curve(std::size_t n) {
    std::vector<ptl::portfolio::EquityPoint> out;
    out.reserve(n);
    ptl::Timestamp t{};
    (void)ptl::parse_timestamp("2024-01-02T20:00:00Z", t);
    double equity = 1'000'000.0;
    for (std::size_t i = 0; i < n; ++i) {
        equity *= 1.0 + std::sin(static_cast<double>(i) * 0.07) * 0.004;
        ptl::portfolio::EquityPoint p;
        p.ts = t;
        p.equity = ptl::Notional{equity};
        p.cash = ptl::Notional{equity * 0.4};
        p.gross_exposure = ptl::Notional{equity * 0.8};
        p.net_exposure = ptl::Notional{equity * 0.2};
        p.turnover = ptl::Notional{static_cast<double>(i + 1) * 25'000.0};
        out.push_back(p);
        t += std::chrono::hours{24};
    }
    return out;
}

[[nodiscard]] std::vector<ptl::accounting::Trade> bench_trades(std::size_t n) {
    std::vector<ptl::accounting::Trade> out;
    out.reserve(n);
    ptl::Timestamp t{};
    (void)ptl::parse_timestamp("2024-01-02T15:00:00Z", t);
    for (std::size_t i = 0; i < n; ++i) {
        ptl::accounting::Trade trade;
        trade.instrument = static_cast<ptl::InstrumentId>(i % 9);
        trade.opened = t;
        trade.closed = t + std::chrono::hours{2};
        trade.quantity = ptl::Qty{100};
        trade.entry_price = ptl::Price{500.0};
        trade.exit_price = ptl::Price{501.0};
        trade.gross_pnl = ptl::Notional{i % 3 == 0 ? -80.0 : 120.0};
        trade.costs = ptl::Notional{3.0};
        out.push_back(trade);
        t += std::chrono::hours{6};
    }
    return out;
}

void BM_DrawdownUpdate(benchmark::State& state) {
    ptl::analytics::DrawdownTracker tracker;
    ptl::Timestamp t{};
    (void)ptl::parse_timestamp("2024-01-02T20:00:00Z", t);
    double equity = 1'000'000.0;
    for (auto _ : state) {
        t += std::chrono::seconds{60};
        equity *= 1.0001;
        benchmark::DoNotOptimize(tracker.update(t, ptl::Notional{equity}).has_value());
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_DrawdownUpdate);

void BM_StatisticsUpdate(benchmark::State& state) {
    ptl::analytics::StatisticsAccumulator acc;
    double x = 0.0;
    for (auto _ : state) {
        x += 1e-6;
        acc.update(x);
        benchmark::DoNotOptimize(acc.stdev());
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_StatisticsUpdate);

void BM_RollingDrawdown(benchmark::State& state) {
    const auto curve = bench_curve(static_cast<std::size_t>(state.range(0)));
    for (auto _ : state) {
        auto r = ptl::analytics::rolling_max_drawdown(curve, 60);
        benchmark::DoNotOptimize(r.has_value());
    }
    state.SetItemsProcessed(state.iterations() * state.range(0));
}
BENCHMARK(BM_RollingDrawdown)->Arg(252)->Arg(2520);

void BM_RiskAnalysis(benchmark::State& state) {
    const auto n = static_cast<std::size_t>(state.range(0));
    std::vector<double> returns(n);
    std::vector<double> benchmark_series(n);
    for (std::size_t i = 0; i < n; ++i) {
        benchmark_series[i] = std::sin(static_cast<double>(i) * 0.11) * 0.01;
        returns[i] = benchmark_series[i] * 1.3 + std::cos(static_cast<double>(i)) * 0.002;
    }
    const ptl::analytics::RiskAnalyzer analyzer;
    for (auto _ : state) {
        auto m = analyzer.analyze(returns, benchmark_series, 0.12);
        benchmark::DoNotOptimize(m.has_value());
    }
    state.SetItemsProcessed(state.iterations() * state.range(0));
}
BENCHMARK(BM_RiskAnalysis)->Arg(252)->Arg(2520);

void BM_PortfolioSnapshot(benchmark::State& state) {
    const auto curve = bench_curve(static_cast<std::size_t>(state.range(0)));
    const ptl::analytics::PerformanceAnalyzer analyzer;
    for (auto _ : state) {
        auto s = analyzer.snapshots(curve, ptl::analytics::SnapshotFrequency::Daily);
        benchmark::DoNotOptimize(s.has_value());
    }
    state.SetItemsProcessed(state.iterations() * state.range(0));
}
BENCHMARK(BM_PortfolioSnapshot)->Arg(252)->Arg(2520);

void BM_TradeAttribution(benchmark::State& state) {
    const auto trades = bench_trades(static_cast<std::size_t>(state.range(0)));
    ptl::analytics::AttributionAnalyzer analyzer;
    for (std::uint32_t i = 0; i < 9; ++i) {
        analyzer.map_instrument(static_cast<ptl::InstrumentId>(i),
                                {static_cast<ptl::InstrumentId>(i),
                                 static_cast<std::int32_t>(i % 3), "strategy", "twap"});
    }
    for (auto _ : state) {
        auto t = analyzer.by_instrument(trades);
        benchmark::DoNotOptimize(t.has_value());
    }
    state.SetItemsProcessed(state.iterations() * state.range(0));
}
BENCHMARK(BM_TradeAttribution)->Arg(100)->Arg(10000);

void BM_TradeAnalysis(benchmark::State& state) {
    const auto trades = bench_trades(static_cast<std::size_t>(state.range(0)));
    const ptl::analytics::TradeAnalyzer analyzer;
    for (auto _ : state) {
        auto s = analyzer.analyze(trades);
        benchmark::DoNotOptimize(s.has_value());
    }
    state.SetItemsProcessed(state.iterations() * state.range(0));
}
BENCHMARK(BM_TradeAnalysis)->Arg(100)->Arg(10000);

/// The whole analytics pass: what a run pays once at the end.
void BM_EndToEndAnalytics(benchmark::State& state) {
    const auto curve = bench_curve(1000);
    const auto trades = bench_trades(500);
    ptl::analytics::AttributionAnalyzer attribution;
    for (std::uint32_t i = 0; i < 9; ++i) {
        attribution.map_instrument(static_cast<ptl::InstrumentId>(i),
                                   {static_cast<ptl::InstrumentId>(i),
                                    static_cast<std::int32_t>(i % 3), "strategy", "twap"});
    }
    const ptl::analytics::PerformanceAnalyzer analyzer;
    for (auto _ : state) {
        auto report = analyzer.analyze(curve, trades, {}, attribution);
        benchmark::DoNotOptimize(report.has_value());
    }
    state.SetItemsProcessed(state.iterations() * 1000);
}
BENCHMARK(BM_EndToEndAnalytics);

void BM_ReportGeneration(benchmark::State& state) {
    const auto curve = bench_curve(1000);
    const auto trades = bench_trades(500);
    ptl::analytics::AttributionAnalyzer attribution;
    for (std::uint32_t i = 0; i < 9; ++i) {
        attribution.map_instrument(static_cast<ptl::InstrumentId>(i),
                                   {static_cast<ptl::InstrumentId>(i),
                                    static_cast<std::int32_t>(i % 3), "strategy", "twap"});
    }
    const ptl::analytics::PerformanceAnalyzer analyzer;
    auto report = analyzer.analyze(curve, trades, {}, attribution);

    const auto format = static_cast<ptl::report::Format>(state.range(0));
    const ptl::report::ReportGenerator generator;
    for (auto _ : state) {
        auto text = generator.generate(*report, format);
        benchmark::DoNotOptimize(text.has_value());
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_ReportGeneration)
    ->Arg(static_cast<int>(ptl::report::Format::Csv))
    ->Arg(static_cast<int>(ptl::report::Format::Json))
    ->Arg(static_cast<int>(ptl::report::Format::Markdown));

}  // namespace

// ---------------------------------------------------------------------------
// Phase 11: portfolio optimization
// ---------------------------------------------------------------------------
//
// Optimization runs once per rebalance, not per bar, so microseconds are
// acceptable here in a way they would not be on the quote path. What matters is
// how the cost scales with universe size, since that is what decides whether a
// 500-name book is tractable.

#include "ptl/optimization/optimizer.hpp"

namespace {

[[nodiscard]] std::vector<double> opt_returns(std::size_t rows, std::size_t assets) {
    std::vector<double> out;
    out.reserve(rows * assets);
    for (std::size_t r = 0; r < rows; ++r) {
        for (std::size_t c = 0; c < assets; ++c) {
            out.push_back(std::sin(static_cast<double>(r) * 0.3 + static_cast<double>(c)) * 0.01);
        }
    }
    return out;
}

[[nodiscard]] ptl::optimization::SymmetricMatrix opt_cov(std::size_t n) {
    ptl::optimization::SymmetricMatrix cov{n};
    for (std::size_t i = 0; i < n; ++i) {
        for (std::size_t j = i; j < n; ++j) {
            const double v = i == j ? 0.02 + 0.01 * static_cast<double>(i % 5) : 0.004;
            cov.set_symmetric(i, j, v);
        }
    }
    return cov;
}

[[nodiscard]] ptl::optimization::OptimizationInput opt_input(std::size_t n) {
    ptl::optimization::OptimizationInput in;
    (void)ptl::parse_timestamp("2024-07-02T15:00:00Z", in.as_of);
    for (std::size_t i = 0; i < n; ++i) {
        in.instruments.push_back(static_cast<ptl::InstrumentId>(i));
    }
    in.covariance = opt_cov(n);
    in.expected_returns.reserve(n);
    in.volatilities.reserve(n);
    in.signals.reserve(n);
    for (std::size_t i = 0; i < n; ++i) {
        in.expected_returns.push_back(0.01 + 0.001 * static_cast<double>(i % 7));
        in.volatilities.push_back(std::sqrt(in.covariance.at(i, i)));
        in.signals.push_back(std::sin(static_cast<double>(i)) * 0.5);
    }
    return in;
}

[[nodiscard]] ptl::optimization::OptimizerConfig opt_config() {
    ptl::optimization::OptimizerConfig cfg;
    cfg.constraints = ptl::optimization::ConstraintSet::long_only(0.20);
    cfg.max_iterations = 200;
    return cfg;
}

void BM_CovarianceBuild(benchmark::State& state) {
    const auto assets = static_cast<std::size_t>(state.range(0));
    const std::size_t rows = 500;
    const auto observations = opt_returns(rows, assets);

    ptl::optimization::CovarianceConfig cfg;
    cfg.method = ptl::optimization::CovarianceMethod::Shrinkage;
    cfg.min_observations_ratio = 0.0;

    for (auto _ : state) {
        ptl::optimization::CovarianceEstimator estimator{cfg};
        auto cov = estimator.estimate(observations, rows, assets);
        benchmark::DoNotOptimize(cov.has_value());
    }
    state.SetItemsProcessed(state.iterations() * state.range(0));
}
BENCHMARK(BM_CovarianceBuild)->Arg(10)->Arg(50);

/// PSD repair is the expensive part of covariance construction: a full Jacobi
/// eigendecomposition, cubic in the universe size.
void BM_PsdEnforcement(benchmark::State& state) {
    const auto n = static_cast<std::size_t>(state.range(0));
    auto cov = opt_cov(n);
    // Make it indefinite so the repair path actually runs.
    cov.set_symmetric(0, 1, 10.0);

    for (auto _ : state) {
        auto fixed = ptl::optimization::CovarianceEstimator::enforce_psd(cov, 1e-10);
        benchmark::DoNotOptimize(fixed.has_value());
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_PsdEnforcement)->Arg(10)->Arg(50);

void BM_EqualWeight(benchmark::State& state) {
    const ptl::optimization::EqualWeightOptimizer optimizer{opt_config()};
    const auto input = opt_input(static_cast<std::size_t>(state.range(0)));
    for (auto _ : state) {
        auto r = optimizer.optimize(input);
        benchmark::DoNotOptimize(r.has_value());
    }
    state.SetItemsProcessed(state.iterations() * state.range(0));
}
BENCHMARK(BM_EqualWeight)->Arg(50);

void BM_MeanVariance(benchmark::State& state) {
    const ptl::optimization::MeanVarianceOptimizer optimizer{opt_config()};
    const auto input = opt_input(static_cast<std::size_t>(state.range(0)));
    for (auto _ : state) {
        auto r = optimizer.optimize(input);
        benchmark::DoNotOptimize(r.has_value());
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_MeanVariance)->Arg(10)->Arg(50);

void BM_RiskParity(benchmark::State& state) {
    const ptl::optimization::RiskParityOptimizer optimizer{opt_config()};
    const auto input = opt_input(static_cast<std::size_t>(state.range(0)));
    for (auto _ : state) {
        auto r = optimizer.optimize(input);
        benchmark::DoNotOptimize(r.has_value());
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_RiskParity)->Arg(10)->Arg(50);

void BM_MaxSharpe(benchmark::State& state) {
    const ptl::optimization::MaximumSharpeOptimizer optimizer{opt_config()};
    const auto input = opt_input(static_cast<std::size_t>(state.range(0)));
    for (auto _ : state) {
        auto r = optimizer.optimize(input);
        benchmark::DoNotOptimize(r.has_value());
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_MaxSharpe)->Arg(10)->Arg(50);

void BM_TargetVolatility(benchmark::State& state) {
    const ptl::optimization::TargetVolatilityOptimizer optimizer{opt_config()};
    const auto input = opt_input(static_cast<std::size_t>(state.range(0)));
    for (auto _ : state) {
        auto r = optimizer.optimize(input);
        benchmark::DoNotOptimize(r.has_value());
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_TargetVolatility)->Arg(10)->Arg(50);

/// Constraint projection alone: it runs inside every solver iteration, so its
/// cost is multiplied by the iteration count.
void BM_ConstraintProjection(benchmark::State& state) {
    const auto n = static_cast<std::size_t>(state.range(0));
    auto input = opt_input(n);
    input.sectors.reserve(n);
    for (std::size_t i = 0; i < n; ++i) {
        input.sectors.push_back(static_cast<std::int32_t>(i % 5));
    }
    const ptl::optimization::ConstraintProjector projector{
        ptl::optimization::ConstraintSet::long_only(0.20)};

    for (auto _ : state) {
        std::vector<double> w(n, 1.0 / static_cast<double>(n));
        benchmark::DoNotOptimize(projector.project(w, input, nullptr).has_value());
    }
    state.SetItemsProcessed(state.iterations() * state.range(0));
}
BENCHMARK(BM_ConstraintProjection)->Arg(50);

/// End to end: estimate the risk model, then optimize against it. What a
/// rebalance actually pays.
void BM_EndToEndOptimization(benchmark::State& state) {
    const auto assets = static_cast<std::size_t>(state.range(0));
    const std::size_t rows = 500;
    const auto observations = opt_returns(rows, assets);

    ptl::optimization::CovarianceConfig cov_cfg;
    cov_cfg.method = ptl::optimization::CovarianceMethod::Shrinkage;
    cov_cfg.min_observations_ratio = 0.0;

    const ptl::optimization::MaximumSharpeOptimizer optimizer{opt_config()};

    for (auto _ : state) {
        ptl::optimization::CovarianceEstimator estimator{cov_cfg};
        auto cov = estimator.estimate(observations, rows, assets);
        if (!cov) continue;

        auto input = opt_input(assets);
        input.covariance = *cov;
        auto result = optimizer.optimize(input);
        benchmark::DoNotOptimize(result.has_value());
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_EndToEndOptimization)->Arg(10)->Arg(50);

}  // namespace

// ---------------------------------------------------------------------------
// Phase 12: attribution, rolling analytics and reporting
// ---------------------------------------------------------------------------
//
// These run once per report, not per event, so milliseconds are acceptable in
// a way they would not be on the quote path. What matters is SCALING: rolling
// analytics are incremental and must stay O(n) in the series length rather than
// O(n*w), and the benchmarks below are sized to show that.

#include "ptl/attribution/pnl.hpp"
#include "ptl/reporting/reports.hpp"

namespace {

[[nodiscard]] std::vector<ptl::Timestamp> attr_timestamps(std::size_t n) {
    std::vector<ptl::Timestamp> out;
    out.reserve(n);
    ptl::Timestamp t{};
    (void)ptl::parse_timestamp("2024-01-02T20:00:00Z", t);
    for (std::size_t i = 0; i < n; ++i) {
        out.push_back(t);
        t += std::chrono::hours{24};
    }
    return out;
}

[[nodiscard]] std::vector<double> attr_returns(std::size_t n, double phase = 0.0) {
    std::vector<double> out;
    out.reserve(n);
    for (std::size_t i = 0; i < n; ++i) {
        out.push_back(std::sin((static_cast<double>(i) + phase) * 0.23) * 0.012 + 0.0002);
    }
    return out;
}

[[nodiscard]] std::vector<ptl::portfolio::EquityPoint> attr_curve(std::size_t n) {
    std::vector<ptl::portfolio::EquityPoint> out;
    out.reserve(n);
    const auto ts = attr_timestamps(n);
    double equity = 1'000'000.0;
    double realized = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        equity *= 1.0 + std::sin(static_cast<double>(i) * 0.19) * 0.003;
        realized += 250.0;
        ptl::portfolio::EquityPoint p;
        p.ts = ts[i];
        p.equity = ptl::Notional{equity};
        p.cash = ptl::Notional{equity * 0.35};
        p.realized_pnl = ptl::Notional{realized};
        p.unrealized_pnl = ptl::Notional{equity * 0.01};
        p.gross_exposure = ptl::Notional{equity * 0.85};
        p.net_exposure = ptl::Notional{equity * 0.30};
        p.turnover = ptl::Notional{static_cast<double>(i + 1) * 20'000.0};
        out.push_back(p);
    }
    return out;
}

/// Incremental rolling volatility. The scaling claim of the whole module: this
/// must grow linearly in the series length, not in length times window.
void BM_RollingVolatility(benchmark::State& state) {
    const auto n = static_cast<std::size_t>(state.range(0));
    const auto ts = attr_timestamps(n);
    const auto returns = attr_returns(n);

    ptl::analytics::RollingConfig cfg;
    cfg.window = 60;
    const ptl::analytics::RollingAnalyzer analyzer{cfg};

    for (auto _ : state) {
        auto series = analyzer.volatility(ts, returns);
        benchmark::DoNotOptimize(series.has_value());
    }
    state.SetItemsProcessed(state.iterations() * state.range(0));
}
BENCHMARK(BM_RollingVolatility)->Arg(1000)->Arg(10000);

void BM_RollingSharpe(benchmark::State& state) {
    const auto n = static_cast<std::size_t>(state.range(0));
    const auto ts = attr_timestamps(n);
    const auto returns = attr_returns(n);

    ptl::analytics::RollingConfig cfg;
    cfg.window = 60;
    const ptl::analytics::RollingAnalyzer analyzer{cfg};

    for (auto _ : state) {
        auto series = analyzer.sharpe(ts, returns);
        benchmark::DoNotOptimize(series.has_value());
    }
    state.SetItemsProcessed(state.iterations() * state.range(0));
}
BENCHMARK(BM_RollingSharpe)->Arg(10000);

void BM_RollingBetaSeries(benchmark::State& state) {
    const auto n = static_cast<std::size_t>(state.range(0));
    const auto ts = attr_timestamps(n);
    const auto returns = attr_returns(n);
    const auto benchmark_series = attr_returns(n, 3.0);

    ptl::analytics::RollingConfig cfg;
    cfg.window = 60;
    const ptl::analytics::RollingAnalyzer analyzer{cfg};

    for (auto _ : state) {
        auto series = analyzer.beta(ts, returns, benchmark_series);
        benchmark::DoNotOptimize(series.has_value());
    }
    state.SetItemsProcessed(state.iterations() * state.range(0));
}
BENCHMARK(BM_RollingBetaSeries)->Arg(10000);

/// VaR genuinely needs order statistics, so it is O(n log w) rather than O(n).
/// Benchmarked separately to keep that cost visible rather than averaged into
/// the incremental statistics.
void BM_RollingVaR(benchmark::State& state) {
    const auto n = static_cast<std::size_t>(state.range(0));
    const auto ts = attr_timestamps(n);
    const auto returns = attr_returns(n);

    ptl::analytics::RollingConfig cfg;
    cfg.window = 60;
    const ptl::analytics::RollingAnalyzer analyzer{cfg};

    for (auto _ : state) {
        auto series = analyzer.value_at_risk(ts, returns);
        benchmark::DoNotOptimize(series.has_value());
    }
    state.SetItemsProcessed(state.iterations() * state.range(0));
}
BENCHMARK(BM_RollingVaR)->Arg(10000);

void BM_PnlAttribution(benchmark::State& state) {
    const auto n = static_cast<std::size_t>(state.range(0));
    const auto curve = attr_curve(n);

    ptl::attribution::FinancingRates rates;
    rates.borrow_rate = 0.04;
    rates.margin_rate = 0.055;
    rates.cash_rate = 0.03;
    rates.opportunity_rate = 0.07;
    const ptl::attribution::PnlAttributor attributor{rates};

    for (auto _ : state) {
        auto series = attributor.decompose_series(curve, {});
        benchmark::DoNotOptimize(series.has_value());
    }
    state.SetItemsProcessed(state.iterations() * state.range(0));
}
BENCHMARK(BM_PnlAttribution)->Arg(1000)->Arg(10000);

void BM_FactorContribution(benchmark::State& state) {
    const auto n = static_cast<std::size_t>(state.range(0));
    const auto portfolio = attr_returns(n);
    const auto benchmark_series = attr_returns(n, 2.0);

    for (auto _ : state) {
        auto contribution =
            ptl::attribution::PnlAttributor::factor_contribution(portfolio, benchmark_series);
        benchmark::DoNotOptimize(contribution.has_value());
    }
    state.SetItemsProcessed(state.iterations() * state.range(0));
}
BENCHMARK(BM_FactorContribution)->Arg(10000);

void BM_TradeExecutionAttribution(benchmark::State& state) {
    const auto n = static_cast<std::size_t>(state.range(0));

    std::vector<ptl::attribution::TradeExecutionQuality> trades;
    trades.reserve(n);
    ptl::Timestamp t{};
    (void)ptl::parse_timestamp("2024-01-02T15:00:00Z", t);
    for (std::size_t i = 0; i < n; ++i) {
        ptl::attribution::TradeExecutionQuality trade;
        trade.instrument = static_cast<ptl::InstrumentId>(i % 9);
        trade.side = i % 2 == 0 ? ptl::Side::Buy : ptl::Side::Sell;
        trade.quantity = ptl::Qty{100.0 + static_cast<double>(i % 50)};
        trade.implementation_shortfall = ptl::Bps{static_cast<double>(i % 20) - 5.0};
        trade.delay_cost = ptl::Bps{static_cast<double>(i % 7)};
        trade.execution_cost = ptl::Bps{static_cast<double>(i % 11) - 3.0};
        trade.realized_edge = ptl::Bps{static_cast<double>(i % 30)};
        trade.decision_time = t;
        trade.first_fill_time = t + std::chrono::seconds{static_cast<long>(i % 600)};
        trade.holding_period = std::chrono::minutes{static_cast<long>(i % 90)};
        trade.fill_count = 1 + i % 4;
        trades.push_back(trade);
        t += std::chrono::minutes{1};
    }

    const ptl::attribution::ExecutionQualityAnalyzer analyzer;
    for (auto _ : state) {
        auto summary = analyzer.summarize(trades);
        benchmark::DoNotOptimize(summary.has_value());
    }
    state.SetItemsProcessed(state.iterations() * state.range(0));
}
BENCHMARK(BM_TradeExecutionAttribution)->Arg(1000)->Arg(10000);

void BM_SignalDecay(benchmark::State& state) {
    const auto n = static_cast<std::size_t>(state.range(0));
    std::vector<ptl::attribution::TradeExecutionQuality> trades;
    trades.reserve(n);
    ptl::Timestamp t{};
    (void)ptl::parse_timestamp("2024-01-02T15:00:00Z", t);
    for (std::size_t i = 0; i < n; ++i) {
        ptl::attribution::TradeExecutionQuality trade;
        trade.decision_time = t;
        trade.first_fill_time = t + std::chrono::seconds{static_cast<long>(i % 1200)};
        trade.realized_edge = ptl::Bps{static_cast<double>(i % 40)};
        trade.quantity = ptl::Qty{100.0};
        trades.push_back(trade);
    }
    const std::vector<ptl::Duration> horizons{std::chrono::seconds{30}, std::chrono::minutes{5},
                                              std::chrono::minutes{20}};

    const ptl::attribution::ExecutionQualityAnalyzer analyzer;
    for (auto _ : state) {
        auto profile = analyzer.signal_decay(trades, horizons);
        benchmark::DoNotOptimize(profile.has_value());
    }
    state.SetItemsProcessed(state.iterations() * state.range(0));
}
BENCHMARK(BM_SignalDecay)->Arg(10000);

/// Report assembly plus JSON serialization: what a scheduled report pays.
void BM_ReportGenerationJson(benchmark::State& state) {
    ptl::analytics::PerformanceReport performance;
    performance.run_id = "bench";
    performance.strategy_name = "bench_strategy";
    (void)ptl::parse_timestamp("2024-01-02T20:00:00Z", performance.period_begin);
    (void)ptl::parse_timestamp("2025-01-02T20:00:00Z", performance.period_end);
    performance.initial_equity = ptl::Notional{1'000'000.0};
    performance.final_equity = ptl::Notional{1'180'000.0};

    const auto ts = attr_timestamps(500);
    double equity = 1'000'000.0;
    for (std::size_t i = 0; i < ts.size(); ++i) {
        ptl::analytics::PerformanceSnapshot snap;
        snap.ts = ts[i];
        equity *= 1.0003;
        snap.equity = ptl::Notional{equity};
        snap.period_return = 0.0003;
        snap.drawdown = static_cast<double>(i % 11) * 0.002;
        snap.exposure.gross_leverage = 0.9;
        snap.exposure.net_leverage = 0.35;
        performance.daily_snapshots.push_back(snap);
        if (i % 21 == 0) performance.monthly_snapshots.push_back(snap);
    }

    ptl::reporting::ReportConfig cfg;
    cfg.max_series_points = 500;
    const ptl::reporting::ReportBuilder builder{cfg};

    for (auto _ : state) {
        auto report = builder.build(ptl::reporting::ReportKind::Daily, performance);
        if (!report) continue;
        auto viz = builder.build_visualization(performance, {});
        if (viz) report->visualization = std::move(*viz);
        auto json = builder.to_json(*report);
        benchmark::DoNotOptimize(json.has_value());
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_ReportGenerationJson);

}  // namespace

// ---------------------------------------------------------------------------
// Phase 13: strategy lifecycle and research infrastructure
// ---------------------------------------------------------------------------
//
// These run at experiment definition time, not per event, so microseconds are
// acceptable. What matters is that a platform holding HUNDREDS of strategies
// and thousands of experiments still answers a lookup or a leaderboard query
// quickly enough to be used interactively.

#include "ptl/experiment/experiment.hpp"

namespace {

[[nodiscard]] ptl::strategy::StrategyDescriptor bench_descriptor(std::size_t i) {
    ptl::strategy::StrategyDescriptor d;
    auto id = ptl::strategy::StrategyId::create("strategy_" + std::to_string(i));
    d.id = *id;
    d.version = ptl::strategy::StrategyVersion{1, static_cast<std::uint32_t>(i % 10), 0};
    d.state = ptl::strategy::StrategyState::Research;
    d.metadata.author = "bench";
    d.metadata.tags = {"intraday", "equities"};
    d.parameters.push_back({"lookback", "int", "bars", true, "20"});
    d.parameters.push_back({"threshold", "double", "entry", false, "0.5"});
    return d;
}

[[nodiscard]] ptl::storage::DatasetVersion bench_dataset(std::size_t i) {
    ptl::storage::DatasetVersion d;
    d.dataset_id = "dataset_" + std::to_string(i);
    d.version = 1;
    d.content_checksum = ptl::storage::Checksum::of("content_" + std::to_string(i));
    d.feature_schema.fields.push_back({"ret_1m", "double", 1});
    d.normalization_version = "zscore_v1";
    return d;
}

void BM_StrategyRegistration(benchmark::State& state) {
    const auto n = static_cast<std::size_t>(state.range(0));
    std::vector<ptl::strategy::StrategyDescriptor> descriptors;
    descriptors.reserve(n);
    for (std::size_t i = 0; i < n; ++i) descriptors.push_back(bench_descriptor(i));

    for (auto _ : state) {
        ptl::strategy::StrategyRegistry registry;
        for (const auto& d : descriptors) {
            benchmark::DoNotOptimize(registry.register_strategy(d).has_value());
        }
    }
    state.SetItemsProcessed(state.iterations() * state.range(0));
}
BENCHMARK(BM_StrategyRegistration)->Arg(100)->Arg(1000);

/// Lookup on a catalogue the size a real platform carries.
void BM_StrategyLookup(benchmark::State& state) {
    const auto n = static_cast<std::size_t>(state.range(0));
    ptl::strategy::StrategyRegistry registry;
    for (std::size_t i = 0; i < n; ++i) {
        (void)registry.register_strategy(bench_descriptor(i));
    }
    const auto target = bench_descriptor(n / 2);

    for (auto _ : state) {
        benchmark::DoNotOptimize(registry.find(target.id, target.version));
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_StrategyLookup)->Arg(1000);

void BM_DatasetLookup(benchmark::State& state) {
    const auto n = static_cast<std::size_t>(state.range(0));
    ptl::storage::DatasetRegistry registry;
    for (std::size_t i = 0; i < n; ++i) {
        (void)registry.register_dataset(bench_dataset(i));
    }
    const std::string target = "dataset_" + std::to_string(n / 2);

    for (auto _ : state) {
        benchmark::DoNotOptimize(registry.contains(target, 1));
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_DatasetLookup)->Arg(1000);

void BM_ModelLookup(benchmark::State& state) {
    const auto n = static_cast<std::size_t>(state.range(0));
    ptl::storage::DatasetRegistry datasets;
    (void)datasets.register_dataset(bench_dataset(0));
    ptl::storage::ModelRegistry models{datasets};

    for (std::size_t i = 0; i < n; ++i) {
        ptl::storage::ModelMetadata model;
        model.model_id = "model_" + std::to_string(i % 20);
        model.version = static_cast<std::uint32_t>(i / 20 + 1);
        model.kind = "ridge";
        model.dataset_id = "dataset_0";
        model.dataset_version = 1;
        model.status = ptl::storage::ModelStatus::Trained;
        (void)models.register_model(model);
    }
    (void)models.promote("model_5", 1, ptl::Timestamp{});

    for (auto _ : state) {
        benchmark::DoNotOptimize(models.champion("model_5"));
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_ModelLookup)->Arg(1000);

/// Config fingerprinting runs on every experiment definition and every resume
/// check, so it sits on the interactive path.
void BM_ConfigFingerprint(benchmark::State& state) {
    ptl::experiment::ExperimentConfig config;
    config.experiment_id = "bench";
    config.strategy_id = *ptl::strategy::StrategyId::create("alpha");
    config.strategy_version = ptl::strategy::StrategyVersion{1, 0, 0};
    config.dataset_id = "equities";
    config.dataset_version = 1;
    config.seed = 42;
    for (int i = 0; i < 20; ++i) {
        config.parameters["param_" + std::to_string(i)] = std::to_string(i * 1.5);
    }

    for (auto _ : state) {
        benchmark::DoNotOptimize(config.fingerprint());
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_ConfigFingerprint);

void BM_ExperimentCreation(benchmark::State& state) {
    ptl::strategy::StrategyRegistry strategies;
    (void)strategies.register_strategy(bench_descriptor(0));
    ptl::storage::DatasetRegistry datasets;
    (void)datasets.register_dataset(bench_dataset(0));

    const ptl::experiment::ExperimentRunner runner{strategies, datasets};

    ptl::experiment::ExperimentConfig config;
    config.experiment_id = "bench";
    config.strategy_id = *ptl::strategy::StrategyId::create("strategy_0");
    config.strategy_version = ptl::strategy::StrategyVersion{1, 0, 0};
    config.dataset_id = "dataset_0";
    config.dataset_version = 1;
    config.seed = 42;
    config.parameters["lookback"] = "30";

    for (auto _ : state) {
        auto prepared = runner.prepare(config);
        benchmark::DoNotOptimize(prepared.has_value());
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_ExperimentCreation);

[[nodiscard]] std::vector<ptl::experiment::ExperimentResult> bench_results(std::size_t n) {
    std::vector<ptl::experiment::ExperimentResult> out;
    out.reserve(n);
    for (std::size_t i = 0; i < n; ++i) {
        ptl::experiment::ExperimentResult r;
        r.experiment_id = "exp_" + std::to_string(i);
        r.status = ptl::experiment::ExperimentStatus::Completed;
        ptl::analytics::PerformanceReport performance;
        performance.metrics.sharpe = std::sin(static_cast<double>(i) * 0.1) * 2.0;
        performance.metrics.sortino = performance.metrics.sharpe * 1.2;
        performance.metrics.calmar = performance.metrics.sharpe * 0.8;
        performance.max_drawdown = 0.05 + static_cast<double>(i % 20) * 0.01;
        performance.turnover.annualized_turnover = 5.0 + static_cast<double>(i % 50);
        r.performance = std::move(performance);
        out.push_back(std::move(r));
    }
    return out;
}

/// Comparison ranks every experiment on every metric, so it is O(m * n log n).
void BM_ComparisonGeneration(benchmark::State& state) {
    const auto results = bench_results(static_cast<std::size_t>(state.range(0)));
    const ptl::experiment::ExperimentComparison comparison;

    for (auto _ : state) {
        auto report = comparison.compare(results);
        benchmark::DoNotOptimize(report.has_value());
    }
    state.SetItemsProcessed(state.iterations() * state.range(0));
}
BENCHMARK(BM_ComparisonGeneration)->Arg(100)->Arg(1000);

void BM_LeaderboardGeneration(benchmark::State& state) {
    const auto n = static_cast<std::size_t>(state.range(0));
    const auto results = bench_results(n);

    std::vector<ptl::experiment::Experiment> experiments;
    experiments.reserve(n);
    for (std::size_t i = 0; i < n; ++i) {
        ptl::experiment::Experiment e;
        e.config.experiment_id = results[i].experiment_id;
        e.config.strategy_id = *ptl::strategy::StrategyId::create("alpha");
        e.status = ptl::experiment::ExperimentStatus::Completed;
        e.result = results[i];
        experiments.push_back(std::move(e));
    }

    for (auto _ : state) {
        auto board = ptl::experiment::LeaderboardBuilder::build(experiments, "sharpe");
        benchmark::DoNotOptimize(board.has_value());
    }
    state.SetItemsProcessed(state.iterations() * state.range(0));
}
BENCHMARK(BM_LeaderboardGeneration)->Arg(100)->Arg(1000);

/// Checkpoint restore: parse plus checksum verification. On the recovery path,
/// where a slow restore delays every resumed experiment.
void BM_CheckpointRestore(benchmark::State& state) {
    ptl::experiment::ExperimentSnapshot snapshot;
    snapshot.experiment_id = "bench_experiment";
    snapshot.sequence = 42;
    snapshot.events_processed = 1'000'000;
    snapshot.config_fingerprint = 0xDEADBEEFCAFEULL;
    const std::string json = snapshot.to_json();

    for (auto _ : state) {
        auto restored = ptl::experiment::ExperimentSnapshot::from_json(json);
        benchmark::DoNotOptimize(restored.has_value());
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_CheckpointRestore);

}  // namespace
