#pragma once

/// \file experiment.hpp
/// Experiment definition, comparison, leaderboard and checkpointing.
///
/// THE EXPERIMENT MANAGER ORCHESTRATES; IT DOES NOT REIMPLEMENT. Metrics come
/// from `ptl::analytics`, attribution from `ptl::attribution`, run and trial
/// provenance from `ptl::experiments::Registry` (Phase 1, SQLite), config
/// hashing from `ptl::config`. Nothing here recomputes a Sharpe ratio, and a
/// second implementation of one is precisely how two reports of the same run
/// come to disagree.
///
/// WHAT IS GENUINELY NEW is the layer above: binding a strategy version, a
/// dataset version, a parameter set and a seed into ONE reproducible object;
/// comparing many such objects; and ranking them.
///
/// THE EXECUTION ENGINE MUST NEVER KNOW ABOUT EXPERIMENTS (Phase 13 design
/// constraint). The dependency runs one way only: experiment knows strategy,
/// storage and analytics; none of those knows experiment.

#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "ptl/analytics/performance_analyzer.hpp"
#include "ptl/attribution/execution_quality.hpp"
#include "ptl/attribution/pnl.hpp"
#include "ptl/core/result.hpp"
#include "ptl/storage/registry.hpp"
#include "ptl/strategy/registry.hpp"

namespace ptl::experiment {

/// Everything needed to reproduce a run, in one object.
///
/// If two ExperimentConfigs have the same fingerprint, they must produce
/// identical results. That is the contract, and it is what every other feature
/// here depends on: a leaderboard comparing runs that cannot be reproduced is
/// a ranking of noise.
struct ExperimentConfig {
    std::string experiment_id;
    std::string description;

    strategy::StrategyId strategy_id;
    strategy::StrategyVersion strategy_version;
    strategy::ParameterMap parameters;

    std::string dataset_id;
    std::uint32_t dataset_version = 0;

    /// The seed. Recorded explicitly rather than defaulted, because a run whose
    /// seed is unknown cannot be reproduced even with everything else pinned.
    std::uint64_t seed = 0;

    /// Hash of the ptl::config the run used, from ptl::config's own hashing.
    /// Not recomputed here -- one implementation, one answer.
    std::uint64_t config_hash = 0;

    /// Free-form tags for grouping in a leaderboard.
    std::vector<std::string> tags;

    /// Fingerprint over everything that affects the result.
    [[nodiscard]] std::uint64_t fingerprint() const;

    /// Structural validation. Does NOT check that the strategy or dataset
    /// exists -- that needs the registries, and is done by ExperimentRunner
    /// where they are available.
    [[nodiscard]] Result<bool> validate() const;
    [[nodiscard]] std::string to_json() const;
};

/// Where an experiment is.
enum class ExperimentStatus : std::uint8_t {
    Defined,
    Running,
    Completed,
    Failed,
    Cancelled,
};

[[nodiscard]] std::string_view to_string(ExperimentStatus) noexcept;

/// The measured outcome of one experiment.
///
/// Holds references to analytics types rather than copies of their numbers, so
/// there is exactly one definition of "Sharpe" in the system.
struct ExperimentResult {
    std::string experiment_id;
    ExperimentStatus status{ExperimentStatus::Defined};
    Timestamp started_at{kNoTimestamp};
    Timestamp finished_at{kNoTimestamp};

    /// The config that produced this, carried along so a result is never
    /// separated from the thing that generated it.
    std::uint64_t config_fingerprint = 0;

    std::optional<analytics::PerformanceReport> performance;
    std::optional<attribution::PnlDecomposition> pnl;
    std::optional<attribution::ExecutionQualitySummary> execution;
    std::optional<attribution::FactorContribution> factors;

    /// Arbitrary scalar outputs the caller wants ranked, ordered by name.
    std::map<std::string, double, std::less<>> custom_metrics;

    std::string error_message;

    /// Named metric lookup across every source, so the comparison engine does
    /// not need to know where a number lives.
    [[nodiscard]] std::optional<double> metric(std::string_view name) const;
    /// Every metric this result can answer for, sorted.
    [[nodiscard]] std::vector<std::string> available_metrics() const;
    [[nodiscard]] Duration duration() const noexcept;
    [[nodiscard]] std::string to_json() const;
};

/// A definition plus, once run, its result.
struct Experiment {
    ExperimentConfig config;
    ExperimentStatus status{ExperimentStatus::Defined};
    std::optional<ExperimentResult> result;

    [[nodiscard]] bool complete() const noexcept {
        return status == ExperimentStatus::Completed && result.has_value();
    }
};

/// Whether higher or lower is better for a metric.
///
/// Explicit per metric, because a leaderboard that assumed "higher is better"
/// would rank the worst drawdown first and the ranking would look plausible.
enum class Direction : std::uint8_t { HigherIsBetter, LowerIsBetter };

/// One metric a comparison ranks on.
struct ComparisonMetric {
    std::string name;
    Direction direction{Direction::HigherIsBetter};
    /// Weight in a composite score. Zero means report but do not rank.
    double weight = 1.0;
};

/// The standard institutional set: Sharpe, Sortino, Calmar, IR, drawdown,
/// turnover, capacity, P&L, execution quality.
[[nodiscard]] std::vector<ComparisonMetric> default_comparison_metrics();

/// One experiment's row in a comparison.
struct ComparisonRow {
    std::string experiment_id;
    /// Metric name to value; absent when the experiment cannot answer for it.
    std::map<std::string, std::optional<double>, std::less<>> values;
    /// Rank per metric, 1 being best. Absent where the value is.
    std::map<std::string, std::optional<std::size_t>, std::less<>> ranks;
    /// Weighted composite over the ranked metrics.
    double composite_score = 0.0;
    std::size_t overall_rank = 0;
};

struct ComparisonReport {
    std::vector<ComparisonMetric> metrics;
    /// Ordered by overall rank, then by id so ties are reproducible.
    std::vector<ComparisonRow> rows;
    /// Metrics no experiment could answer for. Reported rather than dropped:
    /// a silently missing column looks like agreement.
    std::vector<std::string> unavailable_metrics;

