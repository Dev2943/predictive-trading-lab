#include <algorithm>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <string>

#include "ptl/experiment/experiment.hpp"
#include "support/ptl_catch.hpp"

using namespace ptl;
using namespace std::chrono;

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

strategy::StrategyDescriptor descriptor(
    const char* name, const char* version,
    strategy::StrategyState state = strategy::StrategyState::Research) {
    strategy::StrategyDescriptor d;
    d.id = id_of(name);
    d.version = version_of(version);
    d.state = state;
    d.metadata.author = "research";
    d.metadata.description = "a test strategy";
    d.metadata.tags = {"intraday"};
    d.parameters.push_back({"lookback", "int", "bars of history", true, "20"});
    d.parameters.push_back({"threshold", "double", "entry threshold", false, "0.5"});
    return d;
}

storage::DatasetVersion dataset(const char* name, std::uint32_t version) {
    storage::DatasetVersion d;
    d.dataset_id = name;
    d.version = version;
    d.created_at = at("2024-01-02T00:00:00Z");
    d.range_begin = at("2024-01-02T00:00:00Z");
    d.range_end = at("2024-06-01T00:00:00Z");
    d.content_checksum = storage::Checksum::of("some content");
    d.feature_schema.fields.push_back({"ret_1m", "double", 1});
    d.label_schema.fields.push_back({"fwd_ret", "double", 0});
    d.normalization_version = "zscore_v1";
    d.row_count = 10000;
    d.source = "databento:cbbo-1m";
    return d;
}

}  // namespace

// ---------------------------------------------------------------------------
// Strategy catalogue
// ---------------------------------------------------------------------------

TEST_CASE("strategy ids reject unsafe characters", "[strategy][validation]") {
    // Permissive validation here means every downstream layer can embed the id
    // in a filename, a config key or a JSON field without escaping it.
    REQUIRE(strategy::StrategyId::create("mean_reversion-v2.a").has_value());
    REQUIRE_FALSE(strategy::StrategyId::create("").has_value());
    REQUIRE_FALSE(strategy::StrategyId::create("has space").has_value());
    REQUIRE_FALSE(strategy::StrategyId::create("has/slash").has_value());
    REQUIRE_FALSE(strategy::StrategyId::create("has\"quote").has_value());
    REQUIRE_FALSE(strategy::StrategyId::create(std::string(200, 'a')).has_value());
}

TEST_CASE("versions order numerically, not lexically", "[strategy][property][regression]") {
    // Under a string sort 1.10.0 precedes 1.9.0, and a rollback silently picks
    // the wrong artifact.
    REQUIRE(version_of("1.9.0") < version_of("1.10.0"));
    REQUIRE(version_of("1.2.3") < version_of("2.0.0"));
    REQUIRE(version_of("2.0.0") > version_of("1.99.99"));
    REQUIRE(version_of("1.2.3") == version_of("1.2.3"));

    REQUIRE(version_of("1.2.3").compatible_with(version_of("1.9.0")));
    REQUIRE_FALSE(version_of("1.2.3").compatible_with(version_of("2.0.0")));

    REQUIRE_FALSE(strategy::StrategyVersion::parse("1.2").has_value());
    REQUIRE_FALSE(strategy::StrategyVersion::parse("1.2.x").has_value());
    REQUIRE_FALSE(strategy::StrategyVersion::parse("1..3").has_value());
}

TEST_CASE("a duplicate strategy version is refused", "[strategy][registry]") {
    // Two definitions of one version means a lookup would have to choose, and
    // choosing silently is how an experiment runs code nobody intended.
    strategy::StrategyRegistry registry;
    REQUIRE(registry.register_strategy(descriptor("alpha", "1.0.0")).has_value());
    REQUIRE_FALSE(registry.register_strategy(descriptor("alpha", "1.0.0")).has_value());
    // A different version is fine.
    REQUIRE(registry.register_strategy(descriptor("alpha", "1.1.0")).has_value());
    REQUIRE(registry.size() == 2);
}

