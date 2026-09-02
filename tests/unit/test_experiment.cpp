#include <algorithm>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <vector>

#include "ptl/experiment/experiment.hpp"
#include "support/ptl_catch.hpp"

using namespace ptl;
using namespace ptl::experiment;

namespace {

Timestamp at(const char* iso) {
    Timestamp ts{};
    REQUIRE(parse_timestamp(iso, ts));
    return ts;
}

strategy::StrategyId id_of(const char* text) {
    auto id = strategy::StrategyId::create(text);
    REQUIRE(id.has_value());
    return *id;
}

strategy::StrategyVersion version_of(const char* text) {
    auto v = strategy::StrategyVersion::parse(text);
    REQUIRE(v.has_value());
    return *v;
}

/// A registry pair with one usable strategy and one usable dataset.
struct Fixture {
    strategy::StrategyRegistry strategies;
    storage::DatasetRegistry datasets;

    Fixture() {
        strategy::StrategyDescriptor d;
        d.id = id_of("alpha");
        d.version = version_of("1.0.0");
        d.state = strategy::StrategyState::Research;
        d.parameters.push_back({"lookback", "int", "bars", false, "20"});
        REQUIRE(strategies.register_strategy(d).has_value());

        storage::DatasetVersion ds;
        ds.dataset_id = "equities";
        ds.version = 1;
        ds.content_checksum = storage::Checksum::of("content");
        ds.feature_schema.fields.push_back({"ret_1m", "double", 1});
        ds.normalization_version = "zscore_v1";
        REQUIRE(datasets.register_dataset(ds).has_value());
    }
};

ExperimentConfig config_for(const char* experiment_id, std::uint64_t seed = 42) {
    ExperimentConfig cfg;
    cfg.experiment_id = experiment_id;
    cfg.strategy_id = id_of("alpha");
    cfg.strategy_version = version_of("1.0.0");
    cfg.dataset_id = "equities";
    cfg.dataset_version = 1;
    cfg.seed = seed;
    cfg.config_hash = 0xABCDEF;
    return cfg;
}

/// A completed result with the given headline metrics.
ExperimentResult result_with(const char* experiment_id, double sharpe, double max_drawdown,
                             double turnover = 10.0) {
    ExperimentResult r;
    r.experiment_id = experiment_id;
    r.status = ExperimentStatus::Completed;
    r.started_at = at("2024-01-02T00:00:00Z");
    r.finished_at = at("2024-01-02T01:00:00Z");

    analytics::PerformanceReport performance;
    performance.metrics.sharpe = sharpe;
    performance.metrics.sortino = sharpe * 1.2;
    performance.max_drawdown = max_drawdown;
    performance.turnover.annualized_turnover = turnover;
    r.performance = std::move(performance);
    return r;
}

}  // namespace

// ---------------------------------------------------------------------------
// Config and reproducibility
// ---------------------------------------------------------------------------

TEST_CASE("an experiment without a seed is refused", "[experiment][validation][determinism]") {
    // A zero seed is almost always an unset field, and a run whose seed is
    // unknown cannot be reproduced even with everything else pinned.
    auto cfg = config_for("exp1");
    cfg.seed = 0;
    auto validated = cfg.validate();
    REQUIRE_FALSE(validated.has_value());
    REQUIRE(validated.error().message.find("seed") != std::string::npos);

    auto no_dataset = config_for("exp1");
    no_dataset.dataset_version = 0;
    REQUIRE_FALSE(no_dataset.validate().has_value());
}

TEST_CASE("the config fingerprint covers what changes results",
          "[experiment][determinism][property]") {
    const auto base = config_for("exp1");
    const auto original = base.fingerprint();

    // Renaming an experiment does not change what it computes.
    auto renamed = base;
    renamed.experiment_id = "different_name";
    renamed.description = "a different description";
    renamed.tags = {"new_tag"};
    REQUIRE(renamed.fingerprint() == original);

    // These all change the result.
    auto reseeded = base;
    reseeded.seed = 43;
    REQUIRE(reseeded.fingerprint() != original);

    auto reparameterised = base;
    reparameterised.parameters["lookback"] = "30";
    REQUIRE(reparameterised.fingerprint() != original);

    auto redated = base;
    redated.dataset_version = 2;
    REQUIRE(redated.fingerprint() != original);

    auto upgraded = base;
    upgraded.strategy_version = version_of("2.0.0");
    REQUIRE(upgraded.fingerprint() != original);
}

