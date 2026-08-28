#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cmath>
#include <sstream>
#include <vector>

#include "ptl/models/pipeline.hpp"
#include "ptl/research/evaluation.hpp"
#include "ptl/validation/walk_forward.hpp"
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

struct Dataset {
    features::FeatureMatrix features;
    std::vector<double> targets;
    std::vector<ObservationInterval> intervals;
};

/// A series whose scale SHIFTS partway through.
///
/// The shift is what makes a scaler leak detectable: a scaler fitted on the
/// whole sample sees the later regime while standardising the earlier one.
Dataset shifting_dataset(std::size_t n = 1200, std::size_t shift_at = 600) {
    features::FeatureMatrix m{{"x1", "x2"}, 1, 2};
    Dataset d;
    Timestamp t = at("2024-01-02T14:00:00Z");

    for (std::size_t i = 0; i < n; ++i) {
        const double regime = i < shift_at ? 0.0 : 100.0;
        const double x1 = std::sin(static_cast<double>(i) * 0.05) + regime;
        const double x2 = std::cos(static_cast<double>(i) * 0.03);
        const std::vector<double> vals{x1, x2};

        features::FeatureRow fr;
        fr.feature_end_time = t;
        fr.instrument = InstrumentId{0};
        fr.data_version = 1;
        fr.feature_set_id = 2;
        fr.ready_mask = ~0ULL;
        fr.values = vals;
        REQUIRE(m.append(fr).has_value());

        d.targets.push_back(0.5 * x2 + 0.01 * std::sin(static_cast<double>(i) * 0.11));

        ObservationInterval iv;
        iv.sample_start_time = at("2024-01-02T14:00:00Z");
        iv.feature_end_time = t;
        iv.label_start_time = t;
        iv.label_end_time = t + minutes{5};
        d.intervals.push_back(iv);

        t += minutes{1};
    }
    d.features = std::move(m);
    return d;
}

std::vector<std::size_t> range(std::size_t lo, std::size_t hi) {
    std::vector<std::size_t> r;
    for (std::size_t i = lo; i < hi; ++i) r.push_back(i);
    return r;
}

validation::WalkForwardConfig fold_config() {
    validation::WalkForwardConfig cfg;
    cfg.train_size = 300;
    cfg.validation_size = 100;
    cfg.test_size = 100;
    cfg.step = 100;
    cfg.min_train_rows = 50;
    return cfg;
}

}  // namespace

TEST_CASE("scaler statistics come from training rows only", "[models][leakage][scaler]") {
    // THE CLASSIC LEAK. Fitting a scaler on the whole dataset lets every
    // training row see the mean and variance of the test period. It looks
    // harmless -- the scaler is "just preprocessing" -- and it inflates every
    // reported metric.
    const auto data = shifting_dataset();

    // Fit on the FIRST regime only.
    auto train = DesignMatrix::from_features(data.features, range(0, 600));
    REQUIRE(train.has_value());
    StandardScaler scaler;
    REQUIRE(scaler.fit(*train).has_value());

    // The mean must reflect the first regime alone, not the shifted one.
    REQUIRE(scaler.means()[0] == Catch::Approx(0.0).margin(1.0));

    // A scaler fitted on everything sees the shift and reports a mean near 50.
    auto all = DesignMatrix::from_features(data.features, range(0, 1200));
    StandardScaler leaky;
    REQUIRE(leaky.fit(*all).has_value());
    REQUIRE(leaky.means()[0] > 40.0);
    // The two differ enormously: that gap IS the leak, made visible.
    REQUIRE(std::abs(leaky.means()[0] - scaler.means()[0]) > 40.0);
}

TEST_CASE("test rows receive the training statistics unchanged", "[models][leakage][scaler]") {
    // transform() is const, so there is no path by which scoring test data
    // could refit the scaler on the data it is about to score.
    const auto data = shifting_dataset();
    auto train = DesignMatrix::from_features(data.features, range(0, 600));
    StandardScaler scaler;
    REQUIRE(scaler.fit(*train).has_value());

    const double mean_before = scaler.means()[0];
    auto test = DesignMatrix::from_features(data.features, range(600, 1200));
    REQUIRE(scaler.transform(*test).has_value());
    REQUIRE(scaler.means()[0] == mean_before);

    // Standardised with the FIRST regime's statistics, the shifted test rows
    // land far from zero -- which is correct, and is exactly the information a
    // leaky scaler would have hidden.
    REQUIRE(std::abs(test->at(0, 0)) > 5.0);
}

