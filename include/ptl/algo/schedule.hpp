#pragma once

/// \file schedule.hpp
/// Time-sliced execution schedules.
///
/// A schedule is computed ONCE, up front, from the parent order and the
/// execution window. That is what makes it inspectable: a reviewer can look at
/// the plan before a single child is sent and see exactly how the order will be
/// worked, rather than reconstructing it afterwards from fills.
///
/// Schedules are TARGETS, not commitments. The algorithm consults the schedule
/// and then applies the policy -- participation caps, minimum clips, market
/// hours -- so a slice can be reduced or skipped without the schedule being
/// wrong.

#include <cstdint>
#include <span>
#include <vector>

#include "ptl/algo/execution_types.hpp"
#include "ptl/core/result.hpp"

namespace ptl::algo {

/// One slice of an execution schedule.
struct ScheduleSlice {
    std::size_t index = 0;
    /// Half-open [begin, end): the interval during which this slice is due.
    Timestamp begin{kNoTimestamp};
    Timestamp end{kNoTimestamp};
    /// Quantity this slice targets.
    Qty target_quantity{};
    /// Cumulative target through the end of this slice. The algorithm chases
    /// the CUMULATIVE figure rather than the per-slice one, so a slice that was
    /// skipped or capped is made up later instead of being lost.
    Qty cumulative_target{};
};

class ExecutionSchedule {
public:
    ExecutionSchedule() = default;

    [[nodiscard]] std::span<const ScheduleSlice> slices() const noexcept { return slices_; }
    [[nodiscard]] std::size_t size() const noexcept { return slices_.size(); }
    [[nodiscard]] bool empty() const noexcept { return slices_.empty(); }
    [[nodiscard]] Timestamp begin() const noexcept;
    [[nodiscard]] Timestamp end() const noexcept;
    [[nodiscard]] Qty total_quantity() const noexcept;

    /// The slice covering `ts`, or nullptr outside the window.
    [[nodiscard]] const ScheduleSlice* slice_at(Timestamp ts) const noexcept;

    /// Cumulative target quantity due by `ts`, interpolated within the current
    /// slice. This is what an algorithm compares its fills against to decide
    /// whether it is ahead of or behind schedule.
    [[nodiscard]] Qty target_by(Timestamp ts) const noexcept;

    /// Equal quantity per equal time slice.
    [[nodiscard]] static Result<ExecutionSchedule> twap(Qty total, Timestamp begin, Timestamp end,
                                                        std::size_t slice_count);

    /// Quantity proportional to an expected volume profile.
    ///
    /// \param profile expected share of window volume per slice. Need not sum
    ///        to one; it is normalised, because a profile that does not sum to
    ///        one is far more common than a bug and rejecting it would be
    ///        pedantic. A profile summing to ZERO is refused, since it carries
    ///        no information at all.
    [[nodiscard]] static Result<ExecutionSchedule> vwap(Qty total, Timestamp begin, Timestamp end,
                                                        std::span<const double> profile);

    /// One slice covering the whole window: everything, immediately.
    [[nodiscard]] static Result<ExecutionSchedule> immediate(Qty total, Timestamp begin,
                                                             Timestamp end);

private:
    std::vector<ScheduleSlice> slices_;
};

/// A minute-of-session volume profile, for VWAP.
///
/// Built from HISTORY ONLY -- the profile for a session must be estimated from
/// PREVIOUS sessions, never from the session being traded. Using the current
/// session's realised volume would be a textbook lookahead: the algorithm would
/// know where the volume was going to be.
class VolumeProfile {
public:
    explicit VolumeProfile(std::size_t slots = 390);

    /// Fold in one completed session's per-slot volumes.
    [[nodiscard]] Result<bool> add_session(std::span<const double> slot_volumes);

    /// Normalised expected share per slot. Empty until at least one session has
    /// been observed.
    [[nodiscard]] std::span<const double> shares() const noexcept { return shares_; }
    [[nodiscard]] std::size_t sessions_observed() const noexcept { return sessions_; }
    [[nodiscard]] bool ready() const noexcept { return sessions_ > 0; }

    /// Expected share over a contiguous slot range, for a window that covers
    /// only part of the session.
    [[nodiscard]] Result<std::vector<double>> window_profile(std::size_t first_slot,
                                                             std::size_t slot_count) const;

    /// A flat profile, for when no history exists. Named explicitly so a report
    /// can say the VWAP degenerated to a TWAP rather than silently doing so.
    [[nodiscard]] static std::vector<double> uniform(std::size_t slots);

    void reset() noexcept;

private:
    std::vector<double> totals_;
    std::vector<double> shares_;
    std::size_t sessions_ = 0;
};

}  // namespace ptl::algo