TEST_CASE("parameter order does not affect the fingerprint", "[experiment][determinism]") {
    // std::map keeps key order, so the fingerprint does not depend on the order
    // the caller inserted parameters.
    auto a = config_for("exp1");
    a.parameters["lookback"] = "20";
    a.parameters["threshold"] = "0.5";

    auto b = config_for("exp1");
    b.parameters["threshold"] = "0.5";
    b.parameters["lookback"] = "20";

    REQUIRE(a.fingerprint() == b.fingerprint());
}

TEST_CASE("the runner validates against both registries", "[experiment][validation]") {
    Fixture fixture;
    const ExperimentRunner runner{fixture.strategies, fixture.datasets};

    REQUIRE(runner.validate(config_for("exp1")).has_value());

    auto unknown_strategy = config_for("exp1");
    unknown_strategy.strategy_id = id_of("nonexistent");
    REQUIRE_FALSE(runner.validate(unknown_strategy).has_value());

    auto unknown_version = config_for("exp1");
    unknown_version.strategy_version = version_of("9.0.0");
    REQUIRE_FALSE(runner.validate(unknown_version).has_value());

    auto unknown_dataset = config_for("exp1");
    unknown_dataset.dataset_version = 99;
    auto refused = runner.validate(unknown_dataset);
    REQUIRE_FALSE(refused.has_value());
    REQUIRE(refused.error().message.find("not registered") != std::string::npos);

    // Undeclared parameters are caught through the strategy's schema.
    auto bad_parameter = config_for("exp1");
    bad_parameter.parameters["nonexistent_param"] = "1";
    REQUIRE_FALSE(runner.validate(bad_parameter).has_value());
}

TEST_CASE("a retired strategy cannot start new experiments", "[experiment][validation][property]") {
    Fixture fixture;
    REQUIRE(
        fixture.strategies
            .transition(id_of("alpha"), version_of("1.0.0"), strategy::StrategyState::Deprecated)
            .has_value());

    const ExperimentRunner runner{fixture.strategies, fixture.datasets};
    auto refused = runner.validate(config_for("exp1"));
    REQUIRE_FALSE(refused.has_value());
    REQUIRE(refused.error().message.find("does not accept new experiments") != std::string::npos);
}

TEST_CASE("prepare refuses anything validate refuses", "[experiment]") {
    Fixture fixture;
    const ExperimentRunner runner{fixture.strategies, fixture.datasets};

    auto prepared = runner.prepare(config_for("exp1"));
    REQUIRE(prepared.has_value());
    REQUIRE(prepared->status == ExperimentStatus::Defined);
    REQUIRE(prepared->config.experiment_id == "exp1");

    auto invalid = config_for("exp1");
    invalid.seed = 0;
    REQUIRE_FALSE(runner.prepare(invalid).has_value());
}

// ---------------------------------------------------------------------------
// Metric lookup
// ---------------------------------------------------------------------------

TEST_CASE("results read metrics from analytics rather than recomputing", "[experiment][property]") {
    // There is exactly one definition of "Sharpe" in the system, and a result
    // reads it rather than deriving a second one.
    const auto r = result_with("exp1", 1.5, 0.08, 12.0);
    REQUIRE(r.metric("sharpe").has_value());
    REQUIRE(*r.metric("sharpe") == Catch::Approx(1.5));
    REQUIRE(*r.metric("max_drawdown") == Catch::Approx(0.08));
    REQUIRE(*r.metric("turnover") == Catch::Approx(12.0));
    REQUIRE_FALSE(r.metric("nonexistent_metric").has_value());

    const auto available = r.available_metrics();
    REQUIRE(std::is_sorted(available.begin(), available.end()));
    REQUIRE(std::find(available.begin(), available.end(), "sharpe") != available.end());
}