TEST_CASE("latest and latest_compatible pick the right version", "[strategy][registry][property]") {
    strategy::StrategyRegistry registry;
    for (const char* v : {"1.0.0", "1.9.0", "1.10.0", "2.0.0"}) {
        REQUIRE(registry.register_strategy(descriptor("alpha", v)).has_value());
    }

    REQUIRE(registry.latest(id_of("alpha"))->version == version_of("2.0.0"));
    // Compatible means same major: the newest 1.x, not the newest overall.
    const auto* compatible = registry.latest_compatible(id_of("alpha"), version_of("1.0.0"));
    REQUIRE(compatible != nullptr);
    REQUIRE(compatible->version == version_of("1.10.0"));

    REQUIRE(registry.find(id_of("alpha"), version_of("1.9.0")) != nullptr);
    REQUIRE(registry.find(id_of("alpha"), version_of("3.0.0")) == nullptr);
    REQUIRE(registry.latest(id_of("nonexistent")) == nullptr);
}

TEST_CASE("the fingerprint covers the parameter schema but not the state", "[strategy][property]") {
    auto base = descriptor("alpha", "1.0.0");
    const auto original = base.fingerprint();

    // Promotion does not change what the strategy computes, so results
    // recorded before it must not be orphaned.
    base.state = strategy::StrategyState::Production;
    REQUIRE(base.fingerprint() == original);
    base.metadata.author = "someone else";
    REQUIRE(base.fingerprint() == original);

    // A parameter change DOES make results incomparable.
    base.parameters[0].type = "double";
    REQUIRE(base.fingerprint() != original);
}

TEST_CASE("lifecycle transitions are constrained", "[strategy][registry]") {
    strategy::StrategyRegistry registry;
    REQUIRE(registry.register_strategy(descriptor("alpha", "1.0.0", strategy::StrategyState::Draft))
                .has_value());

    // A draft cannot jump straight to production.
    REQUIRE_FALSE(
        registry
            .transition(id_of("alpha"), version_of("1.0.0"), strategy::StrategyState::Production)
            .has_value());
    REQUIRE(
        registry.transition(id_of("alpha"), version_of("1.0.0"), strategy::StrategyState::Research)
            .has_value());
    REQUIRE(
        registry.transition(id_of("alpha"), version_of("1.0.0"), strategy::StrategyState::Retired)
            .has_value());
    // A retired strategy cannot be revived by a state change; re-registration
    // is required so the decision leaves a trace.
    REQUIRE_FALSE(
        registry
            .transition(id_of("alpha"), version_of("1.0.0"), strategy::StrategyState::Production)
            .has_value());
}

TEST_CASE("only some states accept new experiments", "[strategy][property]") {
    // A deprecated strategy may still be REPRODUCED, but no new research
    // should start against it.
    REQUIRE(strategy::accepts_new_experiments(strategy::StrategyState::Research));
    REQUIRE(strategy::accepts_new_experiments(strategy::StrategyState::Production));
    REQUIRE_FALSE(strategy::accepts_new_experiments(strategy::StrategyState::Draft));
    REQUIRE_FALSE(strategy::accepts_new_experiments(strategy::StrategyState::Deprecated));
    REQUIRE_FALSE(strategy::accepts_new_experiments(strategy::StrategyState::Retired));
}

TEST_CASE("dependency cycles are detected", "[strategy][registry][leakage]") {
    // A recursive walk would run until the stack ran out, which is a much worse
    // diagnostic than naming the loop.
    strategy::StrategyRegistry registry;
    auto a = descriptor("a", "1.0.0");
    auto b = descriptor("b", "1.0.0");
    a.depends_on.push_back(id_of("b"));
    b.depends_on.push_back(id_of("a"));
    REQUIRE(registry.register_strategy(a).has_value());
    REQUIRE(registry.register_strategy(b).has_value());

    auto resolved = registry.resolve_dependencies(id_of("a"), version_of("1.0.0"));
    REQUIRE_FALSE(resolved.has_value());
    REQUIRE(resolved.error().message.find("cycle") != std::string::npos);

    // A self-dependency is caught at registration, before it can ever be walked.
    auto self = descriptor("c", "1.0.0");
    self.depends_on.push_back(id_of("c"));
    REQUIRE_FALSE(registry.register_strategy(self).has_value());
}

