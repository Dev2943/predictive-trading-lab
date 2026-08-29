#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cmath>
#include <sstream>
#include <vector>

#include "ptl/models/pipeline.hpp"
#include "support/ptl_catch.hpp"

using namespace ptl;
using namespace ptl::models;
using namespace std::chrono;

namespace {

Timestamp at(const char* iso) {
    Timestamp ts{};
    REQUIRE(parse_timestamp(iso, ts));
    return ts;
}

/// Build a feature matrix from explicit rows.
features::FeatureMatrix matrix_from(const std::vector<std::vector<double>>& rows,
                                    std::vector<std::string> names) {
    features::FeatureMatrix m{names, 1, 2};
    Timestamp t = at("2024-07-02T14:00:00Z");
    for (const auto& r : rows) {
        features::FeatureRow fr;
        fr.feature_end_time = t;
        fr.instrument = InstrumentId{0};
        fr.data_version = 1;
        fr.feature_set_id = 2;
        fr.ready_mask = ~0ULL;
        fr.values = r;
        REQUIRE(m.append(fr).has_value());
        t += minutes{1};
    }
    return m;
}

/// y = 3 + 2*x1 - 1*x2, exactly. A known solution the solver must recover.
struct ExactLinear {
    features::FeatureMatrix features;
    std::vector<double> targets;
};

ExactLinear exact_linear(std::size_t n = 200) {
    std::vector<std::vector<double>> rows;
    std::vector<double> y;
    for (std::size_t i = 0; i < n; ++i) {
        const double x1 = std::sin(static_cast<double>(i) * 0.1) * 10.0;
        const double x2 = std::cos(static_cast<double>(i) * 0.07) * 5.0;
        rows.push_back({x1, x2});
        y.push_back(3.0 + 2.0 * x1 - 1.0 * x2);
    }
    return {matrix_from(rows, {"x1", "x2"}), y};
}

std::vector<std::size_t> all_rows(std::size_t n) {
    std::vector<std::size_t> r(n);
    for (std::size_t i = 0; i < n; ++i) r[i] = i;
    return r;
}

}  // namespace

// ---------------------------------------------------------------------------
// OLS
// ---------------------------------------------------------------------------

TEST_CASE("OLS recovers an exact linear relationship", "[models][ols][numerical]") {
    // A fixture with a known closed-form answer. If the solver cannot recover
    // coefficients from noiseless data, nothing it produces on noisy data means
    // anything.
    const auto data = exact_linear();
    auto training = make_training_data(data.features, data.targets, all_rows(200));
    REQUIRE(training.has_value());

    LinearRegression ols;
    REQUIRE(ols.fit(*training).has_value());

    REQUIRE(ols.intercept() == Catch::Approx(3.0).epsilon(1e-9));
    REQUIRE(ols.coefficients()[0] == Catch::Approx(2.0).epsilon(1e-9));
    REQUIRE(ols.coefficients()[1] == Catch::Approx(-1.0).epsilon(1e-9));
    REQUIRE(ols.diagnostics().r_squared == Catch::Approx(1.0).epsilon(1e-9));
}

TEST_CASE("OLS prediction matches the fitted equation", "[models][ols][numerical]") {
    const auto data = exact_linear();
    auto training = make_training_data(data.features, data.targets, all_rows(200));
    LinearRegression ols;
    REQUIRE(ols.fit(*training).has_value());

    const std::vector<double> x{4.0, 2.0};
    auto y = ols.predict(x);
    REQUIRE(y.has_value());
    REQUIRE(*y == Catch::Approx(3.0 + 2.0 * 4.0 - 1.0 * 2.0).epsilon(1e-9));

    auto batch = ols.predict_batch(training->features);
    REQUIRE(batch.has_value());
    REQUIRE(batch->size() == 200);
    REQUIRE((*batch)[0] == Catch::Approx(data.targets[0]).epsilon(1e-9));
}

TEST_CASE("ridge shrinks coefficients toward zero", "[models][ridge][numerical]") {
    const auto data = exact_linear();
    auto training = make_training_data(data.features, data.targets, all_rows(200));

    LinearConfig none;
    LinearRegression ols{none};
    REQUIRE(ols.fit(*training).has_value());

    LinearConfig heavy;
    heavy.l2_penalty = 1000.0;
    LinearRegression ridge{heavy};
    REQUIRE(ridge.fit(*training).has_value());

    REQUIRE(std::abs(ridge.coefficients()[0]) < std::abs(ols.coefficients()[0]));
    REQUIRE(ridge.name() == "ridge");
    REQUIRE(ols.name() == "ols");
}

