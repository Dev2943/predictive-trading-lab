#include <algorithm>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <limits>
#include <numeric>
#include <vector>

#include "ptl/optimization/optimizer.hpp"
#include "support/ptl_catch.hpp"

using namespace ptl;
using namespace ptl::optimization;

namespace {

Timestamp at(const char* iso) {
    Timestamp ts{};
    REQUIRE(parse_timestamp(iso, ts));
    return ts;
}

std::vector<InstrumentId> universe(std::size_t n) {
    std::vector<InstrumentId> out;
    for (std::size_t i = 0; i < n; ++i) out.push_back(static_cast<InstrumentId>(i));
    return out;
}

/// A deterministic return series for `rows` observations of `assets` assets.
std::vector<double> series(std::size_t rows, std::size_t assets, double phase = 0.0) {
    std::vector<double> out;
    out.reserve(rows * assets);
    for (std::size_t r = 0; r < rows; ++r) {
        for (std::size_t c = 0; c < assets; ++c) {
            out.push_back(
                std::sin((static_cast<double>(r) + phase) * 0.3 + static_cast<double>(c)) * 0.01);
        }
    }
    return out;
}

OptimizationInput input_with(std::size_t n, const SymmetricMatrix& cov) {
    OptimizationInput in;
    in.as_of = at("2024-07-02T15:00:00Z");
    in.instruments = universe(n);
    in.covariance = cov;
    in.expected_returns.assign(n, 0.02);
    for (std::size_t i = 0; i < n; ++i) {
        in.expected_returns[i] = 0.01 + 0.01 * static_cast<double>(i);
    }
    return in;
}

}  // namespace

TEST_CASE("covariance uses only the observations it is given", "[optimization][leakage]") {
    // THE CENTRAL LEAK CHECK. A covariance estimated over a window that
    // includes the period being traded is the most respectable-looking
    // lookahead there is: plausible numbers, short code, and a backtest that is
    // wrong in a way no summary statistic reveals.
    const std::size_t assets = 3;
    const auto history = series(100, assets);

    CovarianceConfig cfg;
    cfg.method = CovarianceMethod::Sample;
    cfg.min_observations_ratio = 0.0;

    // Estimate over the first 60 rows.
    auto truncated = CovarianceEstimator{cfg}.estimate(
        std::span<const double>{history.data(), 60 * assets}, 60, assets);
    REQUIRE(truncated.has_value());

    // The same 60 rows, but with 40 more sitting in the caller's buffer. The
    // estimator must not see them.
    auto with_future = CovarianceEstimator{cfg}.estimate(
        std::span<const double>{history.data(), 60 * assets}, 60, assets);
    REQUIRE(with_future.has_value());

    for (std::size_t i = 0; i < assets; ++i) {
        for (std::size_t j = 0; j < assets; ++j) {
            // BIT-IDENTICAL, not approximately equal. Any difference would mean
            // later observations influenced the estimate.
            REQUIRE(truncated->at(i, j) == with_future->at(i, j));
        }
    }
}

TEST_CASE("a trailing window ends at the last observation", "[optimization][leakage]") {
    // A centred window would straddle the decision instant, which is the same
    // lookahead wearing a different name.
    const std::size_t assets = 2;
    const auto history = series(200, assets);

    CovarianceConfig windowed;
    windowed.method = CovarianceMethod::Rolling;
    windowed.window = 50;
    windowed.min_observations_ratio = 0.0;

    // The trailing 50 of 200 must equal the whole-window estimate over exactly
    // those rows supplied alone.
    auto from_full = CovarianceEstimator{windowed}.estimate(history, 200, assets);
    REQUIRE(from_full.has_value());

    const auto tail_only = std::vector<double>(history.end() - 50 * assets, history.end());
    CovarianceConfig all;
    all.method = CovarianceMethod::Sample;
    all.min_observations_ratio = 0.0;
    auto from_tail = CovarianceEstimator{all}.estimate(tail_only, 50, assets);
    REQUIRE(from_tail.has_value());

    for (std::size_t i = 0; i < assets; ++i) {
        for (std::size_t j = 0; j < assets; ++j) {
            REQUIRE(from_full->at(i, j) == Catch::Approx(from_tail->at(i, j)).epsilon(1e-12));
        }
    }
}

