#include <algorithm>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <vector>

#include "ptl/research/evaluation.hpp"
#include "ptl/research/trial.hpp"
#include "support/ptl_catch.hpp"

using namespace ptl;
using namespace ptl::research;
using namespace std::chrono;

namespace {

Timestamp at(const char* iso) {
    Timestamp ts{};
    REQUIRE(parse_timestamp(iso, ts));
    return ts;
}

struct TempDb {
    std::filesystem::path path;
    TempDb() {
        const auto stamp = static_cast<unsigned long long>(
            std::chrono::steady_clock::now().time_since_epoch().count());
        path = std::filesystem::temp_directory_path() /
               ("ptl_research_" + std::to_string(stamp) + ".sqlite");
        std::filesystem::remove(path);
    }
    ~TempDb() {
        std::error_code ec;
        std::filesystem::remove(path, ec);
        std::filesystem::remove(path.string() + "-wal", ec);
        std::filesystem::remove(path.string() + "-shm", ec);
    }
    TempDb(const TempDb&) = delete;
    TempDb& operator=(const TempDb&) = delete;
};

TrialManifest sample_manifest() {
    TrialManifest m;
    m.research_question = "does 15m reversal survive costs";
    m.hypothesis = "ridge beats the rule baseline net of spread";
    m.data_version = 0x1111;
    m.feature_set_id = 0x2222;
    m.label_config_hash = 0x3333;
    m.fold_config_hash = 0x4444;
    m.model_config_hash = 0x5555;
    m.git_sha = "deadbeef";
    m.seed = 20240101;
    m.holdout_boundary = "2024-09-01";
    return m;
}

}  // namespace

// ---------------------------------------------------------------------------
// Evaluation
// ---------------------------------------------------------------------------

TEST_CASE("regression metrics on a known fixture", "[research][evaluation]") {
    const std::vector<double> pred{1.0, 2.0, 3.0, 4.0};
    const std::vector<double> actual{1.0, 2.0, 3.0, 4.0};
    auto m = Evaluator::regression(pred, actual);
    REQUIRE(m.has_value());
    REQUIRE(m->mse == Catch::Approx(0.0));
    REQUIRE(m->r_squared == Catch::Approx(1.0));
    REQUIRE(m->information_coefficient == Catch::Approx(1.0));
    REQUIRE(m->rank_information_coefficient == Catch::Approx(1.0));
}

TEST_CASE("out-of-sample R-squared may be negative", "[research][evaluation][edge]") {
    // NOT clamped at zero. A negative out-of-sample R-squared means the model
    // is worse than predicting the mean, which is a real and important finding
    // -- hiding it would flatter every failed experiment.
    const std::vector<double> pred{10.0, -10.0, 10.0, -10.0};
    const std::vector<double> actual{1.0, 2.0, 3.0, 4.0};
    auto m = Evaluator::regression(pred, actual);
    REQUIRE(m.has_value());
    REQUIRE(m->r_squared < 0.0);
    REQUIRE(is_finite(m->r_squared));
}

TEST_CASE("rank IC is robust where Pearson IC is not", "[research][evaluation]") {
    // The statistic to trust when the two disagree: returns are heavy-tailed,
    // and one outlier can dominate a Pearson correlation entirely.
    std::vector<double> pred{1.0, 2.0, 3.0, 4.0, 5.0};
    std::vector<double> actual{1.0, 2.0, 3.0, 4.0, 5.0};
    auto clean = Evaluator::regression(pred, actual);
    REQUIRE(clean->rank_information_coefficient == Catch::Approx(1.0));

    // One extreme outlier breaks the linear relationship but not the ordering.
    actual.back() = 1000.0;
    auto outlier = Evaluator::regression(pred, actual);
    REQUIRE(outlier->rank_information_coefficient == Catch::Approx(1.0));
    REQUIRE(outlier->information_coefficient < 1.0);
}

TEST_CASE("correlation of a constant series is zero not infinite", "[research][evaluation][edge]") {
    const std::vector<double> constant{5.0, 5.0, 5.0, 5.0};
    const std::vector<double> varying{1.0, 2.0, 3.0, 4.0};
    REQUIRE(Evaluator::pearson(constant, varying) == Catch::Approx(0.0));
    REQUIRE(is_finite(Evaluator::pearson(constant, varying)));
    REQUIRE(is_finite(Evaluator::spearman(constant, varying)));
}