TEST_CASE("custom metrics take precedence over derived ones", "[experiment][edge]") {
    // A caller who explicitly recorded a value under a name meant that one.
    auto r = result_with("exp1", 1.5, 0.08);
    r.custom_metrics["sharpe"] = 99.0;
    REQUIRE(*r.metric("sharpe") == Catch::Approx(99.0));
}

TEST_CASE("an empty result answers for nothing", "[experiment][edge]") {
    ExperimentResult empty;
    empty.experiment_id = "exp1";
    REQUIRE_FALSE(empty.metric("sharpe").has_value());
    REQUIRE(empty.available_metrics().empty());
}

// ---------------------------------------------------------------------------
// Comparison
// ---------------------------------------------------------------------------

TEST_CASE("comparison respects each metric's direction", "[experiment][comparison][property]") {
    // A leaderboard assuming "higher is better" would rank the worst drawdown
    // first, and the ranking would look perfectly plausible.
    const std::vector<ExperimentResult> results{
        result_with("shallow", 1.0, 0.05),  // worse Sharpe, better drawdown
        result_with("deep", 2.0, 0.40)};    // better Sharpe, worse drawdown

    const ExperimentComparison comparison{{{"sharpe", Direction::HigherIsBetter, 1.0},
                                           {"max_drawdown", Direction::LowerIsBetter, 1.0}}};

    auto report = comparison.compare(results);
    REQUIRE(report.has_value());
    REQUIRE(report->rows.size() == 2);

    const auto find_row = [&report](std::string_view id) {
        return std::find_if(report->rows.begin(), report->rows.end(),
                            [id](const ComparisonRow& row) { return row.experiment_id == id; });
    };
    // Best Sharpe is rank 1 on that metric...
    REQUIRE(*find_row("deep")->ranks["sharpe"] == 1);
    // ...while the SMALLEST drawdown is rank 1 on that one.
    REQUIRE(*find_row("shallow")->ranks["max_drawdown"] == 1);
}

TEST_CASE("a metric no experiment answers is reported, not dropped",
          "[experiment][comparison][edge]") {
    // A silently missing column looks like agreement between experiments that
    // simply never measured it.
    const std::vector<ExperimentResult> results{result_with("exp1", 1.0, 0.05)};
    const ExperimentComparison comparison{
        {{"sharpe", Direction::HigherIsBetter, 1.0},
         {"implementation_shortfall_bps", Direction::LowerIsBetter, 1.0}}};

    auto report = comparison.compare(results);
    REQUIRE(report.has_value());
    REQUIRE(report->unavailable_metrics.size() == 1);
    REQUIRE(report->unavailable_metrics.front() == "implementation_shortfall_bps");
}

TEST_CASE("an experiment missing one metric is not penalised on it",
          "[experiment][comparison][property]") {
    // It simply does not participate in that ranking, rather than being scored
    // as though it did badly.
    auto complete = result_with("complete", 1.0, 0.05);
    complete.custom_metrics["special"] = 5.0;
    const auto partial = result_with("partial", 2.0, 0.05);

    const ExperimentComparison comparison{
        {{"sharpe", Direction::HigherIsBetter, 1.0}, {"special", Direction::HigherIsBetter, 1.0}}};

    const std::vector<ExperimentResult> both{complete, partial};
    auto report = comparison.compare(both);
    REQUIRE(report.has_value());
    // "partial" has the better Sharpe and should still win overall despite
    // having no value for "special".
    REQUIRE(report->best()->experiment_id == "partial");
}

