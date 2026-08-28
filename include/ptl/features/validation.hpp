#pragma once

/// \file validation.hpp
/// Feature-level leakage and quality detection.
///
/// Distinct from the market-data validator, which checks the INPUT stream. This
/// checks the OUTPUT: rows that claim knowledge they could not have had, values
/// that stopped changing, NaNs that would poison a model.
///
/// The causality check here is the one the reconciliation names as
/// `test_feature_causality` -- feed two series identical up to index k and
/// assert every value at or before k is bit-identical. An estimator that peeks
/// fails automatically, without anyone having to guess where it peeked.

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "ptl/core/result.hpp"
#include "ptl/core/types.hpp"
#include "ptl/features/matrix.hpp"

namespace ptl::features {

enum class FeatureIssueCode : std::uint8_t {
    LookaheadDetected,  ///< feature_end_time after the decision it feeds
    NonMonotonicRows,
    NaNValue,
    InfiniteValue,
    StaleFeature,     ///< value unchanged for an implausible run
    ConstantFeature,  ///< zero variance across the whole sample
    MissingData,
    ReadyBeforeWarmup,
};

[[nodiscard]] std::string_view to_string(FeatureIssueCode) noexcept;

struct FeatureIssue {
    FeatureIssueCode code{FeatureIssueCode::NaNValue};
    std::size_t row = 0;
    std::size_t column = 0;
    std::string feature_name;
    std::string detail;

    [[nodiscard]] std::string describe() const;
};

struct FeatureValidationReport {
    std::vector<FeatureIssue> issues;
    std::size_t rows_checked = 0;
    std::size_t ready_rows = 0;

    [[nodiscard]] bool ok() const noexcept { return issues.empty(); }
    [[nodiscard]] std::size_t count(FeatureIssueCode) const noexcept;
    [[nodiscard]] std::string summary() const;
};

struct FeatureValidatorConfig {
    /// Consecutive identical values before a feature is called stale. A price
    /// can legitimately repeat; a hundred consecutive identical values means
    /// the estimator stopped updating.
    std::size_t max_identical_run = 100;
    /// Report features with zero variance across the sample. Usually a wiring
    /// error -- a feature that never moves cannot carry information.
    bool detect_constant = true;
    bool detect_stale = true;
};

class FeatureValidator {
public:
    explicit FeatureValidator(FeatureValidatorConfig cfg = {}) : cfg_(cfg) {}

    [[nodiscard]] FeatureValidationReport validate(const FeatureMatrix&,
                                                   std::uint64_t required_mask = 0) const;

    /// Assert that no row's feature_end_time exceeds the decision time it feeds.
    ///
    /// THE POINT-IN-TIME ASSERTION. `decision_times` is parallel to the
    /// matrix rows. A feature computed at 14:53 informing a decision at 14:52
    /// is lookahead, and this is where it becomes visible.
    [[nodiscard]] FeatureValidationReport check_causality(
        const FeatureMatrix&, std::span<const Timestamp> decision_times) const;

private:
    FeatureValidatorConfig cfg_;
};

/// Run a streaming estimator over two series identical up to `common_prefix`
/// and report the first index at which their outputs diverge.
///
/// \returns npos when outputs agree throughout the prefix -- which is what a
///          causal estimator must produce. A divergence inside the prefix means
///          the estimator consumed data from beyond it.
template <class Estimator, class Updater>
[[nodiscard]] std::size_t first_divergence(Estimator a, Estimator b,
                                           std::span<const double> series_a,
                                           std::span<const double> series_b,
                                           std::size_t common_prefix, Updater update) {
    const std::size_t n = std::min(series_a.size(), series_b.size());
    for (std::size_t i = 0; i < n; ++i) {
        update(a, series_a[i]);
        update(b, series_b[i]);
        if (i >= common_prefix) break;
        // Bit-identical, not approximately equal. A causal estimator fed
        // identical prefixes must produce identical outputs exactly.
        if (a.value() != b.value()) return i;
    }
    return static_cast<std::size_t>(-1);
}

}  // namespace ptl::features