TEST_CASE("dependencies resolve in topological order", "[strategy][registry][property]") {
    strategy::StrategyRegistry registry;
    auto base = descriptor("base", "1.0.0");
    auto middle = descriptor("middle", "1.0.0");
    auto top = descriptor("top", "1.0.0");
    middle.depends_on.push_back(id_of("base"));
    top.depends_on.push_back(id_of("middle"));

    REQUIRE(registry.register_strategy(base).has_value());
    REQUIRE(registry.register_strategy(middle).has_value());
    REQUIRE(registry.register_strategy(top).has_value());

    auto order = registry.resolve_dependencies(id_of("top"), version_of("1.0.0"));
    REQUIRE(order.has_value());
    REQUIRE(order->size() == 3);
    // Dependencies precede what needs them.
    REQUIRE((*order)[0] == id_of("base"));
    REQUIRE((*order)[2] == id_of("top"));

    auto missing = descriptor("orphan", "1.0.0");
    missing.depends_on.push_back(id_of("absent"));
    REQUIRE(registry.register_strategy(missing).has_value());
    REQUIRE_FALSE(registry.resolve_dependencies(id_of("orphan"), version_of("1.0.0")).has_value());
}

TEST_CASE("parameters are typechecked against the schema", "[strategy][validation]") {
    const auto d = descriptor("alpha", "1.0.0");

    strategy::ParameterMap good{{"lookback", "30"}, {"threshold", "0.75"}};
    REQUIRE(strategy::validate_parameters(d, good).has_value());

    // An undeclared parameter is almost always a typo, and ignoring it means
    // the run uses a default nobody intended.
    strategy::ParameterMap undeclared{{"lookback", "30"}, {"lookbcak", "30"}};
    REQUIRE_FALSE(strategy::validate_parameters(d, undeclared).has_value());

    strategy::ParameterMap wrong_type{{"lookback", "not_a_number"}};
    REQUIRE_FALSE(strategy::validate_parameters(d, wrong_type).has_value());

    // Trailing characters mean the value was only partly parsed, which is how
    // "1.5x" silently becomes 1.5.
    strategy::ParameterMap trailing{{"threshold", "0.5x"}};
    REQUIRE_FALSE(strategy::validate_parameters(d, trailing).has_value());
}

TEST_CASE("the catalogue manifest is deterministic", "[strategy][determinism][serialization]") {
    strategy::StrategyRegistry registry;
    REQUIRE(registry.register_strategy(descriptor("zebra", "1.0.0")).has_value());
    REQUIRE(registry.register_strategy(descriptor("alpha", "2.0.0")).has_value());
    REQUIRE(registry.register_strategy(descriptor("alpha", "1.0.0")).has_value());

    const std::string first = registry.to_json();
    REQUIRE(first == registry.to_json());
    // Ordered by name then version, so a manifest can be diffed between runs.
    REQUIRE(first.find("alpha") < first.find("zebra"));
}

// ---------------------------------------------------------------------------
// Dataset and model registries
// ---------------------------------------------------------------------------

TEST_CASE("datasets are append-only", "[storage][dataset][leakage]") {
    // A rewritten version silently invalidates every model trained on it while
    // leaving the version number unchanged.
    storage::DatasetRegistry registry;
    REQUIRE(registry.register_dataset(dataset("equities", 1)).has_value());
    REQUIRE_FALSE(registry.register_dataset(dataset("equities", 1)).has_value());
    // Versions must advance.
    REQUIRE_FALSE(registry.register_dataset(dataset("equities", 1)).has_value());
    REQUIRE(registry.register_dataset(dataset("equities", 2)).has_value());
    REQUIRE(registry.latest("equities")->version == 2);
}