TEST_CASE("classification metrics and AUC", "[research][evaluation]") {
    const std::vector<double> pred{0.9, 0.8, 0.3, 0.1};
    const std::vector<double> actual{1.0, 1.0, 0.0, 0.0};
    auto m = Evaluator::classification(pred, actual, 0.5);
    REQUIRE(m.has_value());
    REQUIRE(m->true_positives == 2);
    REQUIRE(m->true_negatives == 2);
    REQUIRE(m->accuracy == Catch::Approx(1.0));
    REQUIRE(m->auc == Catch::Approx(1.0));  // perfect separation
}

TEST_CASE("AUC is 0.5 when one class is absent", "[research][evaluation][edge]") {
    // Undefined, and 0.5 is the honest "no information" value rather than a
    // flattering 1.0.
    const std::vector<double> pred{0.9, 0.8, 0.7};
    const std::vector<double> all_positive{1.0, 1.0, 1.0};
    auto m = Evaluator::classification(pred, all_positive);
    REQUIRE(m->auc == Catch::Approx(0.5));
}

TEST_CASE("calibration error catches a well-ranked but badly scaled model",
          "[research][evaluation]") {
    // A model can rank perfectly and still be badly calibrated, and position
    // sizing depends on the probability being real.
    std::vector<double> pred;
    std::vector<double> actual;
    for (int i = 0; i < 100; ++i) {
        // Always predicts 0.9 but is right only half the time.
        pred.push_back(0.9);
        actual.push_back(i % 2 == 0 ? 1.0 : 0.0);
    }
    auto m = Evaluator::classification(pred, actual);
    REQUIRE(m->calibration_error == Catch::Approx(0.4).margin(0.01));
}

TEST_CASE("decile monotonicity is the legible diagnostic", "[research][evaluation]") {
    // A monotone staircase is a working signal; anything else is visible at a
    // glance.
    std::vector<double> pred;
    std::vector<double> actual;
    for (int i = 0; i < 100; ++i) {
        pred.push_back(static_cast<double>(i));
        actual.push_back(static_cast<double>(i) * 0.01);
    }
    auto r = Evaluator::ranking(pred, actual, 10);
    REQUIRE(r.has_value());
    REQUIRE(r->monotone);
    REQUIRE(r->long_short_spread > 0.0);
    REQUIRE(r->decile_means.size() == 10);
    REQUIRE(r->spearman == Catch::Approx(1.0));

    // A signal with no relationship is not monotone.
    std::reverse(actual.begin(), actual.end());
    auto inverted = Evaluator::ranking(pred, actual, 10);
    REQUIRE_FALSE(inverted->monotone);
    REQUIRE(inverted->long_short_spread < 0.0);
}

TEST_CASE("cross-sectional IC averages per period not pooled", "[research][evaluation]") {
    // Pooling would let a few large cross-sections dominate and would confound
    // cross-sectional skill with time-series skill.
    std::vector<double> pred;
    std::vector<double> actual;
    std::vector<std::int64_t> periods;
    for (std::int64_t p = 0; p < 20; ++p) {
        for (int i = 0; i < 9; ++i) {
            pred.push_back(static_cast<double>(i));
            actual.push_back(static_cast<double>(i) * 0.01);
            periods.push_back(p);
        }
    }
    auto e = Evaluator::cross_sectional(pred, actual, periods, 252.0);
    REQUIRE(e.has_value());
    REQUIRE(e->periods == 20);
    REQUIRE(e->mean_ic == Catch::Approx(1.0));
    REQUIRE(e->hit_rate == Catch::Approx(1.0));
    // Perfectly stable IC means zero dispersion, so ICIR is defined as zero
    // rather than infinite.
    REQUIRE(is_finite(e->icir));
}

TEST_CASE("a single-name cross-section is skipped not counted", "[research][evaluation][edge]") {
    // A cross-section of one has no dispersion and no meaningful IC.
    const std::vector<double> pred{1.0, 2.0};
    const std::vector<double> actual{1.0, 2.0};
    const std::vector<std::int64_t> periods{0, 1};
    auto e = Evaluator::cross_sectional(pred, actual, periods);
    REQUIRE(e.has_value());
    REQUIRE(e->periods == 0);
}

TEST_CASE("permutation importance is signed", "[research][evaluation]") {
    const std::vector<double> actual{1.0, 2.0, 3.0, 4.0, 5.0};
    const std::vector<double> good{1.0, 2.0, 3.0, 4.0, 5.0};
    const std::vector<double> shuffled{5.0, 1.0, 4.0, 2.0, 3.0};

    auto important = Evaluator::permutation_importance(good, actual, shuffled);
    REQUIRE(important.has_value());
    // Positive means shuffling HURT, so the model was using the feature.
    REQUIRE(*important > 0.0);
}