TEST_CASE("comparison is deterministic including ties", "[experiment][comparison][determinism]") {
    // Identical scores tie-break on id, so the ordering is a pure function of
    // the inputs rather than of sort stability.
    const std::vector<ExperimentResult> results{result_with("zebra", 1.0, 0.05),
                                                result_with("alpha", 1.0, 0.05)};
    const ExperimentComparison comparison;

    auto first = comparison.compare(results);
    auto second = comparison.compare(results);
    REQUIRE(first.has_value());
    REQUIRE(second.has_value());
    REQUIRE(first->to_json() == second->to_json());
    REQUIRE(first->rows.front().experiment_id == "alpha");

    REQUIRE_FALSE(comparison.compare({}).has_value());
}

// ---------------------------------------------------------------------------
// Leaderboard
// ---------------------------------------------------------------------------

TEST_CASE("the leaderboard ranks on one metric", "[experiment][leaderboard][property]") {
    std::vector<Experiment> experiments;
    for (const auto& [name, sharpe] :
         std::vector<std::pair<const char*, double>>{{"a", 0.5}, {"b", 2.5}, {"c", 1.5}}) {
        Experiment e;
        e.config = config_for(name);
        e.status = ExperimentStatus::Completed;
        e.result = result_with(name, sharpe, 0.10);
        experiments.push_back(std::move(e));
    }

    auto board = LeaderboardBuilder::build(experiments, "sharpe");
    REQUIRE(board.has_value());
    REQUIRE(board->entries.size() == 3);
    REQUIRE(board->leader()->experiment_id == "b");
    REQUIRE(board->entries.front().rank == 1);

    // Lower-is-better inverts the order.
    auto inverted = LeaderboardBuilder::build(experiments, "sharpe", Direction::LowerIsBetter);
    REQUIRE(inverted->leader()->experiment_id == "a");

    auto limited = LeaderboardBuilder::build(experiments, "sharpe", Direction::HigherIsBetter, 2);
    REQUIRE(limited->entries.size() == 2);
}

TEST_CASE("only completed experiments are ranked", "[experiment][leaderboard][leakage]") {
    // Including a failed or running experiment would put a partial result on a
    // board that reads as final.
    std::vector<Experiment> experiments;

    Experiment finished;
    finished.config = config_for("done");
    finished.status = ExperimentStatus::Completed;
    finished.result = result_with("done", 1.0, 0.1);
    experiments.push_back(std::move(finished));

    Experiment running;
    running.config = config_for("running");
    running.status = ExperimentStatus::Running;
    experiments.push_back(std::move(running));

    Experiment broken;
    broken.config = config_for("broken");
    broken.status = ExperimentStatus::Failed;
    broken.result = result_with("broken", 99.0, 0.0);  // a spectacular fake
    experiments.push_back(std::move(broken));

    auto board = LeaderboardBuilder::build(experiments, "sharpe");
    REQUIRE(board.has_value());
    REQUIRE(board->entries.size() == 1);
    REQUIRE(board->leader()->experiment_id == "done");

    REQUIRE_FALSE(LeaderboardBuilder::build(experiments, "").has_value());
}

TEST_CASE("leaderboard JSON is deterministic", "[experiment][leaderboard][serialization]") {
    std::vector<Experiment> experiments;
    for (const char* name : {"zebra", "alpha"}) {
        Experiment e;
        e.config = config_for(name);
        e.status = ExperimentStatus::Completed;
        e.result = result_with(name, 1.0, 0.1);  // identical scores
        experiments.push_back(std::move(e));
    }

    auto first = LeaderboardBuilder::build(experiments, "sharpe");
    auto second = LeaderboardBuilder::build(experiments, "sharpe");
    REQUIRE(first->to_json() == second->to_json());
    // Tie-break on id.
    REQUIRE(first->leader()->experiment_id == "alpha");
}

// ---------------------------------------------------------------------------
// Checkpointing
// ---------------------------------------------------------------------------

