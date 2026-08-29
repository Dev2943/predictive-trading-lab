#include "ptl/algo/schedule.hpp"

#include <algorithm>
#include <cmath>
#include <numeric>
#include <sstream>

namespace ptl::algo {
namespace {

[[nodiscard]] Error bad(std::string message, std::string context = {}) {
    return make_error(ErrorCode::ValidationFailed, std::move(message), std::move(context));
}

}  // namespace

std::string_view to_string(AlgoKind k) noexcept {
    switch (k) {
        case AlgoKind::Immediate:
            return "immediate";
        case AlgoKind::Twap:
            return "twap";
        case AlgoKind::Vwap:
            return "vwap";
        case AlgoKind::Participation:
            return "pov";
        case AlgoKind::Iceberg:
            return "iceberg";
        case AlgoKind::AdaptiveLimit:
            return "adaptive_limit";
    }
    return "unknown";
}

std::string_view to_string(ExecutionState s) noexcept {
    switch (s) {
        case ExecutionState::Pending:
            return "pending";
        case ExecutionState::Working:
            return "working";
        case ExecutionState::Completed:
            return "completed";
        case ExecutionState::Cancelled:
            return "cancelled";
        case ExecutionState::Expired:
            return "expired";
        case ExecutionState::Failed:
            return "failed";
    }
    return "unknown";
}

bool is_terminal(ExecutionState s) noexcept {
    return s == ExecutionState::Completed || s == ExecutionState::Cancelled ||
           s == ExecutionState::Expired || s == ExecutionState::Failed;
}

bool ChildOrder::valid() const noexcept {
    if (instrument == kInvalidInstrument) return false;
    if (!is_finite(quantity.get()) || quantity.get() <= 0.0) return false;
    const bool needs_limit = type == oms::OrderType::Limit || type == oms::OrderType::StopLimit;
    const bool needs_stop = type == oms::OrderType::Stop || type == oms::OrderType::StopLimit;
    // Type is the authority and price engagement follows from it, exactly as in
    // oms::Order. Checking here too means a malformed child is caught before it
    // reaches the OMS, where the error would name a different layer.
    if (needs_limit != limit_price.has_value()) return false;
    if (needs_stop != stop_price.has_value()) return false;
    if (limit_price.has_value() && limit_price->get() <= 0.0) return false;
    if (stop_price.has_value() && stop_price->get() <= 0.0) return false;
    return true;
}

double ExecutionProgress::completion_ratio(Qty parent_quantity) const noexcept {
    const double total = parent_quantity.get();
    if (!is_finite(total) || total <= 0.0) return 0.0;
    return std::clamp(filled.get() / total, 0.0, 1.0);
}

std::optional<Price> ExecutionProgress::average_price() const noexcept {
    if (filled.get() <= 0.0) return std::nullopt;
    const double avg = filled_notional.get() / filled.get();
    if (!is_finite(avg) || avg <= 0.0) return std::nullopt;
    return Price{avg};
}

bool ExecutionProgress::complete(Qty parent_quantity) const noexcept {
    // Tolerance, not exact equality: a lot-rounded schedule can leave a
    // fractional share outstanding that will never fill, and treating that as
    // incomplete would leave the execution working forever.
    return filled.get() + 1e-9 >= parent_quantity.get();
}

std::string ExecutionPolicy::signature() const {
    std::ostringstream ss;
    ss.precision(17);
    ss << "pov=" << max_participation_rate << "|clip=" << min_clip_size.get()
       << "|display=" << max_display_quantity.get() << "|collar=" << price_collar.get()
       << "|hours=" << (respect_market_hours ? 1 : 0) << "|lot=" << lot_size
       << "|expire=" << (expire_rather_than_dump ? 1 : 0);
    return ss.str();
}

std::uint64_t ExecutionPolicy::hash() const {
    return fnv1a64(signature());
}

Bps ExecutionResult::shortfall_bps(Side side) const noexcept {
    const auto avg = progress.average_price();
    if (!avg.has_value() || arrival_price.get() <= 0.0) return Bps{0.0};
    const double raw = (avg->get() / arrival_price.get() - 1.0) * 1e4;
    // Signed so POSITIVE ALWAYS MEANS COST. A buy filling above arrival and a
    // sell filling below both hurt.
    return Bps{raw * static_cast<double>(sign_of(side))};
}

Duration ExecutionResult::duration() const noexcept {
    if (!is_set(started) || !is_set(finished)) return Duration::zero();
    return finished - started;
}

std::string ExecutionResult::describe() const {
    std::ostringstream ss;
    ss.precision(4);
    ss << std::fixed << to_string(state) << ": filled " << progress.filled.get() << " of "
       << (progress.filled.get() + progress.remaining.get()) << " over " << progress.slices_released
       << " slices";
    if (const auto avg = progress.average_price()) {
        ss << " at " << avg->get();
    }
    if (!detail.empty()) ss << " (" << detail << ")";
    return ss.str();
}

std::string ExecutionStatistics::describe() const {
    std::ostringstream ss;
    ss << "executions: " << executions_started << " started, " << executions_completed
       << " completed, " << executions_expired << " expired, " << executions_cancelled
       << " cancelled\n";
    ss << "  child orders: " << child_orders_emitted << " emitted, " << child_orders_rejected
       << " rejected\n";
    ss << "  slices skipped: " << slices_skipped_participation << " participation, "
       << slices_skipped_min_clip << " min clip, " << slices_skipped_market_closed
       << " market closed, " << slices_skipped_not_executable << " not executable\n";
    ss << "  limit reprices: " << limit_repriced << '\n';
    return ss.str();
}

// ---------------------------------------------------------------------------
// ExecutionSchedule
// ---------------------------------------------------------------------------

Timestamp ExecutionSchedule::begin() const noexcept {
    return slices_.empty() ? kNoTimestamp : slices_.front().begin;
}

Timestamp ExecutionSchedule::end() const noexcept {
    return slices_.empty() ? kNoTimestamp : slices_.back().end;
}

Qty ExecutionSchedule::total_quantity() const noexcept {
    return slices_.empty() ? Qty{0.0} : slices_.back().cumulative_target;
}

const ScheduleSlice* ExecutionSchedule::slice_at(Timestamp ts) const noexcept {
    // Half-open [begin, end) throughout, so adjacent slices cannot both claim
    // the boundary instant.
    const auto it = std::upper_bound(slices_.begin(), slices_.end(), ts,
                                     [](Timestamp t, const ScheduleSlice& s) { return t < s.end; });
    if (it == slices_.end()) return nullptr;
    if (ts < it->begin) return nullptr;
    return &*it;
}

Qty ExecutionSchedule::target_by(Timestamp ts) const noexcept {
    if (slices_.empty()) return Qty{0.0};
    if (ts < slices_.front().begin) return Qty{0.0};
    if (ts >= slices_.back().end) return slices_.back().cumulative_target;

    const ScheduleSlice* slice = slice_at(ts);
    if (slice == nullptr) return Qty{0.0};

    // Interpolate WITHIN the current slice. Stepping the target only at slice
    // boundaries would make an algorithm alternate between far behind and
    // exactly on schedule, and every clip would be a full slice regardless of
    // how much time had elapsed.
    const double span = static_cast<double>((slice->end - slice->begin).count());
    const double elapsed = static_cast<double>((ts - slice->begin).count());
    const double fraction = span > 0.0 ? std::clamp(elapsed / span, 0.0, 1.0) : 1.0;

    const double before = slice->cumulative_target.get() - slice->target_quantity.get();
    return Qty{before + slice->target_quantity.get() * fraction};
}

Result<ExecutionSchedule> ExecutionSchedule::twap(Qty total, Timestamp begin, Timestamp end,
                                                  std::size_t slice_count) {
    if (!is_finite(total.get()) || total.get() <= 0.0) {
        return fail(bad("schedule quantity must be positive"));
    }
    if (!is_set(begin) || !is_set(end) || end <= begin) {
        return fail(bad("schedule window must be a positive duration"));
    }
    if (slice_count == 0) return fail(bad("schedule needs at least one slice"));

    ExecutionSchedule out;
    const Duration span = end - begin;
    const auto per = span / static_cast<std::int64_t>(slice_count);
    const double per_slice = total.get() / static_cast<double>(slice_count);

    double cumulative = 0.0;
    for (std::size_t i = 0; i < slice_count; ++i) {
        ScheduleSlice s;
        s.index = i;
        s.begin = begin + per * static_cast<std::int64_t>(i);
        // The LAST slice ends exactly at `end` rather than at begin + n*per.
        // Integer division truncates, and without this the window would be
        // short by up to slice_count nanoseconds -- which is harmless until a
        // caller compares schedule.end() against the session close.
        s.end = i + 1 == slice_count ? end : begin + per * static_cast<std::int64_t>(i + 1);
        s.target_quantity = Qty{per_slice};
        cumulative += per_slice;
        s.cumulative_target = Qty{cumulative};
        out.slices_.push_back(s);
    }
    // Snap the final cumulative to the exact total, so accumulated
    // floating-point error cannot leave the execution permanently one
    // ten-thousandth of a share short of complete.
    out.slices_.back().cumulative_target = total;
    return out;
}

Result<ExecutionSchedule> ExecutionSchedule::vwap(Qty total, Timestamp begin, Timestamp end,
                                                  std::span<const double> profile) {
    if (!is_finite(total.get()) || total.get() <= 0.0) {
        return fail(bad("schedule quantity must be positive"));
    }
    if (!is_set(begin) || !is_set(end) || end <= begin) {
        return fail(bad("schedule window must be a positive duration"));
    }
    if (profile.empty()) return fail(bad("VWAP schedule needs a volume profile"));

    double sum = 0.0;
    for (const double w : profile) {
        if (!is_finite(w) || w < 0.0) {
            return fail(bad("volume profile contains a negative or non-finite weight"));
        }
        sum += w;
    }
    if (sum <= 0.0) {
        // A profile summing to zero carries no information; degenerating to a
        // TWAP silently would hide that the profile was unusable.
        return fail(bad("volume profile sums to zero and cannot allocate quantity"));
    }

    ExecutionSchedule out;
    const Duration span = end - begin;
    const auto per = span / static_cast<std::int64_t>(profile.size());

    double cumulative = 0.0;
    for (std::size_t i = 0; i < profile.size(); ++i) {
        ScheduleSlice s;
        s.index = i;
        s.begin = begin + per * static_cast<std::int64_t>(i);
        s.end = i + 1 == profile.size() ? end : begin + per * static_cast<std::int64_t>(i + 1);
        // Quantity proportional to expected volume: trade more where the market
        // trades more, which is the whole point of a VWAP.
        const double share = profile[i] / sum;
        s.target_quantity = Qty{total.get() * share};
        cumulative += s.target_quantity.get();
        s.cumulative_target = Qty{cumulative};
        out.slices_.push_back(s);
    }
    out.slices_.back().cumulative_target = total;
    return out;
}

Result<ExecutionSchedule> ExecutionSchedule::immediate(Qty total, Timestamp begin, Timestamp end) {
    return twap(total, begin, end, 1);
}

// ---------------------------------------------------------------------------
// VolumeProfile
// ---------------------------------------------------------------------------

VolumeProfile::VolumeProfile(std::size_t slots) : totals_(std::max<std::size_t>(1, slots), 0.0) {}

Result<bool> VolumeProfile::add_session(std::span<const double> slot_volumes) {
    if (slot_volumes.size() != totals_.size()) {
        return fail(bad("session volume vector has " + std::to_string(slot_volumes.size()) +
                        " slots, profile expects " + std::to_string(totals_.size())));
    }
    for (std::size_t i = 0; i < slot_volumes.size(); ++i) {
        const double v = slot_volumes[i];
        if (!is_finite(v) || v < 0.0) {
            return fail(
                bad("session volume is negative or non-finite at slot " + std::to_string(i)));
        }
        totals_[i] += v;
    }
    ++sessions_;

    const double sum = std::accumulate(totals_.begin(), totals_.end(), 0.0);
    shares_.assign(totals_.size(), 0.0);
    if (sum > 0.0) {
        for (std::size_t i = 0; i < totals_.size(); ++i) shares_[i] = totals_[i] / sum;
    }
    return true;
}

Result<std::vector<double>> VolumeProfile::window_profile(std::size_t first_slot,
                                                          std::size_t slot_count) const {
    if (!ready()) {
        return fail(
            bad("volume profile has no observed sessions; a VWAP built from it "
                "would be a TWAP wearing the wrong name"));
    }
    if (slot_count == 0) return fail(bad("window profile needs at least one slot"));
    if (first_slot + slot_count > shares_.size()) {
        return fail(bad("window extends beyond the profile: slots " + std::to_string(first_slot) +
                        "+" + std::to_string(slot_count) + " of " +
                        std::to_string(shares_.size())));
    }
    return std::vector<double>{
        shares_.begin() + static_cast<std::ptrdiff_t>(first_slot),
        shares_.begin() + static_cast<std::ptrdiff_t>(first_slot + slot_count)};
}

std::vector<double> VolumeProfile::uniform(std::size_t slots) {
    const std::size_t n = std::max<std::size_t>(1, slots);
    return std::vector<double>(n, 1.0 / static_cast<double>(n));
}

void VolumeProfile::reset() noexcept {
    std::fill(totals_.begin(), totals_.end(), 0.0);
    shares_.clear();
    sessions_ = 0;
}

}  // namespace ptl::algo
