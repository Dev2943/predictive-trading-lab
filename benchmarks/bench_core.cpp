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
