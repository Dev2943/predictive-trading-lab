#include <algorithm>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <numeric>
#include <random>
#include <set>
#include <vector>

#include "ptl/validation/walk_forward.hpp"
#include "support/ptl_catch.hpp"

using namespace ptl;
using namespace ptl::validation;
using namespace std::chrono;

namespace {

Timestamp at(const char* iso) {
    Timestamp ts{};
    REQUIRE(parse_timestamp(iso, ts));
    return ts;
}

/// `n` observations one minute apart, each with a label spanning `horizon` bars.
std::vector<ObservationInterval> intervals(std::size_t n, std::size_t horizon) {
    std::vector<ObservationInterval> out;
    out.reserve(n);
    const Timestamp origin = at("2024-01-02T14:00:00Z");
    for (std::size_t i = 0; i < n; ++i) {
        ObservationInterval iv;
        iv.sample_start_time = origin;
        iv.feature_end_time = origin + minutes{static_cast<long>(i)};
        iv.label_start_time = iv.feature_end_time;
        iv.label_end_time = origin + minutes{static_cast<long>(i + horizon)};
        out.push_back(iv);
    }
    return out;
}

WalkForwardConfig small_config() {
    WalkForwardConfig cfg;
    cfg.train_size = 200;
    cfg.validation_size = 50;
    cfg.test_size = 50;
    cfg.step = 50;
    cfg.min_train_rows = 10;
    return cfg;
}

}  // namespace

TEST_CASE("folds have three disjoint sets", "[validation][walkforward]") {
    // TRAIN, VALIDATION and TEST -- not two. Collapsing validation into test
    // means the reported out-of-sample number was the one being optimised
    // against, and is therefore in-sample.
    auto folds = WalkForwardValidator{small_config()}.split(intervals(600, 1));
    REQUIRE(folds.has_value());
    REQUIRE_FALSE(folds->empty());

    for (const auto& f : *folds) {
        INFO(f.describe());
        REQUIRE(f.disjoint());
        REQUIRE_FALSE(f.train_rows.empty());
        REQUIRE_FALSE(f.validation_rows.empty());
        REQUIRE_FALSE(f.test_rows.empty());
        // Strict ordering: train precedes validation precedes test.
        REQUIRE(f.train_rows.back() < f.validation_rows.front());
        REQUIRE(f.validation_rows.back() < f.test_rows.front());
    }
}

TEST_CASE("purging removes training rows whose label interval overlaps a later set",
          "[validation][walkforward][leakage]") {
    // THE NAMED TEST (test_purge_interval_overlap).
    //
    // With a horizon longer than the step, a training row whose feature_end_time
    // sits comfortably before the test window can still have a LABEL that
    // reaches into it -- and that label encodes test-period outcomes. An
    // endpoint comparison would miss exactly these rows.
    auto cfg = small_config();
    cfg.purge = true;

    // Horizon 30 with a step of 50: the last 30 training rows have labels that
    // extend into the validation window.
    auto folds = WalkForwardValidator{cfg}.split(intervals(600, 30));
    REQUIRE(folds.has_value());
    REQUIRE_FALSE(folds->empty());

    const auto& f = folds->front();
    REQUIRE_FALSE(f.purged_rows.empty());

    // Every purged row must genuinely overlap; nothing is dropped needlessly.
    const auto ivs = intervals(600, 30);
    for (const auto i : f.purged_rows) {
        INFO("purged row " << i);
        const bool overlaps_val =
            ivs[i].label_overlaps(f.validation_begin, ivs[f.validation_rows.back()].label_end_time);
        const bool overlaps_test =
            ivs[i].label_overlaps(f.test_begin, ivs[f.test_rows.back()].label_end_time);
        REQUIRE((overlaps_val || overlaps_test));
    }

    // And every RETAINED training row must be clean.
    for (const auto i : f.train_rows) {
        INFO("retained row " << i);
        REQUIRE_FALSE(ivs[i].label_overlaps(f.test_begin, ivs[f.test_rows.back()].label_end_time));
    }
}

TEST_CASE("disabling purging demonstrably admits contaminated rows",
          "[validation][walkforward][leakage]") {
    // The negative control. If turning purging off changed nothing, the purge
    // would be proving nothing either.
    auto with = small_config();
    with.purge = true;
    auto without = small_config();
    without.purge = false;

    const auto ivs = intervals(600, 30);
    auto purged = WalkForwardValidator{with}.split(ivs);
    auto unpurged = WalkForwardValidator{without}.split(ivs);
    REQUIRE(purged.has_value());
    REQUIRE(unpurged.has_value());

    REQUIRE(unpurged->front().purged_rows.empty());
    REQUIRE(unpurged->front().train_rows.size() > purged->front().train_rows.size());
}

