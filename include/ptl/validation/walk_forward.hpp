#pragma once

/// \file walk_forward.hpp
/// Fold generation and the holdout guard.

#include <span>
#include <string>
#include <utility>
#include <vector>

#include "ptl/core/result.hpp"
#include "ptl/core/time.hpp"
#include "ptl/validation/fold.hpp"

namespace ptl::validation {

/// Splits an ordered observation series into walk-forward folds.
///
/// NEVER SHUFFLES. There is no code path that reorders observations, and the
/// shuffle-invariance test deliberately shuffles the input to prove the
/// resulting metrics change dramatically -- demonstrating the pipeline is
/// sensitive to the thing that should break it.
class WalkForwardValidator {
public:
    explicit WalkForwardValidator(WalkForwardConfig cfg) : cfg_(cfg) {}

    /// \param intervals one per observation, in chronological order by
    ///        feature_end_time. The four stamps are what make interval-overlap
    ///        purging possible.
    [[nodiscard]] Result<std::vector<Fold>> split(
        std::span<const ObservationInterval> intervals) const;

    /// Both window modes over the same data.
    ///
    /// The research requires BOTH to be reported rather than choosing whichever
    /// backtests better -- picking the window mode by its Sharpe is itself a
    /// form of selection bias.
    [[nodiscard]] Result<std::pair<std::vector<Fold>, std::vector<Fold>>> split_both_modes(
        std::span<const ObservationInterval> intervals) const;

    [[nodiscard]] const WalkForwardConfig& config() const noexcept { return cfg_; }

private:
    WalkForwardConfig cfg_;
};

// ---------------------------------------------------------------------------
// Holdout
// ---------------------------------------------------------------------------

/// Guards the chronologically final, locked evaluation period.
///
/// The holdout only means anything if touching it is (a) hard and (b) leaves a
/// permanent mark. This class enforces both: data at or after the boundary is
/// refused outright unless the guard was explicitly unlocked with a written
/// justification, and every unlock is appended to the trial registry with no
/// delete path.
///
/// The boundary is fixed BEFORE any data is examined and enters the config
/// hash, so it cannot be quietly moved after seeing a disappointing result.
class HoldoutGuard {
public:
    /// \param boundary first instant belonging to the holdout.
    explicit HoldoutGuard(Timestamp boundary) noexcept : boundary_(boundary) {}

    /// Unlock, permanently, for this process. Requires a justification: an
    /// unlock nobody had to explain is an unlock nobody will remember.
    [[nodiscard]] Result<bool> unlock(std::string justification);

    [[nodiscard]] bool is_holdout(Timestamp ts) const noexcept {
        return is_set(boundary_) && ts >= boundary_;
    }

    /// \returns an error when `ts` is inside a locked holdout.
    [[nodiscard]] Result<bool> check(Timestamp ts) const;

    /// Filter a series to the development region. Rows inside a locked holdout
    /// are DROPPED rather than erroring, so ordinary research code needs no
    /// special case -- but the count is reported so the drop is never silent.
    struct FilterResult {
        std::vector<std::size_t> allowed_rows;
        std::size_t withheld = 0;
    };
    [[nodiscard]] FilterResult filter(std::span<const ObservationInterval> intervals) const;

    /// Refuse a fold that reaches into a locked holdout. The fold's LABEL
    /// intervals are checked, not merely its decision times: a label whose
    /// horizon extends past the boundary reads holdout prices.
    [[nodiscard]] Result<bool> check_fold(const Fold&,
                                          std::span<const ObservationInterval> intervals) const;

    [[nodiscard]] Timestamp boundary() const noexcept { return boundary_; }
    [[nodiscard]] bool unlocked() const noexcept { return unlocked_; }
    [[nodiscard]] const std::string& justification() const noexcept { return justification_; }

private:
    Timestamp boundary_{kNoTimestamp};
    bool unlocked_ = false;
    std::string justification_;
};

}  // namespace ptl::validation
