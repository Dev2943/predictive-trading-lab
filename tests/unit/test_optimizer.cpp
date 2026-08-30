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

/// A diagonal covariance from volatilities.
SymmetricMatrix diagonal_cov(const std::vector<double>& vols) {
    SymmetricMatrix m{vols.size()};
    for (std::size_t i = 0; i < vols.size(); ++i) m.at(i, i) = vols[i] * vols[i];
    return m;
}

OptimizationInput input_for(std::size_t n, const std::vector<double>& vols = {}) {
    OptimizationInput in;
    in.as_of = at("2024-07-02T15:00:00Z");
    in.instruments = universe(n);
    if (!vols.empty()) {
        in.volatilities = vols;
        in.covariance = diagonal_cov(vols);
    }
    return in;
}

double gross_of(const std::vector<double>& w) {
    double total = 0.0;
    for (const double x : w) total += std::abs(x);
    return total;
}

}  // namespace

// ---------------------------------------------------------------------------
// Matrix and covariance
// ---------------------------------------------------------------------------

TEST_CASE("the quadratic form computes portfolio variance", "[optimization][covariance]") {
    SymmetricMatrix m{2};
    m.set_symmetric(0, 0, 0.04);
    m.set_symmetric(1, 1, 0.09);
    m.set_symmetric(0, 1, 0.012);

    const std::vector<double> w{0.5, 0.5};
    // 0.25*0.04 + 0.25*0.09 + 2*0.25*0.012
    REQUIRE(m.quadratic_form(w) == Catch::Approx(0.0385));
    REQUIRE(m.is_symmetric());
    REQUIRE(m.all_finite());
}

TEST_CASE("variance is never reported negative", "[optimization][covariance][edge]") {
    // A tiny negative from rounding on a near-singular matrix must not
    // propagate into a sqrt.
    SymmetricMatrix m{2};
    m.set_symmetric(0, 0, 1e-18);
    m.set_symmetric(1, 1, 1e-18);
    m.set_symmetric(0, 1, -1e-18);
    const std::vector<double> w{1.0, 1.0};
    REQUIRE(m.quadratic_form(w) >= 0.0);
}

TEST_CASE("sample covariance matches a hand computation", "[optimization][covariance][numerical]") {
    // Two assets, four observations, second exactly twice the first.
    const std::vector<double> obs{0.01, 0.02, -0.01, -0.02, 0.02, 0.04, -0.02, -0.04};

    CovarianceConfig cfg;
    cfg.method = CovarianceMethod::Sample;
    cfg.min_observations_ratio = 0.0;
    cfg.min_eigenvalue_ratio = 0.0;
    CovarianceEstimator estimator{cfg};

    auto cov = estimator.estimate(obs, 4, 2);
    REQUIRE(cov.has_value());
    REQUIRE(cov->size() == 2);
    // var(x) with mean 0: (0.0001+0.0001+0.0004+0.0004)/3
    REQUIRE(cov->at(0, 0) == Catch::Approx(0.001 / 3.0).epsilon(1e-9));
    // Perfectly correlated, so var(y) = 4 var(x) and cov = 2 var(x).
    REQUIRE(cov->at(1, 1) == Catch::Approx(4.0 * cov->at(0, 0)).epsilon(1e-9));
    REQUIRE(cov->at(0, 1) == Catch::Approx(2.0 * cov->at(0, 0)).epsilon(1e-9));
    REQUIRE(cov->is_symmetric());
}