TEST_CASE("a horizon of one bar purges nothing", "[validation][walkforward]") {
    // Non-overlapping labels cannot contaminate, so the purge must not drop
    // rows needlessly -- that would silently shrink every training set.
    auto folds = WalkForwardValidator{small_config()}.split(intervals(600, 1));
    REQUIRE(folds.has_value());
    for (const auto& f : *folds) {
        REQUIRE(f.purged_rows.empty());
    }
}

TEST_CASE("the embargo drops observations after the test window",
          "[validation][walkforward][leakage]") {
    // Features are autocorrelated across the boundary, so a row starting
    // moments after the test ends still shares information with it.
    auto cfg = small_config();
    cfg.embargo = 25;

    auto folds = WalkForwardValidator{cfg}.split(intervals(600, 1));
    REQUIRE(folds.has_value());
    const auto& f = folds->front();
    REQUIRE(f.embargoed_rows.size() == 25);
    // Embargoed rows sit immediately after the test set.
    REQUIRE(f.embargoed_rows.front() == f.test_rows.back() + 1);

    auto none = small_config();
    none.embargo = 0;
    auto no_embargo = WalkForwardValidator{none}.split(intervals(600, 1));
    REQUIRE(no_embargo->front().embargoed_rows.empty());
}

TEST_CASE("rolling and expanding differ and both are produced", "[validation][walkforward]") {
    // The research requires BOTH to be reported rather than choosing whichever
    // backtests better -- picking the window mode by its Sharpe is itself a
    // form of selection bias.
    auto both = WalkForwardValidator{small_config()}.split_both_modes(intervals(600, 1));
    REQUIRE(both.has_value());
    const auto& [rolling, expanding] = *both;

    REQUIRE_FALSE(rolling.empty());
    REQUIRE(rolling.size() == expanding.size());
    REQUIRE(rolling.front().mode == WindowMode::Rolling);
    REQUIRE(expanding.front().mode == WindowMode::Expanding);

    // By the last fold the expanding window has strictly more training data.
    REQUIRE(expanding.back().train_rows.size() > rolling.back().train_rows.size());
    // Rolling keeps a fixed-size window.
    REQUIRE(rolling.back().train_rows.size() == rolling.front().train_rows.size());
}

TEST_CASE("an unordered observation series is refused", "[validation][walkforward][leakage]") {
    // NEVER SHUFFLE. An unordered input would make a "training" window contain
    // observations from the future.
    auto ivs = intervals(600, 1);
    std::swap(ivs[100], ivs[400]);
    auto r = WalkForwardValidator{small_config()}.split(ivs);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().message.find("chronological") != std::string::npos);
}

TEST_CASE("shuffling the data changes the folds dramatically",
          "[validation][walkforward][leakage]") {
    // THE SHUFFLE-INVARIANCE NEGATIVE TEST. Deliberately destroying temporal
    // order must break the pipeline. If it did not, the pipeline would not be
    // sensitive to the one property financial validation depends on.
    const auto ordered = intervals(600, 30);
    auto folds = WalkForwardValidator{small_config()}.split(ordered);
    REQUIRE(folds.has_value());

    auto shuffled = ordered;
    std::mt19937_64 rng{42};
    std::shuffle(shuffled.begin(), shuffled.end(), rng);

    // The validator REFUSES shuffled input outright rather than producing
    // meaningless folds from it.
    auto r = WalkForwardValidator{small_config()}.split(shuffled);
    REQUIRE_FALSE(r.has_value());
}

TEST_CASE("fold generation is deterministic", "[validation][walkforward][determinism]") {
    const auto ivs = intervals(600, 30);
    const auto run = [&ivs] {
        auto f = WalkForwardValidator{small_config()}.split(ivs);
        REQUIRE(f.has_value());
        std::vector<std::size_t> shape;
        for (const auto& fold : *f) {
            shape.push_back(fold.train_rows.size());
            shape.push_back(fold.validation_rows.size());
            shape.push_back(fold.test_rows.size());
            shape.push_back(fold.purged_rows.size());
        }
        return shape;
    };
    REQUIRE(run() == run());
}

TEST_CASE("a fold with too little training data is skipped", "[validation][walkforward]") {
    // A fold with two hundred rows still produces a model, and that model still
    // produces a number. Skipping is safer than emitting a result nothing
    // downstream would flag.
    auto cfg = small_config();
    cfg.min_train_rows = 100000;
    auto folds = WalkForwardValidator{cfg}.split(intervals(600, 1));
    REQUIRE(folds.has_value());
    REQUIRE(folds->empty());
}