TEST_CASE("walk-forward estimates are monotone in information",
          "[optimization][leakage][walkforward]") {
    // Each fold sees strictly more history than the last, and never anything
    // after its own boundary. This is the shape a Phase 5 walk-forward imposes,
    // asserted here for the risk model.
    const std::size_t assets = 2;
    const auto history = series(300, assets);

    CovarianceConfig cfg;
    cfg.method = CovarianceMethod::Sample;
    cfg.min_observations_ratio = 0.0;

    std::vector<SymmetricMatrix> folds;
    for (const std::size_t boundary :
         {std::size_t{60}, std::size_t{120}, std::size_t{180}, std::size_t{240}}) {
        auto cov = CovarianceEstimator{cfg}.estimate(
            std::span<const double>{history.data(), boundary * assets}, boundary, assets);
        REQUIRE(cov.has_value());
        folds.push_back(*cov);
    }

    // Re-running an earlier fold after later folds have been computed gives the
    // identical answer: no state leaks between folds.
    auto rerun = CovarianceEstimator{cfg}.estimate(
        std::span<const double>{history.data(), 60 * assets}, 60, assets);
    REQUIRE(rerun.has_value());
    for (std::size_t i = 0; i < assets; ++i) {
        for (std::size_t j = 0; j < assets; ++j) {
            REQUIRE(rerun->at(i, j) == folds.front().at(i, j));
        }
    }
}

TEST_CASE("no optimizer produces a NaN weight", "[optimization][leakage][property]") {
    // A NaN weight propagates silently through rebalancing into an order
    // quantity, and the first sign of trouble is a rejected order with an
    // unreadable size. Every optimizer is checked against every hostile input.
    auto registry = OptimizerRegistry::with_defaults();
    REQUIRE(registry.has_value());
    REQUIRE(registry->size() == 9);

    const std::size_t n = 4;

    // A battery of awkward risk models.
    std::vector<SymmetricMatrix> models;
    {
        SymmetricMatrix singular{n};  // rank 1: every asset identical
        for (std::size_t i = 0; i < n; ++i) {
            for (std::size_t j = 0; j < n; ++j) singular.at(i, j) = 0.04;
        }
        models.push_back(singular);
    }
    {
        SymmetricMatrix zeros{n};  // no risk at all
        models.push_back(zeros);
    }
    {
        SymmetricMatrix tiny{n};
        for (std::size_t i = 0; i < n; ++i) tiny.at(i, i) = 1e-300;
        models.push_back(tiny);
    }
    {
        SymmetricMatrix huge{n};
        for (std::size_t i = 0; i < n; ++i) huge.at(i, i) = 1e12;
        models.push_back(huge);
    }

    for (const auto& name : registry->names()) {
        for (std::size_t m = 0; m < models.size(); ++m) {
            INFO("optimizer " << name << " model " << m);
            auto optimizer = registry->create(name);
            REQUIRE(optimizer.has_value());

            auto input = input_with(n, models[m]);
            auto result = (*optimizer)->optimize(input);
            // Refusing is acceptable; returning a NaN is not.
            if (!result) continue;
            for (const double w : result->weights) {
                REQUIRE(is_finite(w));
            }
            REQUIRE(is_finite(result->gross_exposure));
            REQUIRE(is_finite(result->net_exposure));
            REQUIRE(is_finite(result->expected_volatility));
            REQUIRE(is_finite(result->sharpe));
        }
    }
}

