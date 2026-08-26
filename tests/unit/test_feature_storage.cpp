#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <limits>
#include <vector>

#include "ptl/features/factor.hpp"
#include "ptl/features/matrix.hpp"
#include "ptl/features/validation.hpp"
#include "support/ptl_catch.hpp"

using namespace ptl;
using namespace ptl::features;
using namespace std::chrono;

namespace {

Timestamp at(const char* iso) {
    Timestamp ts{};
    REQUIRE(parse_timestamp(iso, ts));
    return ts;
}

constexpr InstrumentId kSpy{0};

FeatureMatrix build_matrix(std::size_t rows = 5, std::uint64_t data_version = 111,
                           std::uint64_t set_id = 222) {
    FeatureMatrix m{{"alpha", "beta", "gamma"}, data_version, set_id};
    Timestamp t = at("2024-07-02T14:00:00Z");
    for (std::size_t i = 0; i < rows; ++i) {
        const std::vector<double> vals{static_cast<double>(i), static_cast<double>(i) * 2.0,
                                       std::sin(static_cast<double>(i))};
        FeatureRow r;
        r.feature_end_time = t;
        r.instrument = kSpy;
        r.data_version = data_version;
        r.feature_set_id = set_id;
        r.ready_mask = i >= 2 ? 0b111 : 0b001;
        r.values = vals;
        REQUIRE(m.append(r).has_value());
        t += minutes{1};
    }
    return m;
}

std::filesystem::path temp_path(const char* name) {
    return std::filesystem::temp_directory_path() / name;
}

}  // namespace

// ---------------------------------------------------------------------------
// Matrix
// ---------------------------------------------------------------------------

TEST_CASE("the matrix stores columns contiguously", "[features][matrix]") {
    // Column-major because a model standardises one feature across all rows,
    // and a solver expects columns.
    const auto m = build_matrix();
    REQUIRE(m.rows() == 5);
    REQUIRE(m.cols() == 3);
    REQUIRE(m.column(0).size() == 5);
    REQUIRE(m.column(1)[3] == Catch::Approx(6.0));
    REQUIRE(m.at(3, 1) == Catch::Approx(6.0));
    REQUIRE(m.index_of_name("beta") == 1);
    REQUIRE(m.index_of_name("absent") == static_cast<std::size_t>(-1));
}

TEST_CASE("ready_rows applies the warmup gate at selection time", "[features][matrix][leakage]") {
    // An unready row is never handed to a model, rather than handed over and
    // hopefully ignored.
    const auto m = build_matrix();
    REQUIRE(m.ready_rows(0b111).size() == 3);  // rows 2..4
    REQUIRE(m.ready_rows(0b001).size() == 5);  // every row has bit 0
    REQUIRE(m.ready_rows(0).size() == 5);
}

TEST_CASE("mismatched rows are refused", "[features][matrix][validation]") {
    FeatureMatrix m{{"a", "b"}, 111, 222};
    const std::vector<double> wrong_width{1.0};
    FeatureRow r;
    r.feature_end_time = at("2024-07-02T14:00:00Z");
    r.data_version = 111;
    r.feature_set_id = 222;
    r.values = wrong_width;
    // A truncated row would shift every later column and produce a matrix whose
    // names no longer describe its values.
    REQUIRE_FALSE(m.append(r).has_value());

    const std::vector<double> ok{1.0, 2.0};
    r.values = ok;
    r.feature_set_id = 999;  // different definition
    REQUIRE_FALSE(m.append(r).has_value());
    r.feature_set_id = 222;
    r.data_version = 999;  // different dataset
    REQUIRE_FALSE(m.append(r).has_value());
    r.data_version = 111;
    REQUIRE(m.append(r).has_value());

    // Out-of-order rows would corrupt every time-ordered fold.
    r.feature_end_time = at("2024-07-02T13:00:00Z");
    REQUIRE_FALSE(m.append(r).has_value());
}