TEST_CASE("ridge does not penalise the intercept", "[models][ridge][numerical]") {
    // Shrinking the intercept would bias every prediction toward zero in
    // proportion to the penalty, which is not what a regularisation parameter
    // is meant to control.
    std::vector<std::vector<double>> rows;
    std::vector<double> y;
    for (std::size_t i = 0; i < 200; ++i) {
        const double x = std::sin(static_cast<double>(i) * 0.1);
        rows.push_back({x});
        // A large intercept with a tiny slope.
        y.push_back(500.0 + 0.001 * x);
    }
    const auto m = matrix_from(rows, {"x"});
    auto training = make_training_data(m, y, all_rows(200));

    LinearConfig heavy;
    heavy.l2_penalty = 1e6;
    LinearRegression ridge{heavy};
    REQUIRE(ridge.fit(*training).has_value());

    // The slope is crushed but the intercept survives intact.
    REQUIRE(std::abs(ridge.coefficients()[0]) < 1e-6);
    REQUIRE(ridge.intercept() == Catch::Approx(500.0).epsilon(1e-6));
}

TEST_CASE("ridge rescues a perfectly collinear system", "[models][ridge][edge]") {
    // Duplicate columns make X'X singular. OLS has no unique solution; ridge
    // does, and this is exactly the case the VIF diagnostic warns about.
    std::vector<std::vector<double>> rows;
    std::vector<double> y;
    for (std::size_t i = 0; i < 100; ++i) {
        const double x = std::sin(static_cast<double>(i) * 0.1);
        rows.push_back({x, x});  // identical columns
        y.push_back(2.0 * x);
    }
    const auto m = matrix_from(rows, {"a", "b"});
    auto training = make_training_data(m, y, all_rows(100));

    LinearConfig cfg;
    cfg.l2_penalty = 1.0;
    LinearRegression ridge{cfg};
    REQUIRE(ridge.fit(*training).has_value());
    // The load is split between the two identical columns rather than being
    // arbitrary.
    REQUIRE(ridge.coefficients()[0] == Catch::Approx(ridge.coefficients()[1]));
}

TEST_CASE("VIF flags collinearity", "[models][diagnostics]") {
    // The diagnostic that justifies choosing ridge over OLS: lagged returns are
    // strongly collinear, and a high VIF means the coefficients are unstable
    // rather than informative.
    std::vector<std::vector<double>> rows;
    std::vector<double> y;
    for (std::size_t i = 0; i < 200; ++i) {
        const double x = std::sin(static_cast<double>(i) * 0.1);
        const double nearly_x = x + 1e-4 * std::cos(static_cast<double>(i));
        rows.push_back({x, nearly_x});
        y.push_back(x);
    }
    const auto m = matrix_from(rows, {"x", "nearly_x"});
    auto training = make_training_data(m, y, all_rows(200));

    LinearConfig cfg;
    cfg.compute_diagnostics = true;
    LinearRegression ols{cfg};
    REQUIRE(ols.fit(*training).has_value());
    REQUIRE(ols.diagnostics().variance_inflation.size() == 2);
    REQUIRE(ols.diagnostics().condition_number > 100.0);
}

TEST_CASE("weighted least squares honours the weights", "[models][ols][numerical]") {
    // Overlapping labels arrive already down-weighted from the label builder,
    // so this path is exercised on every real fit.
    std::vector<std::vector<double>> rows{{1.0}, {2.0}, {3.0}, {4.0}, {100.0}};
    const std::vector<double> y{1.0, 2.0, 3.0, 4.0, 999.0};
    const auto m = matrix_from(rows, {"x"});

    // Weighting the outlier to nearly nothing should recover the clean line.
    const std::vector<double> w{1.0, 1.0, 1.0, 1.0, 1e-9};
    auto training = make_training_data(m, y, all_rows(5), w);
    REQUIRE(training.has_value());

    LinearRegression ols;
    REQUIRE(ols.fit(*training).has_value());
    REQUIRE(ols.coefficients()[0] == Catch::Approx(1.0).epsilon(1e-4));
}

TEST_CASE("a model refuses more features than observations", "[models][validation][edge]") {
    // Underdetermined: any solution is arbitrary, so returning one would be
    // worse than refusing.
    std::vector<std::vector<double>> rows{{1.0, 2.0, 3.0}, {4.0, 5.0, 6.0}};
    const std::vector<double> y{1.0, 2.0};
    const auto m = matrix_from(rows, {"a", "b", "c"});
    REQUIRE_FALSE(make_training_data(m, y, all_rows(2)).has_value());
}

TEST_CASE("an unfitted model refuses to predict", "[models][validation]") {
    LinearRegression ols;
    REQUIRE_FALSE(ols.fitted());
    REQUIRE_FALSE(ols.predict(std::vector<double>{1.0}).has_value());

    std::ostringstream os;
    REQUIRE_FALSE(ols.save(os).has_value());
}