TEST_CASE("a snapshot round-trips and detects corruption",
          "[experiment][checkpoint][serialization]") {
    ExperimentSnapshot snapshot;
    snapshot.experiment_id = "exp1";
    snapshot.sequence = 7;
    snapshot.taken_at = at("2024-01-02T15:00:00Z");
    snapshot.events_processed = 4242;
    snapshot.last_event_time = at("2024-01-02T14:59:00Z");
    snapshot.config_fingerprint = config_for("exp1").fingerprint();

    const std::string json = snapshot.to_json();
    auto restored = ExperimentSnapshot::from_json(json);
    REQUIRE(restored.has_value());
    REQUIRE(restored->experiment_id == "exp1");
    REQUIRE(restored->sequence == 7);
    REQUIRE(restored->events_processed == 4242);
    REQUIRE(restored->config_fingerprint == snapshot.config_fingerprint);

    // Corrupting the event count invalidates the checksum, and a damaged
    // snapshot is refused rather than resumed from.
    std::string tampered = json;
    const auto pos = tampered.find("4242");
    REQUIRE(pos != std::string::npos);
    tampered.replace(pos, 4, "9999");
    REQUIRE_FALSE(ExperimentSnapshot::from_json(tampered).has_value());

    REQUIRE_FALSE(ExperimentSnapshot::from_json("{}").has_value());
    REQUIRE_FALSE(ExperimentSnapshot::from_json("not json at all").has_value());
}

TEST_CASE("resume is refused into a different configuration", "[experiment][checkpoint][leakage]") {
    // Resuming into a different configuration silently mixes two runs, and
    // every result afterwards is attributed to the wrong definition.
    const auto config = config_for("exp1");

    ExperimentSnapshot snapshot;
    snapshot.experiment_id = "exp1";
    snapshot.config_fingerprint = config.fingerprint();
    REQUIRE(ExperimentRunner::can_resume(config, snapshot).has_value());

    auto changed = config;
    changed.seed = 999;
    auto refused = ExperimentRunner::can_resume(changed, snapshot);
    REQUIRE_FALSE(refused.has_value());
    REQUIRE(refused.error().message.find("different configuration") != std::string::npos);

    ExperimentSnapshot other;
    other.experiment_id = "different_experiment";
    other.config_fingerprint = config.fingerprint();
    REQUIRE_FALSE(ExperimentRunner::can_resume(config, other).has_value());
}

TEST_CASE("the experiment store persists and reloads through artifacts",
          "[experiment][checkpoint][storage]") {
    const auto root = std::filesystem::temp_directory_path() / "ptl_experiment_store";
    std::filesystem::remove_all(root);
    storage::ArtifactStore artifacts{root.string()};
    ExperimentStore store{artifacts};

    const auto config = config_for("exp1");
    REQUIRE(store.save_config(config).has_value());
    REQUIRE(store.save_result(result_with("exp1", 1.5, 0.08)).has_value());

    ExperimentSnapshot snapshot;
    snapshot.experiment_id = "exp1";
    snapshot.sequence = 3;
    snapshot.events_processed = 100;
    snapshot.config_fingerprint = config.fingerprint();
    REQUIRE(store.save_snapshot(snapshot).has_value());

    auto loaded = store.load_snapshot("exp1");
    REQUIRE(loaded.has_value());
    REQUIRE(loaded->has_value());
    REQUIRE((*loaded)->events_processed == 100);

    // A missing snapshot is absence, not failure.
    auto absent = store.load_snapshot("never_saved");
    REQUIRE(absent.has_value());
    REQUIRE_FALSE(absent->has_value());

    auto ids = store.list_experiments();
    REQUIRE(ids.has_value());
    REQUIRE(ids->size() == 1);
    REQUIRE(ids->front() == "exp1");

    std::filesystem::remove_all(root);
}

TEST_CASE("experiment JSON is deterministic", "[experiment][determinism][serialization]") {
    const auto config = config_for("exp1");
    REQUIRE(config.to_json() == config.to_json());

    const auto result = result_with("exp1", 1.5, 0.08);
    REQUIRE(result.to_json() == result.to_json());
    REQUIRE(result.to_json().find("\"sharpe\"") != std::string::npos);
}
