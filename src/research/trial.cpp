#include "ptl/research/trial.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <numbers>
#include <sstream>

namespace ptl::research {
namespace {

[[nodiscard]] std::string hex64(std::uint64_t v) {
    char buf[20];
    std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(v));
    return std::string{buf};
}

/// Standard normal CDF via erfc: numerically better than 0.5*(1+erf(x/sqrt2))
/// in the far left tail, which is exactly where a deflated Sharpe lives.
[[nodiscard]] double normal_cdf(double z) noexcept {
    return 0.5 * std::erfc(-z / std::numbers::sqrt2);
}

/// Inverse standard normal, Acklam's rational approximation. Accurate to about
/// 1e-9, which is far more than a trial count needs.
[[nodiscard]] double normal_quantile(double p) noexcept {
    if (p <= 0.0) return -1e10;
    if (p >= 1.0) return 1e10;
    static constexpr double a[] = {-3.969683028665376e+01, 2.209460984245205e+02,
                                   -2.759285104469687e+02, 1.383577518672690e+02,
                                   -3.066479806614716e+01, 2.506628277459239e+00};
    static constexpr double b[] = {-5.447609879822406e+01, 1.615858368580409e+02,
                                   -1.556989798598866e+02, 6.680131188771972e+01,
                                   -1.328068155288572e+01};
    static constexpr double c[] = {-7.784894002430293e-03, -3.223964580411365e-01,
                                   -2.400758277161838e+00, -2.549732539343734e+00,
                                   4.374664141464968e+00,  2.938163982698783e+00};
    static constexpr double d[] = {7.784695709041462e-03, 3.224671290700398e-01,
                                   2.445134137142996e+00, 3.754408661907416e+00};
    constexpr double p_low = 0.02425;

    if (p < p_low) {
        const double q = std::sqrt(-2.0 * std::log(p));
        return (((((c[0] * q + c[1]) * q + c[2]) * q + c[3]) * q + c[4]) * q + c[5]) /
               ((((d[0] * q + d[1]) * q + d[2]) * q + d[3]) * q + 1.0);
    }
    if (p > 1.0 - p_low) {
        const double q = std::sqrt(-2.0 * std::log(1.0 - p));
        return -(((((c[0] * q + c[1]) * q + c[2]) * q + c[3]) * q + c[4]) * q + c[5]) /
               ((((d[0] * q + d[1]) * q + d[2]) * q + d[3]) * q + 1.0);
    }
    const double q = p - 0.5;
    const double r = q * q;
    return (((((a[0] * r + a[1]) * r + a[2]) * r + a[3]) * r + a[4]) * r + a[5]) * q /
           (((((b[0] * r + b[1]) * r + b[2]) * r + b[3]) * r + b[4]) * r + 1.0);
}

}  // namespace

std::string TrialManifest::canonical() const {
    std::ostringstream ss;
    // Fixed field order and a separator that cannot appear in a value, so
    // shifting a character across a boundary cannot produce a collision.
    ss << "question=" << research_question << '\x1f';
    ss << "hypothesis=" << hypothesis << '\x1f';
    ss << "data=" << hex64(data_version) << '\x1f';
    ss << "features=" << hex64(feature_set_id) << '\x1f';
    ss << "labels=" << hex64(label_config_hash) << '\x1f';
    ss << "folds=" << hex64(fold_config_hash) << '\x1f';
    ss << "model=" << hex64(model_config_hash) << '\x1f';
    ss << "git=" << git_sha << '\x1f';
    ss << "seed=" << seed << '\x1f';
    ss << "holdout=" << holdout_boundary << '\x1f';
    // std::map iterates in key order, so insertion order cannot change the id.
    for (const auto& [k, v] : parameters) {
        ss << k << '=' << v << '\x1f';
    }
    return ss.str();
}

std::uint64_t TrialManifest::trial_id() const {
    return fnv1a64(canonical());
}
std::string TrialManifest::trial_id_hex() const {
    return hex64(trial_id());
}