TEST_CASE("EWMA weights recent observations more heavily", "[optimization][covariance]") {
    // A calm early period followed by a violent recent one: EWMA must report
    // more risk than the equal-weighted estimate.
    std::vector<double> obs;
    for (int i = 0; i < 50; ++i) obs.push_back(i % 2 == 0 ? 0.001 : -0.001);
    for (int i = 0; i < 50; ++i) obs.push_back(i % 2 == 0 ? 0.05 : -0.05);

    CovarianceConfig sample_cfg;
    sample_cfg.method = CovarianceMethod::Sample;
    sample_cfg.min_observations_ratio = 0.0;
    CovarianceEstimator sample{sample_cfg};

    CovarianceConfig ewma_cfg;
    ewma_cfg.method = CovarianceMethod::Ewma;
    ewma_cfg.ewma_lambda = 0.94;
    ewma_cfg.min_observations_ratio = 0.0;
    CovarianceEstimator ewma{ewma_cfg};

    auto s = sample.estimate(obs, 100, 1);
    auto e = ewma.estimate(obs, 100, 1);
    REQUIRE(s.has_value());
    REQUIRE(e.has_value());
    REQUIRE(e->at(0, 0) > s->at(0, 0));
}

TEST_CASE("shrinkage pulls a covariance toward a scaled identity", "[optimization][covariance]") {
    std::vector<double> obs;
    for (int i = 0; i < 40; ++i) {
        const double x = std::sin(static_cast<double>(i) * 0.5) * 0.01;
        obs.push_back(x);
        obs.push_back(x * 0.98);  // strongly correlated
    }

    CovarianceConfig raw_cfg;
    raw_cfg.method = CovarianceMethod::Sample;
    raw_cfg.min_observations_ratio = 0.0;
    auto raw = CovarianceEstimator{raw_cfg}.estimate(obs, 40, 2);

    CovarianceConfig shrunk_cfg;
    shrunk_cfg.method = CovarianceMethod::Shrinkage;
    shrunk_cfg.shrinkage_intensity = 0.5;
    shrunk_cfg.min_observations_ratio = 0.0;
    CovarianceEstimator estimator{shrunk_cfg};
    auto shrunk = estimator.estimate(obs, 40, 2);

    REQUIRE(raw.has_value());
    REQUIRE(shrunk.has_value());
    // The off-diagonal is pulled toward zero.
    REQUIRE(std::abs(shrunk->at(0, 1)) < std::abs(raw->at(0, 1)));
    REQUIRE(estimator.diagnostics().applied_shrinkage == Catch::Approx(0.5));
}

TEST_CASE("too few observations degrade to a diagonal and say so",
          "[optimization][covariance][edge]") {
    // A 10x10 estimated from 4 rows is not an estimate, and silently returning
    // noise is the worse failure.
    std::vector<double> obs(4 * 10, 0.0);
    for (std::size_t i = 0; i < obs.size(); ++i) {
        obs[i] = std::sin(static_cast<double>(i)) * 0.01;
    }

    CovarianceConfig cfg;
    cfg.method = CovarianceMethod::Sample;
    cfg.min_observations_ratio = 1.5;
    CovarianceEstimator estimator{cfg};

    auto cov = estimator.estimate(obs, 4, 10);
    REQUIRE(cov.has_value());
    REQUIRE(estimator.diagnostics().degraded);
    REQUIRE(estimator.diagnostics().degradation_reason.find("diagonal") != std::string::npos);
    // Off-diagonals are zero, because correlations were not estimated.
    REQUIRE(cov->at(0, 1) == Catch::Approx(0.0));
}

TEST_CASE("PSD enforcement repairs a non-PSD matrix", "[optimization][covariance][numerical]") {
    // An optimizer inverting a singular matrix produces enormous offsetting
    // weights that look like a brilliant hedge and are numerical noise.
    SymmetricMatrix bad{2};
    bad.set_symmetric(0, 0, 1.0);
    bad.set_symmetric(1, 1, 1.0);
    // A correlation above one is impossible and makes the matrix indefinite.
    bad.set_symmetric(0, 1, 1.5);

    auto smallest = CovarianceEstimator::smallest_eigenvalue(bad);
    REQUIRE(smallest.has_value());
    REQUIRE(*smallest < 0.0);

    bool repaired = false;
    double before = 0.0;
    auto fixed = CovarianceEstimator::enforce_psd(bad, 1e-8, &repaired, &before);
    REQUIRE(fixed.has_value());
    REQUIRE(repaired);
    REQUIRE(before < 0.0);

    auto after = CovarianceEstimator::smallest_eigenvalue(*fixed);
    REQUIRE(after.has_value());
    REQUIRE(*after >= -1e-9);
    REQUIRE(fixed->is_symmetric(1e-9));
    REQUIRE(fixed->all_finite());
}