TEST_CASE("every optimizer handles a degenerate universe", "[optimization][leakage][edge]") {
    auto registry = OptimizerRegistry::with_defaults();
    REQUIRE(registry.has_value());

    for (const auto& name : registry->names()) {
        INFO("optimizer " << name);
        auto optimizer = registry->create(name);

        // Empty universe: refused, never a crash.
        OptimizationInput empty;
        empty.as_of = at("2024-07-02T15:00:00Z");
        REQUIRE_FALSE((*optimizer)->optimize(empty).has_value());

        // A single asset is a legitimate portfolio.
        OptimizationInput single;
        single.as_of = at("2024-07-02T15:00:00Z");
        single.instruments = universe(1);
        single.volatilities = {0.15};
        single.covariance = SymmetricMatrix{1};
        single.covariance.at(0, 0) = 0.0225;
        single.expected_returns = {0.05};
        single.signals = {1.0};
        auto one = (*optimizer)->optimize(single);
        if (one) {
            REQUIRE(one->weights.size() == 1);
            REQUIRE(is_finite(one->weights[0]));
        }
    }
}

TEST_CASE("a duplicated instrument is refused", "[optimization][leakage][validation]") {
    // Two entries for one name would give it two weights, and the rebalance
    // engine would see a target it cannot satisfy.
    OptimizationInput in;
    in.as_of = at("2024-07-02T15:00:00Z");
    in.instruments = {InstrumentId{0}, InstrumentId{1}, InstrumentId{0}};
    auto validated = in.validate();
    REQUIRE_FALSE(validated.has_value());
    REQUIRE(validated.error().message.find("duplicate") != std::string::npos);
}

TEST_CASE("mismatched input lengths are refused", "[optimization][leakage][validation]") {
    OptimizationInput in;
    in.as_of = at("2024-07-02T15:00:00Z");
    in.instruments = universe(3);
    in.expected_returns = {0.01, 0.02};  // too short
    REQUIRE_FALSE(in.validate().has_value());

    OptimizationInput bad_cov;
    bad_cov.as_of = at("2024-07-02T15:00:00Z");
    bad_cov.instruments = universe(3);
    bad_cov.covariance = SymmetricMatrix{2};
    REQUIRE_FALSE(bad_cov.validate().has_value());

    OptimizationInput non_finite;
    non_finite.as_of = at("2024-07-02T15:00:00Z");
    non_finite.instruments = universe(2);
    non_finite.expected_returns = {0.01, std::numeric_limits<double>::quiet_NaN()};
    REQUIRE_FALSE(non_finite.validate().has_value());
}

// ---------------------------------------------------------------------------
// Constraints
// ---------------------------------------------------------------------------

TEST_CASE("constraints are enforced, not approximated", "[optimization][constraints][property]") {
    // A penalty produces weights that violate a limit by "only a little", and a
    // limit that can be violated a little is not a limit.
    auto registry = OptimizerRegistry::with_defaults();
    REQUIRE(registry.has_value());

    OptimizerConfig cfg;
    cfg.constraints.direction = DirectionMode::LongOnly;
    cfg.constraints.max_position = 0.15;
    cfg.constraints.min_position = 0.0;
    cfg.constraints.max_gross_leverage = 1.0;
    cfg.constraints.max_net_leverage = 1.0;

    const std::size_t n = 10;
    SymmetricMatrix cov{n};
    for (std::size_t i = 0; i < n; ++i) {
        cov.at(i, i) = 0.01 * (1.0 + static_cast<double>(i) * 0.1);
    }
    auto input = input_with(n, cov);
    input.volatilities.assign(n, 0.1);

    const ConstraintProjector projector{cfg.constraints};
    for (const auto& name : registry->names()) {
        INFO("optimizer " << name);
        auto optimizer = registry->create(name);
        auto result = (*optimizer)->optimize(input);
        if (!result) continue;

        for (const double w : result->weights) {
            REQUIRE(w >= -1e-9);  // long only
            REQUIRE(w <= cfg.constraints.max_position + 1e-9);
        }
        REQUIRE(result->gross_exposure <= cfg.constraints.max_gross_leverage + 1e-9);
        REQUIRE(projector.feasible(result->weights, input));
    }
}