TEST_CASE("a regressor refuses to fabricate a probability", "[models][validation]") {
    // A regressor squashed through a sigmoid looks like a probability and is
    // badly calibrated. Position sizing depends on calibration.
    const auto data = exact_linear();
    auto training = make_training_data(data.features, data.targets, all_rows(200));
    LinearRegression ols;
    REQUIRE(ols.fit(*training).has_value());

    auto p = ols.predict_proba(std::vector<double>{1.0, 1.0});
    REQUIRE_FALSE(p.has_value());
    REQUIRE(p.error().code == ErrorCode::Unsupported);
}

// ---------------------------------------------------------------------------
// Logistic
// ---------------------------------------------------------------------------

TEST_CASE("logistic separates a linearly separable problem", "[models][logistic][numerical]") {
    std::vector<std::vector<double>> rows;
    std::vector<double> y;
    for (std::size_t i = 0; i < 200; ++i) {
        const double x = -5.0 + static_cast<double>(i) * 0.05;
        rows.push_back({x});
        y.push_back(x > 0.0 ? 1.0 : 0.0);
    }
    const auto m = matrix_from(rows, {"x"});
    auto training = make_training_data(m, y, all_rows(200));

    LogisticRegression logit;
    REQUIRE(logit.fit(*training).has_value());
    REQUIRE(logit.converged());
    // A positive slope: larger x means a higher probability.
    REQUIRE(logit.coefficients()[0] > 0.0);

    auto high = logit.predict_proba(std::vector<double>{5.0});
    auto low = logit.predict_proba(std::vector<double>{-5.0});
    REQUIRE(*high > 0.9);
    REQUIRE(*low < 0.1);
}

TEST_CASE("logistic probabilities stay in the unit interval", "[models][logistic][edge]") {
    // The naive sigmoid overflows for large negative arguments. IRLS drives |z|
    // large precisely when the classes separate cleanly, so the stable form is
    // exercised on exactly the easy problems.
    std::vector<std::vector<double>> rows;
    std::vector<double> y;
    for (std::size_t i = 0; i < 100; ++i) {
        const double x = -50.0 + static_cast<double>(i);
        rows.push_back({x});
        y.push_back(x > 0.0 ? 1.0 : 0.0);
    }
    const auto m = matrix_from(rows, {"x"});
    auto training = make_training_data(m, y, all_rows(100));

    LogisticRegression logit;
    REQUIRE(logit.fit(*training).has_value());
    for (const double probe : {-1e6, -100.0, 0.0, 100.0, 1e6}) {
        auto p = logit.predict_proba(std::vector<double>{probe});
        REQUIRE(p.has_value());
        REQUIRE(*p >= 0.0);
        REQUIRE(*p <= 1.0);
        REQUIRE(is_finite(*p));
    }
}

TEST_CASE("logistic refuses a single-class training set", "[models][logistic][edge]") {
    // The likelihood is maximised by pushing the intercept to infinity.
    // Refusing is honest; iterating would produce enormous coefficients that
    // predict the constant class with certainty.
    std::vector<std::vector<double>> rows;
    std::vector<double> y;
    for (std::size_t i = 0; i < 50; ++i) {
        rows.push_back({static_cast<double>(i)});
        y.push_back(1.0);
    }
    const auto m = matrix_from(rows, {"x"});
    auto training = make_training_data(m, y, all_rows(50));

    LogisticRegression logit;
    auto r = logit.fit(*training);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().message.find("both classes") != std::string::npos);
}

TEST_CASE("logistic refuses non-binary targets", "[models][logistic][validation]") {
    std::vector<std::vector<double>> rows;
    std::vector<double> y;
    for (std::size_t i = 0; i < 50; ++i) {
        rows.push_back({static_cast<double>(i)});
        y.push_back(static_cast<double>(i) * 0.1);  // continuous
    }
    const auto m = matrix_from(rows, {"x"});
    auto training = make_training_data(m, y, all_rows(50));
    LogisticRegression logit;
    REQUIRE_FALSE(logit.fit(*training).has_value());
}

TEST_CASE("logistic converges deterministically", "[models][logistic][determinism]") {
    // IRLS starts from zero and is a pure function of the data: no random
    // initialisation, no learning rate.
    std::vector<std::vector<double>> rows;
    std::vector<double> y;
    for (std::size_t i = 0; i < 200; ++i) {
        const double x = std::sin(static_cast<double>(i) * 0.1);
        rows.push_back({x});
        y.push_back(x > 0.0 ? 1.0 : 0.0);
    }
    const auto m = matrix_from(rows, {"x"});
    auto training = make_training_data(m, y, all_rows(200));

    LogisticRegression a;
    LogisticRegression b;
    REQUIRE(a.fit(*training).has_value());
    REQUIRE(b.fit(*training).has_value());
    REQUIRE(a.parameter_hash() == b.parameter_hash());
    REQUIRE(a.iterations_used() == b.iterations_used());
}