TEST_CASE("an already-PSD matrix is left alone", "[optimization][covariance]") {
    SymmetricMatrix good{2};
    good.set_symmetric(0, 0, 0.04);
    good.set_symmetric(1, 1, 0.09);
    good.set_symmetric(0, 1, 0.01);

    bool repaired = true;
    auto result = CovarianceEstimator::enforce_psd(good, 1e-12, &repaired);
    REQUIRE(result.has_value());
    REQUIRE_FALSE(repaired);
    REQUIRE(result->at(0, 1) == Catch::Approx(0.01));
}

TEST_CASE("correlation handles a zero-variance asset", "[optimization][covariance][edge]") {
    SymmetricMatrix cov{2};
    cov.set_symmetric(0, 0, 0.04);
    cov.set_symmetric(1, 1, 0.0);  // constant series
    auto corr = CovarianceEstimator::to_correlation(cov);
    REQUIRE(corr.has_value());
    REQUIRE(corr->at(0, 0) == Catch::Approx(1.0));
    REQUIRE(corr->at(1, 1) == Catch::Approx(1.0));
    REQUIRE(corr->at(0, 1) == Catch::Approx(0.0));
    REQUIRE(corr->all_finite());
}

TEST_CASE("covariance refuses malformed input", "[optimization][covariance][validation]") {
    CovarianceEstimator estimator;
    REQUIRE_FALSE(estimator.estimate({}, 0, 0).has_value());
    const std::vector<double> obs{1.0, 2.0, 3.0};
    // Buffer size does not match the declared shape.
    REQUIRE_FALSE(estimator.estimate(obs, 2, 2).has_value());
    // A single observation cannot yield a variance.
    const std::vector<double> single{0.01};
    REQUIRE_FALSE(estimator.estimate(single, 1, 1).has_value());

    const std::vector<double> with_nan{0.01, std::numeric_limits<double>::quiet_NaN(), 0.02, 0.03};
    REQUIRE_FALSE(estimator.estimate(with_nan, 2, 2).has_value());
}

// ---------------------------------------------------------------------------
// Optimizers
// ---------------------------------------------------------------------------

TEST_CASE("equal weight is exactly 1/N of the budget", "[optimization][optimizer]") {
    OptimizerConfig cfg;
    cfg.constraints = ConstraintSet::long_only(1.0);
    const EqualWeightOptimizer optimizer{cfg};

    auto result = optimizer.optimize(input_for(4));
    REQUIRE(result.has_value());
    REQUIRE(result->weights.size() == 4);
    for (const double w : result->weights) REQUIRE(w == Catch::Approx(0.25));
    REQUIRE(gross_of(result->weights) == Catch::Approx(1.0));
    REQUIRE(result->cash_weight == Catch::Approx(0.0));
}

TEST_CASE("inverse volatility weights the quiet asset more",
          "[optimization][optimizer][property]") {
    OptimizerConfig cfg;
    cfg.constraints = ConstraintSet::long_only(1.0);
    const InverseVolatilityOptimizer optimizer{cfg};

    // Second asset is twice as volatile, so it gets half the weight.
    auto result = optimizer.optimize(input_for(2, {0.10, 0.20}));
    REQUIRE(result.has_value());
    REQUIRE(result->weights[0] == Catch::Approx(2.0 / 3.0));
    REQUIRE(result->weights[1] == Catch::Approx(1.0 / 3.0));
}