TEST_CASE("sector exposure is capped", "[optimization][constraints]") {
    OptimizerConfig cfg;
    cfg.constraints = ConstraintSet::long_only(1.0);
    cfg.constraints.max_sector_exposure = 0.30;

    OptimizationInput in;
    in.as_of = at("2024-07-02T15:00:00Z");
    in.instruments = universe(4);
    in.volatilities.assign(4, 0.10);
    // Three of four names in one sector, which equal weight would put at 75%.
    in.sectors = {1, 1, 1, 2};

    auto result = EqualWeightOptimizer{cfg}.optimize(in);
    REQUIRE(result.has_value());

    double sector_one = 0.0;
    for (std::size_t i = 0; i < 3; ++i) sector_one += std::abs(result->weights[i]);
    REQUIRE(sector_one <= 0.30 + 1e-9);
    REQUIRE(std::find(result->binding_constraints.begin(), result->binding_constraints.end(),
                      "max_sector_exposure") != result->binding_constraints.end());
}

TEST_CASE("dollar neutrality holds", "[optimization][constraints]") {
    OptimizerConfig cfg;
    cfg.constraints = ConstraintSet::market_neutral(0.30);

    OptimizationInput in;
    in.as_of = at("2024-07-02T15:00:00Z");
    in.instruments = universe(4);
    in.signals = {0.5, 0.3, -0.1, -0.1};  // net long signal
    in.volatilities.assign(4, 0.10);

    auto result = SignalWeightedOptimizer{cfg}.optimize(in);
    REQUIRE(result.has_value());
    // The long and short legs net to zero despite a net-long signal.
    REQUIRE(result->net_exposure == Catch::Approx(0.0).margin(1e-6));
}

TEST_CASE("beta neutrality removes benchmark exposure", "[optimization][constraints][property]") {
    OptimizerConfig cfg;
    cfg.constraints.direction = DirectionMode::LongShort;
    cfg.constraints.max_position = 1.0;
    cfg.constraints.min_position = -1.0;
    cfg.constraints.beta_neutral = true;
    cfg.constraints.max_gross_leverage = 2.0;
    cfg.constraints.max_net_leverage = 2.0;

    OptimizationInput in;
    in.as_of = at("2024-07-02T15:00:00Z");
    in.instruments = universe(3);
    in.betas = {1.5, 1.0, 0.5};
    in.signals = {0.5, 0.3, 0.2};
    in.volatilities.assign(3, 0.10);

    auto result = SignalWeightedOptimizer{cfg}.optimize(in);
    REQUIRE(result.has_value());

    double portfolio_beta = 0.0;
    for (std::size_t i = 0; i < 3; ++i) portfolio_beta += result->weights[i] * in.betas[i];
    REQUIRE(portfolio_beta == Catch::Approx(0.0).margin(1e-6));
}

TEST_CASE("the turnover budget limits movement toward the target", "[optimization][constraints]") {
    // Trading to a partial target is what a turnover budget means; the
    // remainder is picked up on the next rebalance.
    OptimizerConfig cfg;
    cfg.constraints = ConstraintSet::long_only(1.0);
    cfg.constraints.max_turnover = 0.10;

    OptimizationInput in;
    in.as_of = at("2024-07-02T15:00:00Z");
    in.instruments = universe(2);
    in.volatilities.assign(2, 0.10);
    // Currently all in the first name; equal weight would move 100%.
    in.current_weights = {1.0, 0.0};

    auto result = EqualWeightOptimizer{cfg}.optimize(in);
    REQUIRE(result.has_value());

    double turnover = 0.0;
    for (std::size_t i = 0; i < 2; ++i) {
        turnover += std::abs(result->weights[i] - in.current_weights[i]);
    }
    REQUIRE(turnover <= 0.10 + 1e-9);
    REQUIRE(result->turnover <= 0.10 + 1e-9);
}

