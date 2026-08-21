#include <catch2/catch_test_macros.hpp>

#include "support/ptl_catch.hpp"

#include <chrono>
#include <filesystem>
#include <string>

#include "ptl/experiments/registry.hpp"

using namespace ptl;
using namespace ptl::experiments;

namespace {

struct TempDb {
    std::filesystem::path path;
    TempDb() {
        // Unique per test case and per process, without reaching for POSIX:
        // the suite must build unchanged on macOS, Linux and Windows.
        const auto stamp = static_cast<unsigned long long>(
            std::chrono::steady_clock::now().time_since_epoch().count());
        path = std::filesystem::temp_directory_path() /
               ("ptl_registry_test_" + std::to_string(stamp) + "_" +
                std::to_string(counter_++) + ".sqlite");
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
    static inline int counter_ = 0;
};

Registry must_open(const std::filesystem::path& p) {
    auto r = Registry::open(p);
    REQUIRE(r.has_value());
    return std::move(*r);
}

RunRecord sample_run(std::string id) {
    RunRecord r;
    r.run_id = std::move(id);
    r.git_sha = "deadbeef";
    r.git_dirty = "clean";
    r.config_hash = "0123456789abcdef";
    r.config_canonical = "run.seed=1\n";
    r.data_manifest_sha = "sha-1";
    r.seed = 20240101;
    r.compiler = "GNU";
    r.compiler_version = "13.3.0";
    r.build_type = "Debug";
    r.tag = "unit";
    return r;
}

}  // namespace

TEST_CASE("registry creates its schema and round-trips a run", "[experiments]") {
    TempDb db;
    auto reg = must_open(db.path);

    REQUIRE(reg.insert_run(sample_run("aaaa0000")).has_value());

    auto found = reg.find_run("aaaa0000");
    REQUIRE(found.has_value());
    REQUIRE(found->has_value());
    REQUIRE((*found)->git_sha == "deadbeef");
    REQUIRE((*found)->seed == 20240101);
    REQUIRE((*found)->status == "started");

    auto missing = reg.find_run("no-such-run");
    REQUIRE(missing.has_value());
    REQUIRE_FALSE(missing->has_value());
}

TEST_CASE("finishing a run records status and chain violations", "[experiments]") {
    TempDb db;
    auto reg = must_open(db.path);
    REQUIRE(reg.insert_run(sample_run("bbbb0000")).has_value());
    REQUIRE(reg.finish_run("bbbb0000", "invalidated", 3).has_value());

    auto found = reg.find_run("bbbb0000");
    REQUIRE((*found)->status == "invalidated");
    // A non-zero chain-violation count invalidates the run. Storing it beside
    // the metrics means nobody can quote the Sharpe without also seeing that
    // the point-in-time invariant was broken while producing it.
    REQUIRE((*found)->chain_violations == 3);
}

TEST_CASE("registry persists across reopen", "[experiments][determinism]") {
    TempDb db;
    {
        auto reg = must_open(db.path);
        REQUIRE(reg.insert_run(sample_run("cccc0000")).has_value());
    }
    auto reg = must_open(db.path);
    auto found = reg.find_run("cccc0000");
    REQUIRE(found->has_value());
}

TEST_CASE("trials are counted per research question", "[experiments][validation]") {
    TempDb db;
    auto reg = must_open(db.path);
    REQUIRE(reg.insert_run(sample_run("dddd0000")).has_value());

    for (int i = 0; i < 5; ++i) {
        TrialRecord t;
        t.run_id = "dddd0000";
        t.research_question = "does 5m reversal survive costs";
        t.hypothesis = "ridge beats the rule baseline net of spread";
        t.params_json = R"({"lambda":)" + std::to_string(i) + "}";
        t.status = "run";
        auto id = reg.insert_trial(t);
        REQUIRE(id.has_value());
        REQUIRE(*id == i + 1);
    }
    auto n = reg.trial_count("does 5m reversal survive costs");
    REQUIRE(*n == 5);
    REQUIRE(*reg.trial_count("some other question") == 0);
}

TEST_CASE("search budgets are declare-once and detect overrun", "[experiments][validation]") {
    TempDb db;
    auto reg = must_open(db.path);
    REQUIRE(reg.insert_run(sample_run("eeee0000")).has_value());

    SearchBudget b;
    b.research_question = "q1";
    b.budget = 3;
    b.rationale = "three regularisation strengths, chosen before evaluation";
    auto first = reg.declare_budget(b);
    REQUIRE(*first);  // inserted

    // Raising the budget after the fact would defeat the entire mechanism, so
    // a second declaration is ignored rather than applied.
    b.budget = 500;
    auto second = reg.declare_budget(b);
    REQUIRE_FALSE(*second);
    auto stored = reg.get_budget("q1");
    REQUIRE((*stored)->budget == 3);

    REQUIRE_FALSE(*reg.budget_exceeded("q1"));
    for (int i = 0; i < 4; ++i) {
        TrialRecord t;
        t.run_id = "eeee0000";
        t.research_question = "q1";
        REQUIRE(reg.insert_trial(t).has_value());
    }
    REQUIRE(*reg.budget_exceeded("q1"));
    REQUIRE((*reg.get_budget("q1"))->used == 4);

    // No declared budget means nothing to violate -- but callers must treat
    // that as its own reportable state, not as compliance.
    REQUIRE_FALSE(*reg.budget_exceeded("undeclared"));
    REQUIRE_FALSE((*reg.get_budget("undeclared")).has_value());
}

TEST_CASE("holdout unlocks are recorded and require justification", "[experiments][validation]") {
    TempDb db;
    auto reg = must_open(db.path);
    REQUIRE(*reg.holdout_unlock_count() == 0);

    auto refused = reg.record_holdout_unlock("ffff0000", "");
    REQUIRE_FALSE(refused.has_value());
    REQUIRE(refused.error().code == ErrorCode::InvalidArgument);

    REQUIRE(reg.record_holdout_unlock("ffff0000", "design frozen 2024-09-01").has_value());
    REQUIRE(*reg.holdout_unlock_count() == 1);

    // Append-only by design: there is no delete path. The holdout is only worth
    // having because unlocking it is permanent and visible.
    REQUIRE(reg.record_holdout_unlock("ffff0001", "second look, reported").has_value());
    REQUIRE(*reg.holdout_unlock_count() == 2);
}

TEST_CASE("metrics attach to runs and trials", "[experiments]") {
    TempDb db;
    auto reg = must_open(db.path);
    REQUIRE(reg.insert_run(sample_run("1111aaaa")).has_value());
    REQUIRE(reg.record_metric("1111aaaa", 0, "net_sharpe", 0.71).has_value());
    REQUIRE(reg.record_metric("1111aaaa", 0, "turnover", 12.4).has_value());
}