TEST_CASE("mismatched series lengths are refused", "[research][evaluation]") {
    REQUIRE_FALSE(Evaluator::regression({{1.0, 2.0}}, {{1.0}}).has_value());
    REQUIRE_FALSE(Evaluator::regression({}, {}).has_value());
}

// ---------------------------------------------------------------------------
// Trial manifests
// ---------------------------------------------------------------------------

TEST_CASE("the trial id is reproducible and covers every input", "[research][trial][determinism]") {
    const auto base = sample_manifest();
    REQUIRE(base.trial_id() == sample_manifest().trial_id());
    REQUIRE(base.trial_id_hex().size() == 16);

    // Each of the five hashes must move the id, or two different experiments
    // would collide and the trial count would be wrong.
    for (int field = 0; field < 5; ++field) {
        auto m = base;
        switch (field) {
            case 0:
                m.data_version ^= 1;
                break;
            case 1:
                m.feature_set_id ^= 1;
                break;
            case 2:
                m.label_config_hash ^= 1;
                break;
            case 3:
                m.fold_config_hash ^= 1;
                break;
            default:
                m.model_config_hash ^= 1;
                break;
        }
        INFO("field " << field);
        REQUIRE(m.trial_id() != base.trial_id());
    }

    auto seeded = base;
    seeded.seed = base.seed + 1;
    REQUIRE(seeded.trial_id() != base.trial_id());
}

TEST_CASE("parameter insertion order does not change the trial id",
          "[research][trial][determinism]") {
    auto a = sample_manifest();
    a.parameters["lambda"] = "0.1";
    a.parameters["alpha"] = "0.5";

    auto b = sample_manifest();
    b.parameters["alpha"] = "0.5";
    b.parameters["lambda"] = "0.1";

    REQUIRE(a.trial_id() == b.trial_id());
}

TEST_CASE("duplicate trials are detected and not double counted", "[research][trial][validation]") {
    // A registry that double-counts a re-run inflates the trial count and
    // over-deflates every Sharpe derived from it.
    TempDb db;
    auto reg = experiments::Registry::open(db.path);
    REQUIRE(reg.has_value());
    ResearchValidator validator{*reg};

    const auto manifest = sample_manifest();
    experiments::RunRecord run;
    run.run_id = manifest.trial_id_hex();
    run.git_sha = "deadbeef";
    run.git_dirty = "clean";
    run.config_hash = "abc";
    run.config_canonical = manifest.canonical();
    run.data_manifest_sha = "sha";
    run.seed = manifest.seed;
    run.compiler = "GNU";
    run.compiler_version = "13";
    run.build_type = "Debug";
    REQUIRE(reg->insert_run(run).has_value());

    auto report = validator.validate(manifest);
    REQUIRE(report.has_value());
    bool saw_duplicate = false;
    for (const auto& i : report->issues) {
        if (i.code == ResearchIssueCode::DuplicateTrial) saw_duplicate = true;
    }
    REQUIRE(saw_duplicate);

    // Registering it again must NOT insert a second trial.
    auto id = validator.register_trial(manifest, run.run_id);
    REQUIRE(id.has_value());
    REQUIRE(*id == 0);
    REQUIRE(*reg->trial_count(manifest.research_question) == 0);
}

TEST_CASE("an undeclared search budget is reported", "[research][trial][validation]") {
    // Not fatal -- exploratory work is legitimate -- but any Sharpe from this
    // question must be reported as uncorrected.
    TempDb db;
    auto reg = experiments::Registry::open(db.path);
    ResearchValidator validator{*reg};

    auto report = validator.validate(sample_manifest());
    REQUIRE(report.has_value());
    bool saw = false;
    for (const auto& i : report->issues) {
        if (i.code == ResearchIssueCode::UndeclaredBudget) saw = true;
    }
    REQUIRE(saw);
    REQUIRE(report->ok());  // a warning, not fatal
}

TEST_CASE("a stale artifact is fatal", "[research][trial][validation]") {
    // A cached matrix from an older definition has values that no longer match
    // their column names, so a model trained on it learns relationships between
    // the wrong things.
    const auto stale = ResearchValidator::check_artifact_freshness(0xAAAA, 0xBBBB, 0x1111, 0x1111);
    REQUIRE(stale.fatal);
    REQUIRE(stale.detail.find("no longer match their names") != std::string::npos);

    const auto old_data =
        ResearchValidator::check_artifact_freshness(0xAAAA, 0xAAAA, 0x1111, 0x2222);
    REQUIRE(old_data.fatal);

    const auto fresh = ResearchValidator::check_artifact_freshness(0xAAAA, 0xAAAA, 0x1111, 0x1111);
    REQUIRE_FALSE(fresh.fatal);
}