std::string_view to_string(ResearchIssueCode c) noexcept {
    switch (c) {
        case ResearchIssueCode::DuplicateTrial:
            return "duplicate_trial";
        case ResearchIssueCode::ConfigurationDrift:
            return "configuration_drift";
        case ResearchIssueCode::StaleArtifact:
            return "stale_artifact";
        case ResearchIssueCode::HoldoutContamination:
            return "holdout_contamination";
        case ResearchIssueCode::LabelFeatureMisalignment:
            return "label_feature_misalignment";
        case ResearchIssueCode::SearchBudgetExceeded:
            return "search_budget_exceeded";
        case ResearchIssueCode::NonDeterministicResult:
            return "non_deterministic_result";
        case ResearchIssueCode::UndeclaredBudget:
            return "undeclared_budget";
    }
    return "unknown";
}

std::string ResearchIssue::describe() const {
    std::string out{to_string(code)};
    out += fatal ? " [FATAL] " : " [warn] ";
    out += detail;
    return out;
}

bool ResearchReport::ok() const noexcept {
    return fatal_count() == 0;
}

std::size_t ResearchReport::fatal_count() const noexcept {
    return static_cast<std::size_t>(std::count_if(issues.begin(), issues.end(),
                                                  [](const ResearchIssue& i) { return i.fatal; }));
}

std::string ResearchReport::summary() const {
    std::ostringstream ss;
    ss << "trials run: " << trials_run;
    if (declared_budget > 0) ss << " of a declared budget of " << declared_budget;
    ss << '\n';
    for (const auto& i : issues) ss << "  " << i.describe() << '\n';
    return ss.str();
}

Result<ResearchReport> ResearchValidator::validate(const TrialManifest& manifest) {
    ResearchReport report;

    auto count = registry_->trial_count(manifest.research_question);
    if (!count) return fail(count.error());
    report.trials_run = *count;

    auto budget = registry_->get_budget(manifest.research_question);
    if (!budget) return fail(budget.error());

    if (!budget->has_value()) {
        // No budget declared at all. Not fatal -- exploratory work is
        // legitimate -- but any Sharpe from this question must be reported as
        // uncorrected, because there is no trial count to correct by.
        report.issues.push_back({ResearchIssueCode::UndeclaredBudget,
                                 "no search budget was declared for '" +
                                     manifest.research_question +
                                     "'; results from it cannot be multiple-testing corrected",
                                 false});
    } else {
        report.declared_budget = (*budget)->budget;
        auto exceeded = registry_->budget_exceeded(manifest.research_question);
        if (!exceeded) return fail(exceeded.error());
        if (*exceeded) {
            report.issues.push_back({ResearchIssueCode::SearchBudgetExceeded,
                                     std::to_string(report.trials_run) +
                                         " trials against a declared budget of " +
                                         std::to_string(report.declared_budget) +
                                         "; every Sharpe from this question needs deflating",
                                     false});
        }
    }

    // Duplicate detection by trial id. Re-running to confirm a result is
    // legitimate, so this is a warning -- but it must not increment the count,
    // or the multiple-testing correction is inflated by repetition.
    auto existing = registry_->find_run(manifest.trial_id_hex());
    if (!existing) return fail(existing.error());
    if (existing->has_value()) {
        report.issues.push_back({ResearchIssueCode::DuplicateTrial,
                                 "trial " + manifest.trial_id_hex() +
                                     " has been run before; it will not be counted again",
                                 false});
    }
    return report;
}

Result<std::int64_t> ResearchValidator::register_trial(const TrialManifest& manifest,
                                                       std::string_view run_id) {
    auto existing = registry_->find_run(manifest.trial_id_hex());
    if (!existing) return fail(existing.error());
    if (existing->has_value()) {
        // Already known: return without inserting, so the trial count stays a
        // count of DISTINCT configurations.
        return std::int64_t{0};
    }

    experiments::TrialRecord t;
    t.run_id = std::string{run_id};
    t.research_question = manifest.research_question;
    t.hypothesis = manifest.hypothesis;
    t.params_json = manifest.canonical();
    t.status = "run";
    return registry_->insert_trial(t);
}

Result<bool> ResearchValidator::verify_determinism(const TrialManifest& manifest,
                                                   std::uint64_t result_hash_a,
                                                   std::uint64_t result_hash_b) {
    if (result_hash_a == result_hash_b) return true;
    // Hidden state, unseeded randomness, or hash-ordered iteration. Whichever
    // it is, every result in the registry becomes suspect: none of them can be
    // reproduced.
    return fail(make_error(
        ErrorCode::ValidationFailed,
        "trial " + manifest.trial_id_hex() +
            " produced different results on two runs of identical inputs. Every "
            "recorded result is now suspect until the source of non-determinism is found.",
        hex64(result_hash_a) + " vs " + hex64(result_hash_b)));
}