TEST_CASE("the fold config hash covers every parameter", "[validation][determinism]") {
    // The embargo length is predeclared and hashed: choosing one after seeing
    // results is a form of tuning.
    WalkForwardConfig a;
    WalkForwardConfig b = a;
    REQUIRE(a.hash() == b.hash());
    b.embargo = a.embargo + 5;
    REQUIRE(a.hash() != b.hash());
    WalkForwardConfig c = a;
    c.mode = WindowMode::Rolling;
    REQUIRE(a.hash() != c.hash());
    WalkForwardConfig d = a;
    d.purge = !a.purge;
    REQUIRE(a.hash() != d.hash());
}

// ---------------------------------------------------------------------------
// Holdout
// ---------------------------------------------------------------------------

TEST_CASE("a locked holdout refuses access", "[validation][holdout][leakage]") {
    // THE NAMED TEST (test_holdout_guard). The holdout only means anything if
    // touching it is hard and leaves a mark.
    const Timestamp boundary = at("2024-09-01T00:00:00Z");
    HoldoutGuard guard{boundary};

    REQUIRE(guard.check(at("2024-08-31T23:59:00Z")).has_value());
    auto refused = guard.check(at("2024-09-01T00:00:00Z"));
    REQUIRE_FALSE(refused.has_value());
    REQUIRE(refused.error().message.find("locked holdout") != std::string::npos);
    // The message names the boundary, so the reader does not have to go looking.
    REQUIRE(refused.error().message.find("2024-09-01") != std::string::npos);
}

TEST_CASE("unlocking requires a written justification", "[validation][holdout]") {
    // An unlock nobody had to explain is an unlock nobody will remember.
    HoldoutGuard guard{at("2024-09-01T00:00:00Z")};
    REQUIRE_FALSE(guard.unlock("").has_value());
    REQUIRE_FALSE(guard.unlocked());

    REQUIRE(guard.unlock("research design frozen 2024-09-01; final evaluation").has_value());
    REQUIRE(guard.unlocked());
    REQUIRE(guard.check(at("2024-12-01T00:00:00Z")).has_value());
    REQUIRE(guard.justification().find("frozen") != std::string::npos);
}

TEST_CASE("the guard checks label end not just decision time", "[validation][holdout][leakage]") {
    // A label whose horizon extends past the boundary READS HOLDOUT PRICES,
    // even though the decision itself was made before it. Checking only the
    // decision time would let exactly those rows through.
    const Timestamp boundary = at("2024-09-01T00:00:00Z");
    HoldoutGuard guard{boundary};

    ObservationInterval straddling;
    straddling.sample_start_time = at("2024-08-01T00:00:00Z");
    straddling.feature_end_time = at("2024-08-31T23:00:00Z");  // before the boundary
    straddling.label_start_time = straddling.feature_end_time;
    straddling.label_end_time = at("2024-09-01T09:00:00Z");  // after it

    const std::vector<ObservationInterval> ivs{straddling};
    const auto result = guard.filter(ivs);
    REQUIRE(result.withheld == 1);
    REQUIRE(result.allowed_rows.empty());
}

TEST_CASE("filtering reports what it withheld", "[validation][holdout]") {
    // Dropped rather than erroring, so ordinary research code needs no special
    // case -- but the count is reported so the drop is never silent.
    const Timestamp boundary = at("2024-01-02T18:00:00Z");
    HoldoutGuard guard{boundary};

    const auto ivs = intervals(600, 1);
    const auto filtered = guard.filter(ivs);
    REQUIRE(filtered.withheld > 0);
    REQUIRE(filtered.allowed_rows.size() + filtered.withheld == ivs.size());

    // Every allowed row is genuinely outside the holdout.
    for (const auto i : filtered.allowed_rows) {
        REQUIRE_FALSE(guard.is_holdout(ivs[i].label_end_time));
    }
}

TEST_CASE("a fold reaching into the holdout is refused", "[validation][holdout][leakage]") {
    const auto ivs = intervals(600, 30);
    auto folds = WalkForwardValidator{small_config()}.split(ivs);
    REQUIRE(folds.has_value());

    // A boundary early enough that the last fold must cross it.
    HoldoutGuard guard{ivs[400].feature_end_time};
    bool any_refused = false;
    for (const auto& f : *folds) {
        if (!guard.check_fold(f, ivs).has_value()) any_refused = true;
    }
    REQUIRE(any_refused);

    // A boundary past the end of the data refuses nothing.
    HoldoutGuard far{at("2030-01-01T00:00:00Z")};
    for (const auto& f : *folds) {
        REQUIRE(far.check_fold(f, ivs).has_value());
    }
}

TEST_CASE("an unset boundary permits everything", "[validation][holdout][edge]") {
    // Phase 2 has not yet fixed the boundary; an unset guard must not block the
    // whole pipeline.
    HoldoutGuard none{kNoTimestamp};
    REQUIRE_FALSE(none.is_holdout(at("2099-01-01T00:00:00Z")));
    REQUIRE(none.check(at("2099-01-01T00:00:00Z")).has_value());
}