    [[nodiscard]] const ComparisonRow* best() const noexcept;
    [[nodiscard]] std::string to_json() const;
    [[nodiscard]] std::string describe() const;
};

/// Compares experiments on a common metric set.
class ExperimentComparison {
public:
    explicit ExperimentComparison(
        std::vector<ComparisonMetric> metrics = default_comparison_metrics())
        : metrics_(std::move(metrics)) {}

    [[nodiscard]] Result<ComparisonReport> compare(std::span<const ExperimentResult>) const;

    [[nodiscard]] const std::vector<ComparisonMetric>& metrics() const noexcept { return metrics_; }

private:
    std::vector<ComparisonMetric> metrics_;
};

/// A ranked leaderboard entry.
struct LeaderboardEntry {
    std::string experiment_id;
    std::string strategy_id;
    double score = 0.0;
    std::size_t rank = 0;
    std::map<std::string, double, std::less<>> metrics;
};

struct Leaderboard {
    std::string name;
    std::string primary_metric;
    std::vector<LeaderboardEntry> entries;

    [[nodiscard]] const LeaderboardEntry* leader() const noexcept;
    [[nodiscard]] std::string to_json() const;
};

/// Ranks experiments on one metric.
///
/// Separate from ExperimentComparison because they answer different questions:
/// a comparison is a full table across many metrics, a leaderboard is an
/// ordering on one. Conflating them produces a table nobody can read and a
/// ranking nobody trusts.
class LeaderboardBuilder {
public:
    /// \param limit zero means no limit.
    [[nodiscard]] static Result<Leaderboard> build(std::span<const Experiment>,
                                                   std::string_view metric,
                                                   Direction direction = Direction::HigherIsBetter,
                                                   std::size_t limit = 0);
};

/// A point-in-time snapshot of an experiment, for resume.
struct ExperimentSnapshot {
    std::string experiment_id;
    std::uint64_t sequence = 0;
    Timestamp taken_at{kNoTimestamp};
    ExperimentStatus status{ExperimentStatus::Running};

    /// How far the run had progressed. Resume continues from here, so a
    /// partial rerun does not repeat completed work.
    std::size_t events_processed = 0;
    Timestamp last_event_time{kNoTimestamp};
    std::uint64_t config_fingerprint = 0;

    /// Fingerprint over the snapshot's own contents, so a corrupt or truncated
    /// snapshot is detected on load rather than resumed from.
    [[nodiscard]] std::uint64_t checksum() const;
    [[nodiscard]] std::string to_json() const;
    [[nodiscard]] static Result<ExperimentSnapshot> from_json(std::string_view);
};

/// Persists experiments, results and snapshots through the artifact store.
///
/// Composes `storage::ArtifactStore` rather than doing its own file handling:
/// one implementation of atomic write, one place for it to be correct.
class ExperimentStore {
public:
    explicit ExperimentStore(storage::ArtifactStore& artifacts) noexcept : artifacts_(&artifacts) {}

    [[nodiscard]] Result<bool> save_config(const ExperimentConfig&);
    [[nodiscard]] Result<bool> save_result(const ExperimentResult&);
    [[nodiscard]] Result<bool> save_snapshot(const ExperimentSnapshot&);

    [[nodiscard]] Result<std::optional<ExperimentSnapshot>> load_snapshot(
        std::string_view experiment_id);
    [[nodiscard]] Result<std::vector<std::string>> list_experiments() const;

private:
    storage::ArtifactStore* artifacts_;
};

/// Validates and prepares experiments against the registries.
///
/// Does NOT run the engine. Running is the caller's job, because the runner
/// would otherwise need to know about execution -- which this module is
/// forbidden to know about. It answers "is this experiment coherent and
/// reproducible?", which is the question the registries can actually settle.
class ExperimentRunner {
public:
    ExperimentRunner(const strategy::StrategyRegistry& strategies,
                     const storage::DatasetRegistry& datasets) noexcept
        : strategies_(&strategies), datasets_(&datasets) {}

    /// Full validation: the strategy exists at that version, accepts new
    /// experiments, its parameters typecheck, and the dataset version is
    /// registered.
    [[nodiscard]] Result<bool> validate(const ExperimentConfig&) const;

    /// Prepare an experiment for running. Refuses anything validate() refuses,
    /// so an invalid experiment never reaches the engine.
    [[nodiscard]] Result<Experiment> prepare(ExperimentConfig) const;

    /// Whether a result may resume from a snapshot: same config fingerprint,
    /// intact checksum. Resuming into a different configuration silently mixes
    /// two runs.
    [[nodiscard]] static Result<bool> can_resume(const ExperimentConfig&,
                                                 const ExperimentSnapshot&);

private:
    const strategy::StrategyRegistry* strategies_;
    const storage::DatasetRegistry* datasets_;
};

}  // namespace ptl::experiment