TEST_CASE("a matrix round-trips through serialization", "[features][matrix][determinism]") {
    const auto m = build_matrix();
    const auto path = temp_path("ptl_matrix_roundtrip.fmx");
    std::filesystem::remove(path);

    REQUIRE(m.save(path).has_value());
    auto loaded = FeatureMatrix::load(path);
    REQUIRE(loaded.has_value());

    REQUIRE(loaded->rows() == m.rows());
    REQUIRE(loaded->cols() == m.cols());
    REQUIRE(loaded->data_version() == m.data_version());
    REQUIRE(loaded->feature_set_id() == m.feature_set_id());
    // Bit-identical, not approximately equal.
    REQUIRE(loaded->content_hash() == m.content_hash());
    for (std::size_t j = 0; j < m.cols(); ++j) {
        for (std::size_t i = 0; i < m.rows(); ++i) {
            REQUIRE(loaded->at(i, j) == m.at(i, j));
        }
    }
    std::filesystem::remove(path);
}

TEST_CASE("a corrupt matrix file is detected not silently loaded",
          "[features][matrix][validation]") {
    // A silent corruption would train a model on values nobody produced.
    const auto m = build_matrix();
    const auto path = temp_path("ptl_matrix_corrupt.fmx");
    std::filesystem::remove(path);
    REQUIRE(m.save(path).has_value());

    // Flip a byte deep in the value block.
    {
        std::fstream f{path, std::ios::binary | std::ios::in | std::ios::out};
        f.seekp(-8, std::ios::end);
        const char junk[8] = {1, 2, 3, 4, 5, 6, 7, 8};
        f.write(junk, 8);
    }
    auto loaded = FeatureMatrix::load(path);
    REQUIRE_FALSE(loaded.has_value());
    REQUIRE(loaded.error().message.find("hash mismatch") != std::string::npos);

    std::filesystem::remove(path);
    REQUIRE_FALSE(FeatureMatrix::load(temp_path("ptl_does_not_exist.fmx")).has_value());
}

TEST_CASE("the content hash distinguishes a single-ulp difference",
          "[features][matrix][determinism]") {
    // Hashing the bit pattern rather than a rounded decimal, so the
    // determinism test cannot pass over a real divergence.
    auto a = build_matrix();
    FeatureMatrix b{{"alpha", "beta", "gamma"}, 111, 222};
    Timestamp t = at("2024-07-02T14:00:00Z");
    for (std::size_t i = 0; i < 5; ++i) {
        std::vector<double> vals{static_cast<double>(i), static_cast<double>(i) * 2.0,
                                 std::sin(static_cast<double>(i))};
        if (i == 3) vals[0] = std::nextafter(vals[0], 1e9);  // one ulp
        FeatureRow r;
        r.feature_end_time = t;
        r.instrument = kSpy;
        r.data_version = 111;
        r.feature_set_id = 222;
        r.ready_mask = i >= 2 ? 0b111 : 0b001;
        r.values = vals;
        REQUIRE(b.append(r).has_value());
        t += minutes{1};
    }
    REQUIRE(a.content_hash() != b.content_hash());
}

TEST_CASE("the cache key includes the feature-set definition", "[features][matrix][determinism]") {
    // Reusing a cached matrix after a feature definition changed would train a
    // model on values that no longer match their names.
    const auto dir = std::filesystem::temp_directory_path();
    REQUIRE(FeatureMatrix::cache_path(dir, 1, 2) != FeatureMatrix::cache_path(dir, 1, 3));
    REQUIRE(FeatureMatrix::cache_path(dir, 1, 2) != FeatureMatrix::cache_path(dir, 2, 2));
    REQUIRE(FeatureMatrix::cache_path(dir, 1, 2) == FeatureMatrix::cache_path(dir, 1, 2));
}

// ---------------------------------------------------------------------------
// Factor graph
// ---------------------------------------------------------------------------

TEST_CASE("factors evaluate in dependency order", "[features][factor]") {
    FactorGraph g;
    REQUIRE(g.add({"double_a",
                   {"a"},
                   [](const FactorInputs& in) { return in.get("a").value() * 2.0; },
                   0})
                .has_value());
    REQUIRE(g.add({"plus_one",
                   {"double_a"},
                   [](const FactorInputs& in) { return in.get("double_a").value() + 1.0; },
                   0})
                .has_value());
    REQUIRE(g.finalize().has_value());

    // double_a must precede plus_one regardless of registration order.
    const auto order = g.evaluation_order();
    REQUIRE(order.size() == 2);
    REQUIRE(order[0] == "double_a");
    REQUIRE(order[1] == "plus_one");

    auto values = g.evaluate({{"a", 5.0}});
    REQUIRE(values.has_value());
    REQUIRE((*values)["double_a"] == Catch::Approx(10.0));
    REQUIRE((*values)["plus_one"] == Catch::Approx(11.0));
}

