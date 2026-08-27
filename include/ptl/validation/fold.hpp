#pragma once

/// \file fold.hpp
/// Time-series folds with purging and embargo.
///
/// Three sets per fold, not two: TRAIN, VALIDATION and TEST. Hyperparameters
/// are selected on validation; test is scored once. Collapsing validation into
/// test -- the common shortcut -- means the reported out-of-sample number was
/// the one being optimised against, and is therefore in-sample.

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "ptl/core/time.hpp"
#include "ptl/core/types.hpp"

namespace ptl::validation {

enum class WindowMode : std::uint8_t {
    /// Train on a fixed-length trailing window. Adapts to regime change at the
    /// cost of sample size.
    Rolling,
    /// Train on everything since the start. More data, but assumes the distant
    /// past still informs the present.
    Expanding,
};

[[nodiscard]] std::string_view to_string(WindowMode) noexcept;

enum class SetKind : std::uint8_t { Train, Validation, Test, Purged, Embargoed, Unused };

[[nodiscard]] std::string_view to_string(SetKind) noexcept;

/// One walk-forward fold.
///
/// Row indices, not timestamps: the caller supplies observations in
/// chronological order and receives index sets, so folds cost nothing to carry
/// around and cannot drift from the data they describe.
struct Fold {
    int fold_id = 0;
    WindowMode mode{WindowMode::Expanding};

    Timestamp train_begin{kNoTimestamp};
    Timestamp train_end{kNoTimestamp};
    Timestamp validation_begin{kNoTimestamp};
    Timestamp validation_end{kNoTimestamp};
    Timestamp test_begin{kNoTimestamp};
    Timestamp test_end{kNoTimestamp};

    std::vector<std::size_t> train_rows;
    std::vector<std::size_t> validation_rows;
    std::vector<std::size_t> test_rows;

    /// Dropped because their label interval overlapped a later set.
    std::vector<std::size_t> purged_rows;
    /// Dropped because they fall inside the post-test embargo.
    std::vector<std::size_t> embargoed_rows;

    [[nodiscard]] std::size_t size() const noexcept {
        return train_rows.size() + validation_rows.size() + test_rows.size();
    }
    /// No index may appear in more than one set. Checked directly by the fold
    /// disjointness test.
    [[nodiscard]] bool disjoint() const;
    [[nodiscard]] std::string describe() const;
};

struct WalkForwardConfig {
    WindowMode mode{WindowMode::Expanding};

    /// Window lengths in OBSERVATIONS. Bars rather than calendar days, so a
    /// half day does not silently shorten a fold.
    std::size_t train_size = 20000;
    std::size_t validation_size = 4000;
    std::size_t test_size = 4000;
    std::size_t step = 4000;

    /// Minimum bars of history before the first fold may begin.
    std::size_t warmup = 0;

    /// Extra observations dropped after the test window. Predeclared and
    /// hashed: choosing an embargo after seeing results is a form of tuning.
    std::size_t embargo = 0;

    /// Refuse to emit a fold whose train set is smaller than this. A fold with
    /// two hundred rows produces a model, and that model produces a number, and
    /// nothing downstream would indicate it was meaningless.
    std::size_t min_train_rows = 500;

    /// Purge training rows whose label interval overlaps a later set. On by
    /// default; turning it off is only for demonstrating the contamination it
    /// prevents.
    bool purge = true;

    [[nodiscard]] std::string signature() const;
    [[nodiscard]] std::uint64_t hash() const;
};

}  // namespace ptl::validation