TEST_CASE("a constant feature does not produce infinities", "[models][scaler][edge]") {
    // Zero variance would divide by zero and give inf for every row at once.
    features::FeatureMatrix m{{"constant", "varying"}, 1, 2};
    Timestamp t = at("2024-01-02T14:00:00Z");
    for (std::size_t i = 0; i < 50; ++i) {
        const std::vector<double> vals{7.0, static_cast<double>(i)};
        features::FeatureRow fr;
        fr.feature_end_time = t;
        fr.data_version = 1;
        fr.feature_set_id = 2;
        fr.ready_mask = ~0ULL;
        fr.values = vals;
        REQUIRE(m.append(fr).has_value());
        t += minutes{1};
    }

    auto design = DesignMatrix::from_features(m, range(0, 50));
    StandardScaler scaler;
    REQUIRE(scaler.fit(*design).has_value());
    REQUIRE(scaler.stdevs()[0] == Catch::Approx(1.0));
    REQUIRE(scaler.transform(*design).has_value());
    REQUIRE(design->all_finite());
}

TEST_CASE("the pipeline fits scaler and model from one row set", "[models][leakage][pipeline]") {
    // Bundling is the point: a scaler and a model kept as separate variables
    // can be fitted on different selections and nothing complains.
    const auto data = shifting_dataset();
    Pipeline pipeline{std::make_unique<LinearRegression>(), PipelineConfig{}};

    REQUIRE(pipeline.fit(data.features, data.targets, range(0, 600)).has_value());
    REQUIRE(pipeline.fitted());
    // The scaler inside the pipeline saw only the training rows.
    REQUIRE(pipeline.scaler().means()[0] == Catch::Approx(0.0).margin(1.0));

    auto predictions = pipeline.predict(data.features, range(600, 700));
    REQUIRE(predictions.has_value());
    REQUIRE(predictions->size() == 100);
    for (const double p : *predictions) REQUIRE(is_finite(p));
}

TEST_CASE("each fold gets a freshly fitted model", "[models][leakage][walkforward]") {
    // Reusing a fitted instance would carry the previous fold's parameters into
    // the next fold's starting point -- for IRLS that changes the answer, and
    // for any model it means fold k saw fold k-1's test data through its
    // initialisation.
    const auto data = shifting_dataset();
    auto folds = validation::WalkForwardValidator{fold_config()}.split(data.intervals);
    REQUIRE(folds.has_value());
    REQUIRE(folds->size() > 2);

    WalkForwardRunner runner{std::make_unique<LinearRegression>()};
    auto result = runner.run(data.features, data.targets, *folds);
    REQUIRE(result.has_value());
    REQUIRE(result->folds_fitted == folds->size());

    // Different folds see different data, so their parameters must differ.
    // Identical hashes across folds would mean the model was never refitted.
    bool any_differ = false;
    for (std::size_t i = 1; i < result->per_fold_parameter_hashes.size(); ++i) {
        if (result->per_fold_parameter_hashes[i] != result->per_fold_parameter_hashes[i - 1]) {
            any_differ = true;
        }
    }
    REQUIRE(any_differ);
}

TEST_CASE("out-of-sample predictions come only from test rows", "[models][leakage][walkforward]") {
    // A prediction for a row the model TRAINED on is in-sample and would
    // flatter every metric derived from it.
    const auto data = shifting_dataset();
    auto folds = validation::WalkForwardValidator{fold_config()}.split(data.intervals);
    WalkForwardRunner runner{std::make_unique<LinearRegression>()};
    auto result = runner.run(data.features, data.targets, *folds);
    REQUIRE(result.has_value());

    std::size_t expected = 0;
    for (const auto& f : *folds) expected += f.test_rows.size();
    REQUIRE(result->predictions.size() == expected);

    for (const auto& p : result->predictions) {
        REQUIRE(p.is_test);
        // Each prediction's timestamp must fall inside its fold's test window.
        const auto& fold = (*folds)[static_cast<std::size_t>(p.fold_id)];
        REQUIRE(p.feature_end_time >= fold.test_begin);
        REQUIRE(p.feature_end_time <= fold.test_end);
    }
}

TEST_CASE("a model never sees a row from a later fold", "[models][leakage][walkforward]") {
    // Strict causality across folds: every training row must precede every test
    // row of the same fold.
    const auto data = shifting_dataset();
    auto folds = validation::WalkForwardValidator{fold_config()}.split(data.intervals);
    REQUIRE(folds.has_value());

    for (const auto& f : *folds) {
        INFO(f.describe());
        for (const auto train_row : f.train_rows) {
            REQUIRE(data.intervals[train_row].feature_end_time < f.test_begin);
        }
    }
}