TEST_CASE("feature and label misalignment is fatal", "[research][trial][leakage]") {
    // A one-row offset LOOKS like a signal and is entirely lookahead: row i's
    // features would be paired with row i+1's outcome.
    const std::vector<Timestamp> features{at("2024-07-02T14:00:00Z"), at("2024-07-02T14:01:00Z"),
                                          at("2024-07-02T14:02:00Z")};
    REQUIRE_FALSE(ResearchValidator::check_alignment(features, features).fatal);

    const std::vector<Timestamp> offset{at("2024-07-02T14:01:00Z"), at("2024-07-02T14:02:00Z"),
                                        at("2024-07-02T14:03:00Z")};
    const auto misaligned = ResearchValidator::check_alignment(features, offset);
    REQUIRE(misaligned.fatal);
    REQUIRE(misaligned.detail.find("row 0") != std::string::npos);

    const std::vector<Timestamp> shorter{at("2024-07-02T14:00:00Z")};
    REQUIRE(ResearchValidator::check_alignment(features, shorter).fatal);
}

TEST_CASE("a non-deterministic result invalidates the registry", "[research][trial][determinism]") {
    TempDb db;
    auto reg = experiments::Registry::open(db.path);
    ResearchValidator validator{*reg};
    const auto manifest = sample_manifest();

    REQUIRE(validator.verify_determinism(manifest, 0xABCD, 0xABCD).has_value());

    auto mismatch = validator.verify_determinism(manifest, 0xABCD, 0x1234);
    REQUIRE_FALSE(mismatch.has_value());
    // The message must convey the scope of the problem: it is not one bad run.
    REQUIRE(mismatch.error().message.find("Every recorded result is now suspect") !=
            std::string::npos);
}

// ---------------------------------------------------------------------------
// Multiple testing
// ---------------------------------------------------------------------------

TEST_CASE("expected max Sharpe grows with the trial count", "[research][trial][statistics]") {
    // After enough trials, an impressive Sharpe is the expected maximum of
    // noise. This is the benchmark an observed value must beat.
    REQUIRE(expected_max_sharpe(1) == Catch::Approx(0.0));
    const double ten = expected_max_sharpe(10);
    const double hundred = expected_max_sharpe(100);
    const double thousand = expected_max_sharpe(1000);
    REQUIRE(ten > 0.0);
    REQUIRE(hundred > ten);
    REQUIRE(thousand > hundred);
    REQUIRE(is_finite(thousand));
}

TEST_CASE("the deflated Sharpe falls as trials accumulate", "[research][trial][statistics]") {
    // The same observed Sharpe means less after a hundred attempts than after
    // one. Reporting it without the correction is the single most common way a
    // backtest misleads.
    constexpr double kSharpe = 1.5;
    constexpr std::size_t kObs = 1000;

    const double few = deflated_sharpe_ratio(kSharpe, 2, kObs);
    const double many = deflated_sharpe_ratio(kSharpe, 500, kObs);
    REQUIRE(few > many);
    REQUIRE(few <= 1.0);
    REQUIRE(many >= 0.0);
    REQUIRE(is_finite(many));

    // A stronger result survives more trials.
    REQUIRE(deflated_sharpe_ratio(3.0, 500, kObs) > deflated_sharpe_ratio(1.0, 500, kObs));
}

TEST_CASE("the deflated Sharpe accounts for non-normality", "[research][trial][statistics]") {
    // Ignoring skew and kurtosis understates the estimator's variance, which
    // flatters the result exactly when returns are most non-normal.
    // A Sharpe that actually CLEARS the expected maximum for this trial count.
    // With unit cross-trial dispersion the expected max over 50 trials is
    // around 2.2, so an observed 1.5 deflates to zero for any skew -- correct,
    // but not a discriminating test of the non-normality adjustment.
    constexpr double kSharpe = 3.0;
    const double normal = deflated_sharpe_ratio(kSharpe, 50, 1000, 0.0, 3.0);
    const double fat_tailed = deflated_sharpe_ratio(kSharpe, 50, 1000, -1.0, 8.0);
    REQUIRE(fat_tailed < normal);
    REQUIRE(is_finite(fat_tailed));
}

TEST_CASE("degenerate inputs to the deflated Sharpe stay finite", "[research][trial][edge]") {
    REQUIRE(is_finite(deflated_sharpe_ratio(1.0, 0, 1000)));
    REQUIRE(is_finite(deflated_sharpe_ratio(1.0, 100, 1)));
    REQUIRE(is_finite(deflated_sharpe_ratio(0.0, 100, 1000)));
}