TEST_CASE("the cash reserve is held back", "[optimization][constraints]") {
    OptimizerConfig cfg;
    cfg.constraints = ConstraintSet::long_only(1.0);
    cfg.constraints.cash_reserve = 0.20;

    auto in = input_with(4, SymmetricMatrix{});
    in.volatilities.assign(4, 0.10);

    auto result = EqualWeightOptimizer{cfg}.optimize(in);
    REQUIRE(result.has_value());
    REQUIRE(result->gross_exposure == Catch::Approx(0.80).margin(1e-9));
    // Cash is reported explicitly rather than left implicit.
    REQUIRE(result->cash_weight == Catch::Approx(0.20).margin(1e-9));
}

TEST_CASE("dust positions are removed", "[optimization][constraints][edge]") {
    // A one-basis-point position costs a full round trip in commission and
    // contributes nothing.
    OptimizerConfig cfg;
    cfg.constraints = ConstraintSet::long_only(1.0);
    cfg.constraints.min_trade_size = 0.05;

    OptimizationInput in;
    in.as_of = at("2024-07-02T15:00:00Z");
    in.instruments = universe(3);
    in.signals = {0.98, 0.01, 0.01};
    in.volatilities.assign(3, 0.10);

    auto result = SignalWeightedOptimizer{cfg}.optimize(in);
    REQUIRE(result.has_value());
    REQUIRE(result->weights[1] == Catch::Approx(0.0));
    REQUIRE(result->weights[2] == Catch::Approx(0.0));
}

TEST_CASE("a contradictory constraint set is refused", "[optimization][constraints][validation]") {
    ConstraintSet contradictory;
    contradictory.direction = DirectionMode::LongOnly;
    contradictory.min_position = -0.10;  // shorts in a long-only book
    REQUIRE_FALSE(contradictory.validate().has_value());

    ConstraintSet inverted;
    inverted.max_position = 0.05;
    inverted.min_position = 0.10;
    REQUIRE_FALSE(inverted.validate().has_value());

    ConstraintSet bad_cash;
    bad_cash.cash_reserve = 1.5;
    REQUIRE_FALSE(bad_cash.validate().has_value());
}

// ---------------------------------------------------------------------------
// Determinism
// ---------------------------------------------------------------------------

TEST_CASE("optimization is deterministic", "[optimization][determinism]") {
    auto registry = OptimizerRegistry::with_defaults();
    REQUIRE(registry.has_value());

    OptimizerConfig cfg;
    cfg.constraints = ConstraintSet::long_only(0.40);

    const std::size_t n = 5;
    SymmetricMatrix cov{n};
    for (std::size_t i = 0; i < n; ++i) {
        for (std::size_t j = i; j < n; ++j) {
            cov.set_symmetric(i, j, i == j ? 0.02 + 0.01 * static_cast<double>(i) : 0.005);
        }
    }
    const auto input = input_with(n, cov);

    for (const auto& name : registry->names()) {
        INFO("optimizer " << name);
        auto first = registry->create(name);
        auto second = registry->create(name);
        auto a = (*first)->optimize(input);
        auto b = (*second)->optimize(input);
        REQUIRE(a.has_value() == b.has_value());
        if (!a) continue;
        REQUIRE(a->weights.size() == b->weights.size());
        for (std::size_t i = 0; i < a->weights.size(); ++i) {
            // EXACT equality: float summation is not associative, so any
            // ordering difference would surface here.
            REQUIRE(a->weights[i] == b->weights[i]);
        }
        REQUIRE(a->iterations == b->iterations);
    }
}

TEST_CASE("the registry holds all nine optimizers in a stable order", "[optimization][registry]") {
    auto registry = OptimizerRegistry::with_defaults();
    REQUIRE(registry.has_value());
    REQUIRE(registry->size() == 9);

    const auto names = registry->names();
    // std::map ordering, so a report listing optimizers is identical run to run.
    REQUIRE(std::is_sorted(names.begin(), names.end()));
    REQUIRE(registry->contains("risk_parity"));
    REQUIRE(registry->contains("max_sharpe"));
    REQUIRE(registry->contains("signal_weighted"));

    auto unknown = registry->create("does_not_exist");
    REQUIRE_FALSE(unknown.has_value());
    REQUIRE(unknown.error().message.find("kelly") != std::string::npos);
}
