#include "ptl/algo/algorithms.hpp"

#include <algorithm>
#include <cmath>

namespace ptl::algo {
namespace {

[[nodiscard]] Error bad(std::string message, std::string context = {}) {
    return make_error(ErrorCode::ValidationFailed, std::move(message), std::move(context));
}

}  // namespace

Result<bool> ExecutionRequest::validate() const {
    if (parent.quantity().get() <= 0.0) {
        return fail(bad("execution request has no parent quantity"));
    }
    if (!is_set(window_begin) || !is_set(window_end) || window_end <= window_begin) {
        return fail(bad("execution window must be a positive duration"));
    }
    if (window_begin < parent.decision_time()) {
        // An execution window opening before the decision that produced the
        // parent would let a child order arrive before its own parent existed.
        return fail(bad("execution window begins before the parent's decision time",
                        to_iso8601(window_begin) + " < " + to_iso8601(parent.decision_time())));
    }
    if (policy.max_participation_rate <= 0.0 || policy.max_participation_rate > 1.0) {
        return fail(bad("participation rate must lie in (0, 1]"));
    }
    if (policy.min_clip_size.get() < 0.0) {
        return fail(bad("minimum clip size cannot be negative"));
    }
    return true;
}

// ---------------------------------------------------------------------------
// AlgorithmBase: shared constraint application
// ---------------------------------------------------------------------------

bool AlgorithmBase::releasable(const ExecutionRequest& request, const ExecutionContext& ctx,
                               ExecutionStatistics* stats) {
    // The market must actually be executable: halted, stale, crossed and absent
    // quotes all mean no venue would accept the child.
    if (!ctx.market.executable) {
        if (stats != nullptr) ++stats->slices_skipped_not_executable;
        return false;
    }

    if (request.policy.respect_market_hours) {
        // Auction protection. The open and close auctions have different
        // microstructure, and an algorithm calibrated on continuous trading
        // does not describe them.
        if (is_set(ctx.session_open) && ctx.now < ctx.session_open + request.policy.open_buffer) {
            if (stats != nullptr) ++stats->slices_skipped_market_closed;
            return false;
        }
        if (is_set(ctx.session_close) &&
            ctx.now >= ctx.session_close - request.policy.close_buffer) {
            if (stats != nullptr) ++stats->slices_skipped_market_closed;
            return false;
        }
    }

    // Outside the execution window nothing is released. The window is the
    // caller's statement of when this order may trade.
    if (ctx.now < request.window_begin || ctx.now >= request.window_end) {
        if (stats != nullptr) ++stats->slices_skipped_market_closed;
        return false;
    }
    return true;
}

Qty AlgorithmBase::apply_policy_caps(const ExecutionRequest& request, const ExecutionContext& ctx,
                                     double desired, Qty remaining, ExecutionStatistics* stats) {
    // Never exceed what is actually left of the parent.
    double want = std::min(desired, remaining.get());
    if (!is_finite(want) || want <= 0.0) return Qty{0.0};

    // Participation cap against OBSERVED interval volume.
    if (ctx.interval_volume.get() > 0.0) {
        const double cap = ctx.interval_volume.get() * request.policy.max_participation_rate;
        if (cap < want) {
            want = cap;
            if (stats != nullptr) ++stats->slices_skipped_participation;
        }
    }

    // Display cap: never show more than the policy permits at once.
    if (request.policy.max_display_quantity.get() > 0.0) {
        want = std::min(want, request.policy.max_display_quantity.get());
    }

    // Round toward zero, so rounding can never breach a cap just applied.
    if (request.policy.lot_size > 0.0) {
        want = std::trunc(want / request.policy.lot_size) * request.policy.lot_size;
    }

    // Minimum clip LAST, after every reduction. Checking it earlier would let a
    // participation cap shrink a clip below the floor and send it anyway.
    if (want < request.policy.min_clip_size.get()) {
        // One exception: if the remainder itself is below the floor, send it
        // anyway. Otherwise the execution can never complete -- it would sit
        // forever holding a residue too small to be worth sending.
        const bool is_final_residue = remaining.get() <= request.policy.min_clip_size.get();
        if (!is_final_residue) {
            if (stats != nullptr) ++stats->slices_skipped_min_clip;
            return Qty{0.0};
        }
        want = remaining.get();
    }
    return Qty{std::max(0.0, want)};
}

Qty AlgorithmBase::clip_quantity(const ExecutionRequest& request, const ExecutionSchedule& schedule,
                                 const ExecutionProgress& progress, const ExecutionContext& ctx,
                                 ExecutionStatistics* stats) {
    // CHASE THE CUMULATIVE TARGET, not the per-slice one. A slice that was
    // skipped or capped is then made up later rather than being lost, which is
    // what keeps an execution on track after a halt or a thin interval.
    const double due = schedule.target_by(ctx.now).get();
    const double shortfall = due - progress.filled.get();
    return apply_policy_caps(request, ctx, shortfall, progress.remaining, stats);
}

std::optional<Price> AlgorithmBase::collared_limit(const ExecutionRequest& request,
                                                   const ExecutionContext& ctx, Bps offset) {
    if (!ctx.market.executable) return std::nullopt;
    const Side side = request.parent.side();
    const Price touch = ctx.market.state.touch(side);
    if (touch.get() <= 0.0) return std::nullopt;

    // Offset AWAY from the touch: a buy limit below, a sell limit above.
    // Offsetting the other way would cross the spread and make the limit a
    // market order wearing a limit's name.
    const Price limit = apply_bps(touch, offset, -sign_of(side));

    // The collar is measured against the touch. A stale or corrupt reference
    // would otherwise produce an order at an absurd level.
    const double deviation = std::abs(to_bps(limit, touch).get());
    if (deviation > request.policy.price_collar.get()) {
        return apply_bps(touch, request.policy.price_collar, -sign_of(side));
    }
    return limit;
}

// ---------------------------------------------------------------------------
// Immediate
// ---------------------------------------------------------------------------

Result<ExecutionSchedule> ImmediateAlgorithm::plan(const ExecutionRequest& request) const {
    if (auto ok = request.validate(); !ok) return fail(ok.error());
    return ExecutionSchedule::immediate(request.parent.quantity(), request.window_begin,
                                        request.window_end);
}

std::optional<ChildOrder> ImmediateAlgorithm::next_child(const ExecutionRequest& request,
                                                         const ExecutionSchedule& schedule,
                                                         const ExecutionProgress& progress,
                                                         const ExecutionContext& ctx) const {
    (void)schedule;
    if (!releasable(request, ctx, nullptr)) return std::nullopt;

    // EVERYTHING NOW, not a schedule-interpolated share. Routing this through
    // clip_quantity() would make Immediate chase a cumulative target across its
    // own window and send only the elapsed fraction -- which is a TWAP, not an
    // immediate execution. The policy caps still apply.
    const Qty clip =
        apply_policy_caps(request, ctx, progress.remaining.get(), progress.remaining, nullptr);
    if (clip.get() <= 0.0) return std::nullopt;

    ChildOrder child;
    child.instrument = request.parent.instrument();
    child.side = request.parent.side();
    child.quantity = clip;
    child.type = oms::OrderType::Market;
    child.time_in_force = request.policy.time_in_force;
    child.slice_index = progress.slices_released;
    return child;
}

std::unique_ptr<IExecutionAlgorithm> ImmediateAlgorithm::clone() const {
    return std::make_unique<ImmediateAlgorithm>();
}

// ---------------------------------------------------------------------------
// TWAP
// ---------------------------------------------------------------------------

Result<ExecutionSchedule> TwapAlgorithm::plan(const ExecutionRequest& request) const {
    if (auto ok = request.validate(); !ok) return fail(ok.error());
    return ExecutionSchedule::twap(request.parent.quantity(), request.window_begin,
                                   request.window_end,
                                   std::max<std::size_t>(1, request.slice_count));
}

std::optional<ChildOrder> TwapAlgorithm::next_child(const ExecutionRequest& request,
                                                    const ExecutionSchedule& schedule,
                                                    const ExecutionProgress& progress,
                                                    const ExecutionContext& ctx) const {
    if (!releasable(request, ctx, nullptr)) return std::nullopt;
    const Qty clip = clip_quantity(request, schedule, progress, ctx, nullptr);
    if (clip.get() <= 0.0) return std::nullopt;

    ChildOrder child;
    child.instrument = request.parent.instrument();
    child.side = request.parent.side();
    child.quantity = clip;
    child.type = oms::OrderType::Market;
    child.time_in_force = request.policy.time_in_force;
    const auto* slice = schedule.slice_at(ctx.now);
    child.slice_index = slice != nullptr ? slice->index : progress.slices_released;
    return child;
}

std::unique_ptr<IExecutionAlgorithm> TwapAlgorithm::clone() const {
    return std::make_unique<TwapAlgorithm>();
}

// ---------------------------------------------------------------------------
// VWAP
// ---------------------------------------------------------------------------

Result<ExecutionSchedule> VwapAlgorithm::plan(const ExecutionRequest& request) const {
    if (auto ok = request.validate(); !ok) return fail(ok.error());
    if (request.volume_profile.empty()) {
        // REFUSE rather than degenerate to a TWAP. A VWAP without a profile is
        // a TWAP, and silently substituting one would make a report claim a
        // volume-following execution that never happened.
        return fail(
            bad("VWAP requires a volume profile; without one it would be a TWAP, "
                "and reporting it as a VWAP would be false"));
    }
    return ExecutionSchedule::vwap(request.parent.quantity(), request.window_begin,
                                   request.window_end, request.volume_profile);
}

std::optional<ChildOrder> VwapAlgorithm::next_child(const ExecutionRequest& request,
                                                    const ExecutionSchedule& schedule,
                                                    const ExecutionProgress& progress,
                                                    const ExecutionContext& ctx) const {
    if (!releasable(request, ctx, nullptr)) return std::nullopt;
    const Qty clip = clip_quantity(request, schedule, progress, ctx, nullptr);
    if (clip.get() <= 0.0) return std::nullopt;

    ChildOrder child;
    child.instrument = request.parent.instrument();
    child.side = request.parent.side();
    child.quantity = clip;
    child.type = oms::OrderType::Market;
    child.time_in_force = request.policy.time_in_force;
    const auto* slice = schedule.slice_at(ctx.now);
    child.slice_index = slice != nullptr ? slice->index : progress.slices_released;
    return child;
}

std::unique_ptr<IExecutionAlgorithm> VwapAlgorithm::clone() const {
    return std::make_unique<VwapAlgorithm>();
}

// ---------------------------------------------------------------------------
// Participation (POV)
// ---------------------------------------------------------------------------

Result<ExecutionSchedule> ParticipationAlgorithm::plan(const ExecutionRequest& request) const {
    if (auto ok = request.validate(); !ok) return fail(ok.error());
    // POV has no time-based schedule: its pace is set by the market, not the
    // clock. The single slice exists only to bound the execution window.
    return ExecutionSchedule::immediate(request.parent.quantity(), request.window_begin,
                                        request.window_end);
}

std::optional<ChildOrder> ParticipationAlgorithm::next_child(const ExecutionRequest& request,
                                                             const ExecutionSchedule& schedule,
                                                             const ExecutionProgress& progress,
                                                             const ExecutionContext& ctx) const {
    (void)schedule;
    if (!releasable(request, ctx, nullptr)) return std::nullopt;
    if (ctx.interval_volume.get() <= 0.0) return std::nullopt;  // nothing traded

    // Pace against OBSERVED volume, not the clock. That is what distinguishes
    // POV from VWAP: VWAP follows an expected profile computed in advance,
    // whereas POV reacts to what is actually trading.
    const double effective_rate = std::min(target_rate_, request.policy.max_participation_rate);
    double want = ctx.interval_volume.get() * effective_rate;
    want = std::min(want, progress.remaining.get());

    if (request.policy.lot_size > 0.0) {
        want = std::trunc(want / request.policy.lot_size) * request.policy.lot_size;
    }
    if (want < request.policy.min_clip_size.get()) {
        const bool is_final_residue =
            progress.remaining.get() <= request.policy.min_clip_size.get();
        if (!is_final_residue) return std::nullopt;
        want = progress.remaining.get();
    }
    if (want <= 0.0) return std::nullopt;

    ChildOrder child;
    child.instrument = request.parent.instrument();
    child.side = request.parent.side();
    child.quantity = Qty{want};
    child.type = oms::OrderType::Market;
    child.time_in_force = request.policy.time_in_force;
    child.slice_index = progress.slices_released;
    return child;
}

std::unique_ptr<IExecutionAlgorithm> ParticipationAlgorithm::clone() const {
    return std::make_unique<ParticipationAlgorithm>(target_rate_);
}

// ---------------------------------------------------------------------------
// Iceberg
// ---------------------------------------------------------------------------

Result<ExecutionSchedule> IcebergAlgorithm::plan(const ExecutionRequest& request) const {
    if (auto ok = request.validate(); !ok) return fail(ok.error());
    if (display_size_.get() <= 0.0) {
        return fail(bad("iceberg display size must be positive"));
    }
    return ExecutionSchedule::immediate(request.parent.quantity(), request.window_begin,
                                        request.window_end);
}

std::optional<ChildOrder> IcebergAlgorithm::next_child(const ExecutionRequest& request,
                                                       const ExecutionSchedule& schedule,
                                                       const ExecutionProgress& progress,
                                                       const ExecutionContext& ctx) const {
    (void)schedule;
    if (!releasable(request, ctx, nullptr)) return std::nullopt;

    // REFRESH ON COMPLETION. A new clip is shown only when the previous one has
    // filled -- that is what an iceberg is. This makes NO claim about where the
    // refreshed clip lands in a queue: sampled top-of-book data cannot
    // establish that, and ADR-0003 forbids pretending otherwise.
    double want = std::min(display_size_.get(), progress.remaining.get());
    if (request.policy.lot_size > 0.0) {
        want = std::trunc(want / request.policy.lot_size) * request.policy.lot_size;
    }
    if (want <= 0.0) {
        want = progress.remaining.get();  // final residue below one lot
        if (want <= 0.0) return std::nullopt;
    }

    ChildOrder child;
    child.instrument = request.parent.instrument();
    child.side = request.parent.side();
    child.quantity = Qty{want};
    child.type = oms::OrderType::Limit;
    child.limit_price = collared_limit(request, ctx, Bps{0.0});
    if (!child.limit_price.has_value()) return std::nullopt;
    child.time_in_force = request.policy.time_in_force;
    child.slice_index = progress.slices_released;
    // The undisplayed remainder, recorded so a report can show the true size.
    child.hidden_quantity = Qty{std::max(0.0, progress.remaining.get() - want)};
    return child;
}

std::unique_ptr<IExecutionAlgorithm> IcebergAlgorithm::clone() const {
    return std::make_unique<IcebergAlgorithm>(display_size_);
}

// ---------------------------------------------------------------------------
// Adaptive limit
// ---------------------------------------------------------------------------

Result<ExecutionSchedule> AdaptiveLimitAlgorithm::plan(const ExecutionRequest& request) const {
    if (auto ok = request.validate(); !ok) return fail(ok.error());
    return ExecutionSchedule::twap(request.parent.quantity(), request.window_begin,
                                   request.window_end, std::max<std::size_t>(1, cfg_.slice_count));
}

std::optional<ChildOrder> AdaptiveLimitAlgorithm::next_child(const ExecutionRequest& request,
                                                             const ExecutionSchedule& schedule,
                                                             const ExecutionProgress& progress,
                                                             const ExecutionContext& ctx) const {
    if (!releasable(request, ctx, nullptr)) return std::nullopt;
    const Qty clip = clip_quantity(request, schedule, progress, ctx, nullptr);
    if (clip.get() <= 0.0) return std::nullopt;

    ChildOrder child;
    child.instrument = request.parent.instrument();
    child.side = request.parent.side();
    child.quantity = clip;
    child.time_in_force = request.policy.time_in_force;
    child.slice_index = progress.slices_released;

    // How far behind schedule are we, as a fraction of the parent?
    const double due = schedule.target_by(ctx.now).get();
    const double parent = request.parent.quantity().get();
    const double shortfall = parent > 0.0 ? (due - progress.filled.get()) / parent : 0.0;

    if (shortfall >= cfg_.urgency_threshold) {
        // CROSS. An adaptive algorithm that never crosses simply fails to
        // complete in a trending market, which is worse than paying the spread.
        child.type = oms::OrderType::Market;
        return child;
    }

    // Otherwise rest passively, repriced to the current touch. Repricing on
    // every quote change is what "adaptive" means here: the limit follows the
    // market rather than being left behind by it.
    child.type = oms::OrderType::Limit;
    child.limit_price = collared_limit(request, ctx, cfg_.passive_offset);
    if (!child.limit_price.has_value()) return std::nullopt;
    return child;
}

std::unique_ptr<IExecutionAlgorithm> AdaptiveLimitAlgorithm::clone() const {
    return std::make_unique<AdaptiveLimitAlgorithm>(cfg_);
}

// ---------------------------------------------------------------------------
// Registry
// ---------------------------------------------------------------------------

Result<bool> AlgorithmRegistry::register_algorithm(std::string name,
                                                   std::unique_ptr<IExecutionAlgorithm> algorithm) {
    if (name.empty()) return fail(bad("algorithm name cannot be empty"));
    if (algorithm == nullptr) return fail(bad("algorithm is null"));
    if (algorithms_.contains(name)) {
        // Silently replacing would make the active algorithm depend on
        // registration order, which is the hidden non-determinism a registry
        // exists to remove.
        return fail(bad("algorithm already registered: " + name));
    }
    algorithms_.emplace(std::move(name), std::move(algorithm));
    return true;
}

Result<std::unique_ptr<IExecutionAlgorithm>> AlgorithmRegistry::create(
    std::string_view name) const {
    const auto it = algorithms_.find(name);
    if (it == algorithms_.end()) {
        std::string msg = "unknown execution algorithm '" + std::string{name} + "'. Registered: ";
        bool first = true;
        for (const auto& [k, v] : algorithms_) {
            if (!first) msg += ", ";
            first = false;
            msg += k;
        }
        if (algorithms_.empty()) msg += "(none)";
        return fail(make_error(ErrorCode::NotFound, std::move(msg)));
    }
    return it->second->clone();
}

bool AlgorithmRegistry::contains(std::string_view name) const noexcept {
    return algorithms_.find(name) != algorithms_.end();
}

std::vector<std::string_view> AlgorithmRegistry::names() const {
    std::vector<std::string_view> out;
    out.reserve(algorithms_.size());
    // std::map iterates in key order, so a report listing algorithms is
    // identical between runs.
    for (const auto& [k, v] : algorithms_) out.emplace_back(k);
    return out;
}

Result<AlgorithmRegistry> AlgorithmRegistry::with_defaults() {
    AlgorithmRegistry reg;
    if (auto r = reg.register_algorithm("immediate", std::make_unique<ImmediateAlgorithm>()); !r) {
        return fail(r.error());
    }
    if (auto r = reg.register_algorithm("twap", std::make_unique<TwapAlgorithm>()); !r) {
        return fail(r.error());
    }
    if (auto r = reg.register_algorithm("vwap", std::make_unique<VwapAlgorithm>()); !r) {
        return fail(r.error());
    }
    if (auto r = reg.register_algorithm("pov", std::make_unique<ParticipationAlgorithm>()); !r) {
        return fail(r.error());
    }
    if (auto r = reg.register_algorithm("iceberg", std::make_unique<IcebergAlgorithm>()); !r) {
        return fail(r.error());
    }
    if (auto r =
            reg.register_algorithm("adaptive_limit", std::make_unique<AdaptiveLimitAlgorithm>());
        !r) {
        return fail(r.error());
    }
    return reg;
}

}  // namespace ptl::algo