TEST_CASE("a zero-volatility asset takes no weight rather than infinite",
          "[optimization][optimizer][edge]") {
    // A constant price series is a data problem, not an arbitrage.
    OptimizerConfig cfg;
    cfg.constraints = ConstraintSet::long_only(1.0);
    auto result = InverseVolatilityOptimizer{cfg}.optimize(input_for(2, {0.10, 0.0}));
    REQUIRE(result.has_value());
    REQUIRE(result->weights[1] == Catch::Approx(0.0));
    REQUIRE(is_finite(result->weights[0]));

    // Every asset zero: fall back to equal weight and say so.
    auto all_zero = InverseVolatilityOptimizer{cfg}.optimize(input_for(2, {0.0, 0.0}));
    REQUIRE(all_zero.has_value());
    REQUIRE(all_zero->detail.find("fell back to equal weight") != std::string::npos);
}

TEST_CASE("risk parity equalises risk contributions", "[optimization][optimizer][numerical]") {
    // THE DEFINING PROPERTY. Not equal weights, and not inverse volatility
    // either once the assets are correlated.
    OptimizerConfig cfg;
    cfg.constraints = ConstraintSet::long_only(1.0);
    cfg.max_iterations = 2000;

    OptimizationInput in = input_for(3, {0.10, 0.20, 0.15});
    // Introduce correlation, so the answer differs from inverse volatility.
    in.covariance.set_symmetric(0, 1, 0.10 * 0.20 * 0.5);
    in.covariance.set_symmetric(0, 2, 0.10 * 0.15 * 0.3);
    in.covariance.set_symmetric(1, 2, 0.20 * 0.15 * 0.4);

    auto result = RiskParityOptimizer{cfg}.optimize(in);
    REQUIRE(result.has_value());

    const auto contributions =
        RiskParityOptimizer::risk_contributions(result->weights, in.covariance);
    const double target = 1.0 / 3.0;
    for (const double rc : contributions) {
        REQUIRE(rc == Catch::Approx(target).margin(0.02));
    }
    // The contributions are shares, so they sum to one.
    REQUIRE(std::accumulate(contributions.begin(), contributions.end(), 0.0) ==
            Catch::Approx(1.0).epsilon(0.01));
}

TEST_CASE("minimum variance concentrates in the low-variance asset",
          "[optimization][optimizer][property]") {
    OptimizerConfig cfg;
    cfg.constraints = ConstraintSet::long_only(1.0);
    cfg.max_iterations = 3000;

    auto result = MinimumVarianceOptimizer{cfg}.optimize(input_for(2, {0.05, 0.40}));
    REQUIRE(result.has_value());
    REQUIRE(result->usable());
    // Far more in the quiet asset.
    REQUIRE(result->weights[0] > result->weights[1]);
    REQUIRE(result->expected_volatility < 0.40);
}

TEST_CASE("minimum variance refuses without a covariance",
          "[optimization][optimizer][validation]") {
    OptimizerConfig cfg;
    auto result = MinimumVarianceOptimizer{cfg}.optimize(input_for(3));
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().message.find("covariance") != std::string::npos);
}

TEST_CASE("mean variance prefers the higher expected return",
          "[optimization][optimizer][property]") {
    OptimizerConfig cfg;
    cfg.constraints = ConstraintSet::long_only(1.0);
    cfg.objective.risk_aversion = 1.0;
    cfg.max_iterations = 3000;

    OptimizationInput in = input_for(2, {0.15, 0.15});  // equal risk
    in.expected_returns = {0.10, 0.02};

    auto result = MeanVarianceOptimizer{cfg}.optimize(in);
    REQUIRE(result.has_value());
    REQUIRE(result->weights[0] > result->weights[1]);
    REQUIRE(result->expected_return > 0.0);
}