TEST_CASE("two identical runs produce identical predictions",
          "[models][determinism][walkforward]") {
    // THE PHASE 6 DETERMINISM REQUIREMENT. Nothing in the model path carries
    // hidden state, unseeded randomness, or hash-ordered iteration.
    const auto data = shifting_dataset();
    auto folds = validation::WalkForwardValidator{fold_config()}.split(data.intervals);

    const auto execute = [&] {
        WalkForwardRunner runner{std::make_unique<LinearRegression>()};
        auto r = runner.run(data.features, data.targets, *folds);
        REQUIRE(r.has_value());
        return r->content_hash();
    };
    REQUIRE(execute() == execute());
}

TEST_CASE("ridge and OLS produce different but reproducible results",
          "[models][determinism][walkforward]") {
    const auto data = shifting_dataset();
    auto folds = validation::WalkForwardValidator{fold_config()}.split(data.intervals);

    const auto execute = [&](double penalty) {
        LinearConfig cfg;
        cfg.l2_penalty = penalty;
        WalkForwardRunner runner{std::make_unique<LinearRegression>(cfg)};
        auto r = runner.run(data.features, data.targets, *folds);
        REQUIRE(r.has_value());
        return r->content_hash();
    };
    REQUIRE(execute(0.0) == execute(0.0));
    REQUIRE(execute(10.0) == execute(10.0));
    // A different penalty is a different model and must not collide.
    REQUIRE(execute(0.0) != execute(10.0));
}

TEST_CASE("walk-forward output feeds the Phase 5 evaluators", "[models][walkforward][evaluation]") {
    // The integration point: predictions and outcomes go straight into the
    // existing evaluation utilities with no adaptation layer.
    const auto data = shifting_dataset();
    auto folds = validation::WalkForwardValidator{fold_config()}.split(data.intervals);
    WalkForwardRunner runner{std::make_unique<LinearRegression>()};
    auto result = runner.run(data.features, data.targets, *folds);
    REQUIRE(result.has_value());

    const auto predictions = result->prediction_values();
    const auto actuals = result->actual_values();

    auto metrics = research::Evaluator::regression(predictions, actuals);
    REQUIRE(metrics.has_value());
    REQUIRE(is_finite(metrics->r_squared));
    REQUIRE(is_finite(metrics->rank_information_coefficient));

    auto ranking = research::Evaluator::ranking(predictions, actuals, 10);
    REQUIRE(ranking.has_value());
    REQUIRE(ranking->decile_means.size() == 10);
}

TEST_CASE("a pipeline round-trips with its scaler", "[models][serialization][leakage]") {
    // A model reloaded WITHOUT its scaler would receive unstandardised inputs
    // and produce confident nonsense, so the two are serialised together.
    const auto data = shifting_dataset();
    Pipeline original{std::make_unique<LinearRegression>()};
    REQUIRE(original.fit(data.features, data.targets, range(0, 600)).has_value());

    std::stringstream buffer;
    REQUIRE(original.save(buffer).has_value());

    Pipeline restored{std::make_unique<LinearRegression>()};
    REQUIRE(restored.load(buffer).has_value());
    REQUIRE(restored.parameter_hash() == original.parameter_hash());

    auto a = original.predict(data.features, range(600, 650));
    auto b = restored.predict(data.features, range(600, 650));
    REQUIRE(a.has_value());
    REQUIRE(b.has_value());
    for (std::size_t i = 0; i < a->size(); ++i) {
        // Bit-identical, not approximately equal.
        REQUIRE((*a)[i] == (*b)[i]);
    }
}

TEST_CASE("a run with no fittable fold fails rather than reporting nothing",
          "[models][walkforward][edge]") {
    // A run that quietly evaluated zero folds would report an empty result that
    // looks like a clean pass.
    const auto data = shifting_dataset(100, 50);
    std::vector<validation::Fold> empty_folds{validation::Fold{}};
    WalkForwardRunner runner{std::make_unique<LinearRegression>()};
    auto r = runner.run(data.features, data.targets, empty_folds);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().message.find("no fold could be fitted") != std::string::npos);
}

TEST_CASE("a target series of the wrong length is refused", "[models][walkforward][leakage]") {
    // A length mismatch is the misalignment that looks like a signal and is
    // entirely lookahead.
    const auto data = shifting_dataset();
    auto folds = validation::WalkForwardValidator{fold_config()}.split(data.intervals);
    WalkForwardRunner runner{std::make_unique<LinearRegression>()};

    std::vector<double> short_targets(data.targets.begin(), data.targets.begin() + 100);
    REQUIRE_FALSE(runner.run(data.features, short_targets, *folds).has_value());
}