TEST_CASE("a dependency cycle is rejected at finalize", "[features][factor]") {
    // A cycle found mid-run would either recurse until the stack died or
    // silently use a stale value from the previous bar.
    FactorGraph g;
    const auto noop = [](const FactorInputs&) { return 1.0; };
    REQUIRE(g.add({"x", {"y"}, noop, 0}).has_value());
    REQUIRE(g.add({"y", {"x"}, noop, 0}).has_value());

    auto r = g.finalize();
    REQUIRE_FALSE(r.has_value());
    // Naming the members turns an abstract failure into a fixable one.
    REQUIRE(r.error().message.find("cycle") != std::string::npos);
    REQUIRE(r.error().message.find("x") != std::string::npos);
    REQUIRE_FALSE(g.finalized());
    REQUIRE_FALSE(g.evaluate({}).has_value());
}

TEST_CASE("reading an undeclared dependency is refused", "[features][factor]") {
    // Reading an undeclared input would work by accident whenever evaluation
    // order happened to cooperate, and break silently when it did not.
    FactorGraph g;
    REQUIRE(g.add({"sneaky",
                   {"a"},
                   [](const FactorInputs& in) {
                       auto undeclared = in.get("b");
                       return undeclared.has_value() ? *undeclared : -1.0;
                   },
                   0})
                .has_value());
    REQUIRE(g.finalize().has_value());

    auto values = g.evaluate({{"a", 1.0}, {"b", 99.0}});
    REQUIRE(values.has_value());
    // 'b' exists in the base values but was never declared, so the read fails
    // and the factor falls back rather than silently succeeding.
    REQUIRE((*values)["sneaky"] == Catch::Approx(-1.0));
}

TEST_CASE("invalidation recomputes the transitive closure", "[features][factor][determinism]") {
    // A partial update that refreshed only DIRECT dependents would leave
    // anything two hops away holding a stale -- but still plausible -- value.
    FactorGraph g;
    const auto passthrough = [](std::string dep) {
        return [dep](const FactorInputs& in) { return in.get(dep).value(); };
    };
    REQUIRE(g.add({"b", {"a"}, passthrough("a"), 0}).has_value());
    REQUIRE(g.add({"c", {"b"}, passthrough("b"), 0}).has_value());
    REQUIRE(g.add({"d", {"c"}, passthrough("c"), 0}).has_value());
    REQUIRE(g.add({"unrelated", {"z"}, passthrough("z"), 0}).has_value());
    REQUIRE(g.finalize().has_value());

    const auto deps = g.dependents_of("a");
    REQUIRE(deps.size() == 3);  // b, c AND d -- two hops away included

    const std::vector<std::string> dirty{"a"};
    auto values = g.evaluate({{"a", 7.0}, {"z", 1.0}}, dirty);
    REQUIRE(values.has_value());
    REQUIRE((*values)["d"] == Catch::Approx(7.0));
}

TEST_CASE("a factor producing a non-finite value is refused", "[features][factor][edge]") {
    // Letting it through would surface as a NaN Sharpe with no attribution.
    FactorGraph g;
    REQUIRE(
        g.add({"bad", {"a"}, [](const FactorInputs& in) { return in.get("a").value() / 0.0; }, 0})
            .has_value());
    REQUIRE(g.finalize().has_value());

    auto r = g.evaluate({{"a", 1.0}});
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().message.find("non-finite") != std::string::npos);
    REQUIRE(r.error().message.find("bad") != std::string::npos);
}

TEST_CASE("duplicate factor registration is refused", "[features][factor]") {
    FactorGraph g;
    const auto noop = [](const FactorInputs&) { return 1.0; };
    REQUIRE(g.add({"dup", {}, noop, 0}).has_value());
    REQUIRE_FALSE(g.add({"dup", {}, noop, 0}).has_value());
    REQUIRE_FALSE(g.add({"", {}, noop, 0}).has_value());
    REQUIRE_FALSE(g.add({"null", {}, nullptr, 0}).has_value());
}