// ---------------------------------------------------------------------------
// Serialization
// ---------------------------------------------------------------------------

TEST_CASE("models round-trip through serialization", "[models][serialization][determinism]") {
    const auto data = exact_linear();
    auto training = make_training_data(data.features, data.targets, all_rows(200));

    LinearRegression original;
    REQUIRE(original.fit(*training).has_value());

    std::stringstream buffer;
    REQUIRE(original.save(buffer).has_value());

    LinearRegression restored;
    REQUIRE(restored.load(buffer).has_value());
    REQUIRE(restored.fitted());
    // Bit-identical parameters, not merely close.
    REQUIRE(restored.parameter_hash() == original.parameter_hash());

    const std::vector<double> probe{2.5, -1.5};
    REQUIRE(*restored.predict(probe) == *original.predict(probe));
}

TEST_CASE("serialization is byte-identical for an identical fit",
          "[models][serialization][determinism]") {
    const auto data = exact_linear();
    auto training = make_training_data(data.features, data.targets, all_rows(200));

    const auto serialise = [&training] {
        LinearRegression m;
        REQUIRE(m.fit(*training).has_value());
        std::stringstream ss;
        REQUIRE(m.save(ss).has_value());
        return ss.str();
    };
    REQUIRE(serialise() == serialise());
}

TEST_CASE("a corrupt model file is detected", "[models][serialization][validation]") {
    // A corrupt model would predict confidently and wrongly.
    const auto data = exact_linear();
    auto training = make_training_data(data.features, data.targets, all_rows(200));
    LinearRegression m;
    REQUIRE(m.fit(*training).has_value());

    std::stringstream ss;
    REQUIRE(m.save(ss).has_value());
    std::string bytes = ss.str();
    bytes[bytes.size() / 2] = static_cast<char>(bytes[bytes.size() / 2] ^ 0x5A);

    std::stringstream corrupt{bytes};
    LinearRegression restored;
    auto r = restored.load(corrupt);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().message.find("hash mismatch") != std::string::npos);

    std::stringstream garbage{std::string{"not a model at all"}};
    LinearRegression other;
    REQUIRE_FALSE(other.load(garbage).has_value());
}

TEST_CASE("logistic round-trips too", "[models][serialization]") {
    std::vector<std::vector<double>> rows;
    std::vector<double> y;
    for (std::size_t i = 0; i < 100; ++i) {
        const double x = -2.0 + static_cast<double>(i) * 0.04;
        rows.push_back({x});
        y.push_back(x > 0.0 ? 1.0 : 0.0);
    }
    const auto m = matrix_from(rows, {"x"});
    auto training = make_training_data(m, y, all_rows(100));

    LogisticRegression original;
    REQUIRE(original.fit(*training).has_value());
    std::stringstream ss;
    REQUIRE(original.save(ss).has_value());

    LogisticRegression restored;
    REQUIRE(restored.load(ss).has_value());
    REQUIRE(restored.parameter_hash() == original.parameter_hash());
}

// ---------------------------------------------------------------------------
// Rule baseline
// ---------------------------------------------------------------------------

TEST_CASE("the rule baseline is a permanent benchmark", "[models][baseline]") {
    // The most useful entry in the model progression, and the one most often
    // skipped. If ridge does not beat this after costs, the ML adds nothing.
    std::vector<std::vector<double>> rows;
    std::vector<double> y;
    for (std::size_t i = 0; i < 100; ++i) {
        const double signal = std::sin(static_cast<double>(i) * 0.1);
        rows.push_back({signal});
        y.push_back(signal);
    }
    const auto m = matrix_from(rows, {"signal"});
    auto training = make_training_data(m, y, all_rows(100));

    RuleBaselineConfig cfg;
    cfg.signal_column = 0;
    cfg.direction = -1.0;  // reversal
    RuleBaseline rule{cfg};
    REQUIRE(rule.fit(*training).has_value());

    // A reversal rule inverts the sign of the signal.
    auto up = rule.predict(std::vector<double>{1.0});
    REQUIRE(up.has_value());
    REQUIRE(*up < 0.0);

    std::stringstream ss;
    REQUIRE(rule.save(ss).has_value());
    RuleBaseline restored;
    REQUIRE(restored.load(ss).has_value());
    REQUIRE(*restored.predict(std::vector<double>{1.0}) == *up);
}