TEST_CASE("a dataset without a checksum is refused", "[storage][dataset][validation]") {
    storage::DatasetRegistry registry;
    auto no_checksum = dataset("equities", 1);
    no_checksum.content_checksum = storage::Checksum{};
    REQUIRE_FALSE(registry.register_dataset(no_checksum).has_value());

    auto no_schema = dataset("equities", 1);
    no_schema.feature_schema.fields.clear();
    REQUIRE_FALSE(registry.register_dataset(no_schema).has_value());

    auto zero_version = dataset("equities", 0);
    REQUIRE_FALSE(registry.register_dataset(zero_version).has_value());

    auto unordered_splits = dataset("equities", 1);
    unordered_splits.split_boundaries = {at("2024-03-01T00:00:00Z"), at("2024-02-01T00:00:00Z")};
    REQUIRE_FALSE(registry.register_dataset(unordered_splits).has_value());
}

TEST_CASE("the dataset fingerprint covers normalization", "[storage][dataset][property]") {
    // A normalisation change invalidates every model trained against the
    // previous version even at identical raw data.
    auto a = dataset("equities", 1);
    const auto original = a.fingerprint();
    a.normalization_version = "zscore_v2";
    REQUIRE(a.fingerprint() != original);

    auto b = dataset("equities", 1);
    b.feature_schema.fields.push_back({"vol_5m", "double", 5});
    REQUIRE(b.fingerprint() != original);
}

TEST_CASE("a model on an unregistered dataset is refused", "[storage][model][leakage]") {
    // THE CENTRAL INVARIANT OF THE MODULE. A model whose training data cannot
    // be identified is not reproducible.
    storage::DatasetRegistry datasets;
    REQUIRE(datasets.register_dataset(dataset("equities", 1)).has_value());
    storage::ModelRegistry models{datasets};

    storage::ModelMetadata model;
    model.model_id = "ridge_alpha";
    model.version = 1;
    model.kind = "ridge";
    model.dataset_id = "equities";
    model.dataset_version = 99;  // never registered
    model.status = storage::ModelStatus::Trained;

    auto refused = models.register_model(model);
    REQUIRE_FALSE(refused.has_value());
    REQUIRE(refused.error().message.find("unregistered dataset") != std::string::npos);

    model.dataset_version = 1;
    REQUIRE(models.register_model(model).has_value());

    // A model naming no dataset at all is refused before the registry is even
    // consulted.
    storage::ModelMetadata orphan = model;
    orphan.version = 2;
    orphan.dataset_id.clear();
    REQUIRE_FALSE(models.register_model(orphan).has_value());
}

TEST_CASE("promotion archives the incumbent and rollback restores it",
          "[storage][model][property]") {
    storage::DatasetRegistry datasets;
    REQUIRE(datasets.register_dataset(dataset("equities", 1)).has_value());
    storage::ModelRegistry models{datasets};

    for (std::uint32_t v : {1u, 2u}) {
        storage::ModelMetadata model;
        model.model_id = "ridge";
        model.version = v;
        model.kind = "ridge";
        model.dataset_id = "equities";
        model.dataset_version = 1;
        model.status = storage::ModelStatus::Challenger;
        REQUIRE(models.register_model(model).has_value());
    }

    REQUIRE(models.promote("ridge", 1, at("2024-06-01T00:00:00Z")).has_value());
    REQUIRE(models.champion("ridge")->version == 1);

    REQUIRE(models.promote("ridge", 2, at("2024-07-01T00:00:00Z")).has_value());
    REQUIRE(models.champion("ridge")->version == 2);
    // The incumbent is ARCHIVED, not deleted: rollback needs it.
    REQUIRE(models.find("ridge", 1)->status == storage::ModelStatus::Archived);

    REQUIRE(models.rollback("ridge", 1, at("2024-07-02T00:00:00Z"), "regression").has_value());
    REQUIRE(models.champion("ridge")->version == 1);
    // Every transition is recorded.
    REQUIRE(models.history().size() == 3);
}