ResearchIssue ResearchValidator::check_artifact_freshness(std::uint64_t artifact_feature_set_id,
                                                          std::uint64_t current_feature_set_id,
                                                          std::uint64_t artifact_data_version,
                                                          std::uint64_t current_data_version) {
    if (artifact_feature_set_id != current_feature_set_id) {
        // FATAL. A cached matrix from an older definition has values that no
        // longer match their column names -- a model trained on it learns
        // relationships between the wrong things.
        return {ResearchIssueCode::StaleArtifact,
                "artifact was built from feature set " + hex64(artifact_feature_set_id) +
                    " but the current definition is " + hex64(current_feature_set_id) +
                    "; its values no longer match their names",
                true};
    }
    if (artifact_data_version != current_data_version) {
        return {ResearchIssueCode::StaleArtifact,
                "artifact derives from dataset " + hex64(artifact_data_version) +
                    " but the current dataset is " + hex64(current_data_version),
                true};
    }
    return {ResearchIssueCode::StaleArtifact, "artifact is current", false};
}

ResearchIssue ResearchValidator::check_alignment(std::span<const Timestamp> feature_times,
                                                 std::span<const Timestamp> label_times) {
    if (feature_times.size() != label_times.size()) {
        return {ResearchIssueCode::LabelFeatureMisalignment,
                "feature series has " + std::to_string(feature_times.size()) +
                    " rows but the label series has " + std::to_string(label_times.size()),
                true};
    }
    for (std::size_t i = 0; i < feature_times.size(); ++i) {
        if (feature_times[i] != label_times[i]) {
            // A one-row offset between features and labels LOOKS like a signal
            // and is entirely lookahead: row i's features would be paired with
            // row i+1's outcome, which the model could not have known.
            return {ResearchIssueCode::LabelFeatureMisalignment,
                    "row " + std::to_string(i) + ": feature time " + to_iso8601(feature_times[i]) +
                        " does not match label time " + to_iso8601(label_times[i]),
                    true};
        }
    }
    return {ResearchIssueCode::LabelFeatureMisalignment, "aligned", false};
}

double expected_max_sharpe(std::size_t n_trials, double trial_stdev) {
    if (n_trials <= 1) return 0.0;
    const double n = static_cast<double>(n_trials);
    constexpr double kEulerMascheroni = 0.5772156649015329;
    // Bailey & Lopez de Prado's expected maximum of n independent draws.
    const double z1 = normal_quantile(1.0 - 1.0 / n);
    const double z2 = normal_quantile(1.0 - 1.0 / (n * std::numbers::e));
    return trial_stdev * ((1.0 - kEulerMascheroni) * z1 + kEulerMascheroni * z2);
}

double deflated_sharpe_ratio(double observed_sharpe, std::size_t n_trials,
                             std::size_t n_observations, double skewness, double kurtosis,
                             double trial_sharpe_stdev) {
    if (n_observations < 2) return 0.0;
    if (n_trials <= 1) return observed_sharpe > 0.0 ? 1.0 : 0.0;

    // The benchmark an observed Sharpe must beat: after enough trials, an
    // impressive number is the expected maximum of noise.
    const double sr_star = expected_max_sharpe(n_trials, trial_sharpe_stdev);
    const double t = static_cast<double>(n_observations);

    // Variance of the Sharpe estimator, adjusted for the non-normality of
    // returns. Ignoring skew and kurtosis understates it, which flatters the
    // result exactly when returns are most non-normal.
    const double denominator = 1.0 - skewness * observed_sharpe +
                               (kurtosis - 1.0) / 4.0 * observed_sharpe * observed_sharpe;
    if (denominator <= 0.0 || !is_finite(denominator)) return 0.0;

    const double z = (observed_sharpe - sr_star) * std::sqrt(t - 1.0) / std::sqrt(denominator);
    if (!is_finite(z)) return 0.0;
    // Probability that the true Sharpe exceeds zero given the trial count.
    return normal_cdf(z);
}

}  // namespace ptl::research
