#pragma once

/// \file trial.hpp
/// Trial manifests, reproducible ids, and research-level validation.
///
/// A trial is one complete experiment: a dataset, a feature definition, a label
/// definition, a fold scheme and a model configuration. Its ID IS THE HASH OF
/// ALL FIVE, so two trials that differ in any respect cannot collide, and two
/// that are identical cannot be counted twice.
///
/// That last property is what makes the multiple-testing correction honest.
/// Deflated Sharpe needs to know how many things were tried; a registry that
/// double-counts a re-run inflates the count, and one that misses a genuinely
/// new configuration deflates it.

#include <cstdint>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "ptl/core/result.hpp"
#include "ptl/core/types.hpp"
#include "ptl/experiments/registry.hpp"
#include "ptl/validation/fold.hpp"

namespace ptl::research {

/// Everything that can change a result. Anything omitted here is, by
/// definition, claimed not to matter.
struct TrialManifest {
    std::string research_question;
    std::string hypothesis;

    /// Hash of the dataset manifest: vendor, feed, schema, symbols, range.
    std::uint64_t data_version = 0;
    /// Hash of the ordered feature definitions.
    std::uint64_t feature_set_id = 0;
    /// Hash of the label configuration.
    std::uint64_t label_config_hash = 0;
    /// Hash of the walk-forward configuration.
    std::uint64_t fold_config_hash = 0;
    /// Hash of the model configuration, including hyperparameters.
    std::uint64_t model_config_hash = 0;

    std::string git_sha;
    std::uint64_t seed = 0;
    std::string holdout_boundary;

    /// Ordered, so two manifests with the same parameters in a different
    /// insertion order still hash identically.
    std::map<std::string, std::string, std::less<>> parameters;

    /// Canonical serialisation: sorted, fully explicit, format-independent.
    [[nodiscard]] std::string canonical() const;

    /// Reproducible id. Same inputs, same id, on any machine and in any run.
    [[nodiscard]] std::uint64_t trial_id() const;
    [[nodiscard]] std::string trial_id_hex() const;
};

// ---------------------------------------------------------------------------
// Research validation
// ---------------------------------------------------------------------------

enum class ResearchIssueCode : std::uint8_t {
    DuplicateTrial,        ///< this exact configuration has been run before
    ConfigurationDrift,    ///< a recorded trial no longer matches its manifest
    StaleArtifact,         ///< artifact derives from a superseded definition
    HoldoutContamination,  ///< a fold reaches into the locked holdout
    LabelFeatureMisalignment,
    SearchBudgetExceeded,
    NonDeterministicResult,
    UndeclaredBudget,  ///< trials run with no budget declared at all
};

[[nodiscard]] std::string_view to_string(ResearchIssueCode) noexcept;

struct ResearchIssue {
    ResearchIssueCode code{ResearchIssueCode::DuplicateTrial};
    std::string detail;
    bool fatal = false;

    [[nodiscard]] std::string describe() const;
};

struct ResearchReport {
    std::vector<ResearchIssue> issues;
    std::int64_t trials_run = 0;
    std::int64_t declared_budget = 0;

    [[nodiscard]] bool ok() const noexcept;
    [[nodiscard]] std::size_t fatal_count() const noexcept;
    [[nodiscard]] std::string summary() const;
};

/// Validates a trial against the registry BEFORE it runs.
class ResearchValidator {
public:
    explicit ResearchValidator(experiments::Registry& registry) noexcept : registry_(&registry) {}

    /// \returns issues found. A duplicate is not fatal -- re-running a trial to
    ///          confirm a result is legitimate -- but it must not increment the
    ///          trial count, and the caller is told so it can decide.
    [[nodiscard]] Result<ResearchReport> validate(const TrialManifest&);

    /// Register a trial, refusing to double-count an identical one.
    [[nodiscard]] Result<std::int64_t> register_trial(const TrialManifest&,
                                                      std::string_view run_id);

    /// Two runs of the same trial must produce the same result hash. A
    /// mismatch means hidden state, unseeded randomness, or hash-ordered
    /// iteration -- and invalidates every result the registry holds.
    [[nodiscard]] Result<bool> verify_determinism(const TrialManifest&, std::uint64_t result_hash_a,
                                                  std::uint64_t result_hash_b);

    /// Detect an artifact built from a superseded definition.
    [[nodiscard]] static ResearchIssue check_artifact_freshness(
        std::uint64_t artifact_feature_set_id, std::uint64_t current_feature_set_id,
        std::uint64_t artifact_data_version, std::uint64_t current_data_version);

    /// Every label's feature_end_time must equal the feature row it pairs with.
    /// A one-row offset between the two series is the classic misalignment: it
    /// looks like a signal and is entirely lookahead.
    [[nodiscard]] static ResearchIssue check_alignment(std::span<const Timestamp> feature_times,
                                                       std::span<const Timestamp> label_times);

private:
    experiments::Registry* registry_;
};

/// Deflated Sharpe Ratio.
///
/// Corrects an observed Sharpe for the number of trials that produced it. The
/// research names this explicitly: after enough trials, an impressive Sharpe is
/// the expected maximum of noise, and reporting it without the correction is
/// the single most common way a backtest misleads.
/// \param trial_sharpe_stdev dispersion of Sharpe estimates ACROSS the trials
///        actually run. The default of 1.0 assumes unit dispersion, which is
///        conservative -- it makes the benchmark harder to beat. Supply the
///        observed dispersion for a correction that reflects the real search.
[[nodiscard]] double deflated_sharpe_ratio(double observed_sharpe, std::size_t n_trials,
                                           std::size_t n_observations, double skewness = 0.0,
                                           double kurtosis = 3.0, double trial_sharpe_stdev = 1.0);

/// Expected maximum Sharpe from `n_trials` draws of pure noise. The benchmark
/// an observed Sharpe must beat before it means anything.
[[nodiscard]] double expected_max_sharpe(std::size_t n_trials, double trial_stdev = 1.0);

}  // namespace ptl::research
