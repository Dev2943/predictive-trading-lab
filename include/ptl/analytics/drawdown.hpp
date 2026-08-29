#pragma once

/// \file drawdown.hpp
/// Drawdown tracking and the underwater curve.
///
/// INCREMENTAL, not recomputed. A tracker fed one equity point at a time
/// answers "what is the drawdown now?" in constant time, which is what lets a
/// live risk check consult it on every bar. Phase 3's MetricsEngine computes a
/// max drawdown over a finished curve; this is the streaming counterpart, and
/// the two are cross-checked against each other in the tests.
///
/// PURE ANALYSIS. Nothing here touches trading state. A DrawdownTracker holds
/// its own accumulators and reads equity values it is handed.

#include <cstddef>
#include <span>
#include <string>
#include <vector>

#include "ptl/core/result.hpp"
#include "ptl/core/types.hpp"
#include "ptl/portfolio/portfolio.hpp"

namespace ptl::analytics {

/// One point of the underwater curve.
struct UnderwaterPoint {
    Timestamp ts{kNoTimestamp};
    /// Fraction below the running peak, in [0, 1]. Zero at a new high.
    double drawdown = 0.0;
    Notional equity{};
    Notional peak{};
    /// Periods since the peak this drawdown started from.
    std::size_t periods_underwater = 0;
};

/// One completed peak-to-recovery episode.
struct DrawdownEpisode {
    Timestamp peak_time{kNoTimestamp};
    Timestamp trough_time{kNoTimestamp};
    /// Unset while the episode has not recovered.
    Timestamp recovery_time{kNoTimestamp};
    Notional peak_equity{};
    Notional trough_equity{};
    double depth = 0.0;

    [[nodiscard]] bool recovered() const noexcept { return is_set(recovery_time); }
    /// Peak to trough.
    [[nodiscard]] Duration decline_duration() const noexcept;
    /// Peak to recovery. Zero while still underwater -- an unrecovered episode
    /// has no duration yet, and reporting one would understate the pain.
    [[nodiscard]] Duration total_duration() const noexcept;
};

class DrawdownTracker {
public:
    /// Fold in one equity observation. Must be non-decreasing in time.
    [[nodiscard]] Result<bool> update(Timestamp, Notional equity);

    [[nodiscard]] double current_drawdown() const noexcept { return current_; }
    [[nodiscard]] double max_drawdown() const noexcept { return max_; }
    [[nodiscard]] Notional peak_equity() const noexcept { return peak_; }
    [[nodiscard]] Timestamp max_drawdown_peak() const noexcept { return max_peak_time_; }
    [[nodiscard]] Timestamp max_drawdown_trough() const noexcept { return max_trough_time_; }
    [[nodiscard]] bool underwater() const noexcept { return current_ > 0.0; }
    [[nodiscard]] std::size_t periods_underwater() const noexcept { return underwater_; }

    /// Longest stretch below a peak, in observations. A shallow drawdown that
    /// lasts a year is a different problem from a deep one that recovers in a
    /// week, and reporting only depth hides that entirely.
    [[nodiscard]] std::size_t longest_underwater_periods() const noexcept {
        return longest_underwater_;
    }

    [[nodiscard]] std::span<const UnderwaterPoint> underwater_curve() const noexcept {
        return curve_;
    }
    [[nodiscard]] std::span<const DrawdownEpisode> episodes() const noexcept { return episodes_; }
    [[nodiscard]] std::size_t size() const noexcept { return curve_.size(); }

    void reset() noexcept;

private:
    double current_ = 0.0;
    double max_ = 0.0;
    Notional peak_{};
    Timestamp peak_time_{kNoTimestamp};
    Timestamp last_ts_{kNoTimestamp};
    Timestamp max_peak_time_{kNoTimestamp};
    Timestamp max_trough_time_{kNoTimestamp};
    std::size_t underwater_ = 0;
    std::size_t longest_underwater_ = 0;
    bool have_peak_ = false;

    std::vector<UnderwaterPoint> curve_;
    std::vector<DrawdownEpisode> episodes_;
    /// The episode in progress, if any.
    bool in_episode_ = false;
    DrawdownEpisode active_{};
};

/// Maximum drawdown within a trailing window of `window` observations.
///
/// Distinct from the all-time figure: a strategy whose worst 60-day drawdown is
/// 8% behaves differently from one whose worst 60-day drawdown is 25%, even if
/// both have the same all-time maximum.
[[nodiscard]] Result<std::vector<double>> rolling_max_drawdown(
    std::span<const portfolio::EquityPoint> curve, std::size_t window);

}  // namespace ptl::analytics
