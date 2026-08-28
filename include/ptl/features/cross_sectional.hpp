#pragma once

/// \file cross_sectional.hpp
/// The cross-sectional barrier and its transforms.
///
/// THE POINT-IN-TIME PROBLEM THIS SOLVES. A market-relative return needs SPY's
/// state at the same instant as the asset's. A per-instrument streaming
/// estimator cannot reach another instrument, and letting it would open a door
/// to reading a neighbour's FUTURE state if the two were updated out of order.
///
/// The barrier closes that door. Per-instrument sets are updated first; then,
/// once every set for a given bar timestamp has been folded in, the barrier
/// runs and may read only states whose feature_end_time equals the current bar
/// close. Anything older or newer is refused, not skipped.

#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "ptl/core/result.hpp"
#include "ptl/core/types.hpp"
#include "ptl/features/feature_set.hpp"

namespace ptl::features {

// ---------------------------------------------------------------------------
// Transforms
// ---------------------------------------------------------------------------

/// Fractional ranks in [0, 1], ties averaged.
///
/// Averaged ties matter: with many identical values -- common for a feature
/// that is zero until warmup -- ordinal ranking would impose an arbitrary
/// ordering that depends on input order, which is a determinism hazard.
[[nodiscard]] std::vector<double> percentile_ranks(std::span<const double> values);

/// Cross-sectional z-score. Zero dispersion yields all zeros, never infinities.
[[nodiscard]] std::vector<double> cross_sectional_zscore(std::span<const double> values);

/// Clip to the given quantiles, computed from the values themselves.
///
/// Winsorisation, not trimming: outliers are pulled to the boundary rather than
/// dropped, so the cross-section keeps its size and every instrument still gets
/// a value.
[[nodiscard]] std::vector<double> winsorize(std::span<const double> values,
                                            double lower_quantile = 0.01,
                                            double upper_quantile = 0.99);

/// Subtract the cross-sectional mean. The simplest form of market neutrality.
[[nodiscard]] std::vector<double> demean(std::span<const double> values);

/// Subtract the mean WITHIN each group, leaving the group structure intact.
///
/// Sector-neutralisation: a signal that merely says "energy outperformed" is
/// a sector bet wearing an alpha costume, and demeaning within sector is what
/// separates the two.
[[nodiscard]] std::vector<double> group_demean(std::span<const double> values,
                                               std::span<const std::int32_t> groups);

/// Median. Exposed because it is the robust centre used by several transforms
/// and deserves direct testing.
[[nodiscard]] double median_of(std::span<const double> values);

// ---------------------------------------------------------------------------
// The barrier
// ---------------------------------------------------------------------------

struct UniverseMember {
    InstrumentId instrument{kInvalidInstrument};
    /// Group id for sector-neutral transforms; -1 means ungrouped.
    std::int32_t sector = -1;
    /// True for the market proxy (SPY). Exactly one member may be marked.
    bool is_market_proxy = false;
};

struct CrossSectionalConfig {
    std::vector<UniverseMember> universe;
    bool winsorize_inputs = true;
    double winsor_lower = 0.01;
    double winsor_upper = 0.99;
    bool sector_neutral = false;
    /// Require every member to have contributed a same-bar state before the
    /// barrier will produce output. On by default: a partial cross-section
    /// silently changes what a rank means.
    bool require_complete_universe = true;
};

/// One instrument's cross-sectional output for one bar.
struct CrossSectionalRow {
    InstrumentId instrument{kInvalidInstrument};
    Timestamp feature_end_time{kNoTimestamp};
    double market_relative_return = 0.0;
    double sector_relative_return = 0.0;
    double return_rank = 0.5;
    double return_zscore = 0.0;
    double volume_rank = 0.5;
    bool ready = false;
};

class CrossSectionalStage {
public:
    explicit CrossSectionalStage(CrossSectionalConfig cfg);

    /// Register one instrument's state for the current bar.
    ///
    /// \returns an error when `feature_end_time` disagrees with the timestamp
    ///          already established for this bar. That disagreement is the
    ///          symptom of reading a neighbour's stale or future state, and it
    ///          is refused rather than tolerated.
    [[nodiscard]] Result<bool> contribute(InstrumentId instrument, Timestamp feature_end_time,
                                          double log_return, double volume);

    /// Compute the cross-section once every member has contributed.
    [[nodiscard]] Result<std::vector<CrossSectionalRow>> compute();

    /// Market proxy return for this bar, if the proxy has contributed.
    [[nodiscard]] std::optional<double> market_return() const noexcept;

    [[nodiscard]] std::size_t contributed() const noexcept { return pending_.size(); }
    [[nodiscard]] Timestamp bar_time() const noexcept { return bar_time_; }
    [[nodiscard]] const CrossSectionalConfig& config() const noexcept { return cfg_; }

    /// Clear for the next bar. Called automatically by compute().
    void clear() noexcept;

private:
    struct Contribution {
        InstrumentId instrument{kInvalidInstrument};
        std::int32_t sector = -1;
        double log_return = 0.0;
        double volume = 0.0;
        bool is_proxy = false;
    };

    CrossSectionalConfig cfg_;
    std::map<std::uint32_t, Contribution> pending_;
    Timestamp bar_time_{kNoTimestamp};
};

}  // namespace ptl::features