TEST_CASE("only a former champion can be rolled back to", "[storage][model][edge]") {
    // Rolling back to something that was never champion is a promotion, and
    // calling it a rollback would misrepresent the history.
    storage::DatasetRegistry datasets;
    REQUIRE(datasets.register_dataset(dataset("equities", 1)).has_value());
    storage::ModelRegistry models{datasets};

    storage::ModelMetadata challenger;
    challenger.model_id = "ridge";
    challenger.version = 1;
    challenger.kind = "ridge";
    challenger.dataset_id = "equities";
    challenger.dataset_version = 1;
    challenger.status = storage::ModelStatus::Challenger;
    REQUIRE(models.register_model(challenger).has_value());

    REQUIRE_FALSE(models.rollback("ridge", 1, at("2024-07-01T00:00:00Z")).has_value());
    REQUIRE_FALSE(models.rollback("ridge", 99, at("2024-07-01T00:00:00Z")).has_value());
}

TEST_CASE("a failed model cannot be promoted", "[storage][model][edge]") {
    storage::DatasetRegistry datasets;
    REQUIRE(datasets.register_dataset(dataset("equities", 1)).has_value());
    storage::ModelRegistry models{datasets};

    storage::ModelMetadata broken;
    broken.model_id = "ridge";
    broken.version = 1;
    broken.kind = "ridge";
    broken.dataset_id = "equities";
    broken.dataset_version = 1;
    broken.status = storage::ModelStatus::Failed;
    REQUIRE(models.register_model(broken).has_value());
    REQUIRE_FALSE(models.promote("ridge", 1, at("2024-07-01T00:00:00Z")).has_value());
}

// ---------------------------------------------------------------------------
// Artifact store
// ---------------------------------------------------------------------------

TEST_CASE("artifacts round-trip and list in sorted order", "[storage][artifacts][serialization]") {
    const auto root = std::filesystem::temp_directory_path() / "ptl_artifacts_test";
    std::filesystem::remove_all(root);
    storage::ArtifactStore store{root.string()};

    REQUIRE(store.put("experiments/b/result", R"({"x": 1})").has_value());
    REQUIRE(store.put("experiments/a/result", R"({"x": 2})").has_value());
    REQUIRE(store.contains("experiments/a/result"));

    auto contents = store.get("experiments/a/result");
    REQUIRE(contents.has_value());
    REQUIRE(*contents == R"({"x": 2})");

    auto keys = store.list("experiments");
    REQUIRE(keys.has_value());
    REQUIRE(keys->size() == 2);
    // Directory iteration order is filesystem-defined; the sort is what makes
    // a listing reproducible.
    REQUIRE(std::is_sorted(keys->begin(), keys->end()));

    REQUIRE(store.checksum_of("experiments/a/result").has_value());
    REQUIRE_FALSE(store.get("experiments/missing").has_value());
    REQUIRE(store.remove("experiments/a/result").has_value());
    REQUIRE_FALSE(store.contains("experiments/a/result"));

    std::filesystem::remove_all(root);
}

TEST_CASE("artifact keys reject path traversal", "[storage][artifacts][validation]") {
    // A key containing ".." is a bug or an attack, and quietly rewriting it
    // hides which.
    const auto root = std::filesystem::temp_directory_path() / "ptl_artifacts_guard";
    storage::ArtifactStore store{root.string()};
    REQUIRE_FALSE(store.put("../escape", "{}").has_value());
    REQUIRE_FALSE(store.put("/absolute", "{}").has_value());
    REQUIRE_FALSE(store.put("", "{}").has_value());
    std::filesystem::remove_all(root);
}