TEST_CASE("the evaluation order is deterministic", "[features][factor][determinism]") {
    // With several factors simultaneously ready, the lexicographically smallest
    // is emitted first. An unordered container would give a different valid
    // order per run, and floating-point summation is not associative.
    const auto build_order = [] {
        FactorGraph g;
        const auto noop = [](const FactorInputs&) { return 1.0; };
        for (const auto* n : {"zulu", "alpha", "mike", "bravo"}) {
            REQUIRE(g.add({n, {}, noop, 0}).has_value());
        }
        REQUIRE(g.finalize().has_value());
        std::vector<std::string> out;
        for (const auto& s : g.evaluation_order()) out.emplace_back(s);
        return out;
    };
    const auto a = build_order();
    REQUIRE(a == build_order());
    REQUIRE(a[0] == "alpha");
    REQUIRE(a[3] == "zulu");
}

// ---------------------------------------------------------------------------
// Feature validation
// ---------------------------------------------------------------------------

TEST_CASE("the validator detects lookahead against decision times",
          "[features][validation][leakage]") {
    // THE POINT-IN-TIME ASSERTION. A feature computed at 14:53 informing a
    // decision at 14:52 means the model saw the future.
    const auto m = build_matrix(3);
    std::vector<Timestamp> good;
    std::vector<Timestamp> bad;
    for (const auto& k : m.keys()) {
        good.push_back(k.feature_end_time + minutes{1});
        bad.push_back(k.feature_end_time - minutes{1});
    }

    const FeatureValidator v;
    REQUIRE(v.check_causality(m, good).ok());

    const auto report = v.check_causality(m, bad);
    REQUIRE_FALSE(report.ok());
    REQUIRE(report.count(FeatureIssueCode::LookaheadDetected) == 3);
    REQUIRE(report.issues.front().describe().find("is after decision_time") != std::string::npos);

    // A length mismatch is reported rather than reading out of bounds.
    REQUIRE_FALSE(v.check_causality(m, std::vector<Timestamp>{}).ok());
}

TEST_CASE("the validator detects NaN and infinite values", "[features][validation][edge]") {
    FeatureMatrix m{{"clean", "poisoned"}, 1, 2};
    Timestamp t = at("2024-07-02T14:00:00Z");
    for (int i = 0; i < 4; ++i) {
        std::vector<double> vals{static_cast<double>(i), static_cast<double>(i)};
        if (i == 1) vals[1] = std::numeric_limits<double>::quiet_NaN();
        if (i == 2) vals[1] = std::numeric_limits<double>::infinity();
        FeatureRow r;
        r.feature_end_time = t;
        r.data_version = 1;
        r.feature_set_id = 2;
        r.ready_mask = 0b11;
        r.values = vals;
        REQUIRE(m.append(r).has_value());
        t += minutes{1};
    }

    const auto report = FeatureValidator{}.validate(m);
    REQUIRE_FALSE(report.ok());
    REQUIRE(report.count(FeatureIssueCode::NaNValue) == 1);
    REQUIRE(report.count(FeatureIssueCode::InfiniteValue) == 1);
    // The report names the offending feature, not just the row.
    REQUIRE(report.issues.front().feature_name == "poisoned");
}

TEST_CASE("the validator detects constant and stale features", "[features][validation]") {
    // A feature that never moves cannot carry information -- almost always a
    // wiring error rather than a property of the market.
    FeatureMatrix m{{"varying", "frozen"}, 1, 2};
    Timestamp t = at("2024-07-02T14:00:00Z");
    for (int i = 0; i < 150; ++i) {
        const std::vector<double> vals{static_cast<double>(i), 42.0};
        FeatureRow r;
        r.feature_end_time = t;
        r.data_version = 1;
        r.feature_set_id = 2;
        r.ready_mask = 0b11;
        r.values = vals;
        REQUIRE(m.append(r).has_value());
        t += minutes{1};
    }

    FeatureValidatorConfig cfg;
    cfg.max_identical_run = 100;
    const auto report = FeatureValidator{cfg}.validate(m);
    REQUIRE(report.count(FeatureIssueCode::ConstantFeature) == 1);
    REQUIRE(report.count(FeatureIssueCode::StaleFeature) == 1);
    REQUIRE(report.summary().find("frozen") != std::string::npos);
}

TEST_CASE("a clean matrix validates without issues", "[features][validation]") {
    const auto report = FeatureValidator{}.validate(build_matrix(50), 0b111);
    REQUIRE(report.ok());
    REQUIRE(report.rows_checked == 50);
    REQUIRE(report.ready_rows == 48);
}