TEST_CASE("an objective needing forecasts refuses without them",
          "[optimization][optimizer][validation]") {
    // A zero forecast is a real opinion, not an absence of one.
    OptimizerConfig cfg;
    auto result = MaximumSharpeOptimizer{cfg}.optimize(input_for(3, {0.1, 0.1, 0.1}));
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().message.find("expected returns") != std::string::npos);
}

TEST_CASE("maximum Sharpe beats equal weight on Sharpe", "[optimization][optimizer][property]") {
    OptimizerConfig cfg;
    cfg.constraints = ConstraintSet::long_only(1.0);
    cfg.max_iterations = 5000;

    OptimizationInput in = input_for(3, {0.10, 0.30, 0.20});
    in.expected_returns = {0.08, 0.05, 0.02};

    auto optimal = MaximumSharpeOptimizer{cfg}.optimize(in);
    auto equal = EqualWeightOptimizer{cfg}.optimize(in);
    REQUIRE(optimal.has_value());
    REQUIRE(equal.has_value());
    REQUIRE(optimal->sharpe >= equal->sharpe);
}

TEST_CASE("Kelly is fractional by default", "[optimization][optimizer]") {
    OptimizerConfig cfg;
    cfg.constraints = ConstraintSet::long_only(1.0);
    cfg.objective.kelly_fraction = 0.25;

    OptimizationInput in = input_for(2, {0.15, 0.15});
    in.expected_returns = {0.10, 0.05};

    auto result = KellyOptimizer{cfg}.optimize(in);
    REQUIRE(result.has_value());
    REQUIRE(result->detail.find("assumes the edge is exact") != std::string::npos);
    REQUIRE(is_finite(result->weights[0]));
}

TEST_CASE("target volatility scales toward its target", "[optimization][optimizer][property]") {
    OptimizerConfig cfg;
    cfg.constraints = ConstraintSet::long_only(3.0);
    cfg.constraints.max_position = 3.0;
    cfg.target_volatility = 0.10;
    cfg.max_iterations = 2000;

    auto result = TargetVolatilityOptimizer{cfg}.optimize(input_for(2, {0.20, 0.20}));
    REQUIRE(result.has_value());
    REQUIRE(result->expected_volatility == Catch::Approx(0.10).margin(0.02));
    REQUIRE(result->detail.find("target") != std::string::npos);
}

TEST_CASE("signal weighting is proportional and signed", "[optimization][optimizer]") {
    OptimizerConfig cfg;
    cfg.constraints.direction = DirectionMode::LongShort;
    cfg.constraints.max_position = 1.0;
    cfg.constraints.min_position = -1.0;
    cfg.constraints.max_gross_leverage = 1.0;
    cfg.constraints.max_net_leverage = 1.0;

    OptimizationInput in = input_for(3);
    in.signals = {0.6, -0.3, 0.1};

    auto result = SignalWeightedOptimizer{cfg}.optimize(in);
    REQUIRE(result.has_value());
    // The sign carries direction, the magnitude carries conviction.
    REQUIRE(result->weights[0] > 0.0);
    REQUIRE(result->weights[1] < 0.0);
    REQUIRE(result->weights[0] == Catch::Approx(0.6));
    REQUIRE(result->weights[1] == Catch::Approx(-0.3));
}

TEST_CASE("all-zero signals produce a flat book", "[optimization][optimizer][edge]") {
    // A genuine flat view, and holding nothing is the correct expression of it.
    OptimizerConfig cfg;
    OptimizationInput in = input_for(3);
    in.signals = {0.0, 0.0, 0.0};

    auto result = SignalWeightedOptimizer{cfg}.optimize(in);
    REQUIRE(result.has_value());
    REQUIRE(gross_of(result->weights) == Catch::Approx(0.0));
    REQUIRE(result->cash_weight == Catch::Approx(1.0));
    REQUIRE(result->detail.find("flat view") != std::string::npos);
}
