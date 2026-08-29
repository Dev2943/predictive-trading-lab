#include "ptl/algo/executor.hpp"

#include <algorithm>
#include <cmath>

namespace ptl::algo {
namespace {

[[nodiscard]] Error bad(std::string message, std::string context = {}) {
    return make_error(ErrorCode::ValidationFailed, std::move(message), std::move(context));
}

void hash_bytes(std::uint64_t& h, const void* data, std::size_t len) {
    const auto* p = static_cast<const unsigned char*>(data);
    for (std::size_t i = 0; i < len; ++i) {
        h ^= static_cast<std::uint64_t>(p[i]);
        h *= 0x100000001b3ULL;
    }
}

}  // namespace

ExecutionResult ExecutionPlan::result() const {
    ExecutionResult r;
    r.state = state;
    r.progress = progress;
    r.started = started;
    r.finished = finished;
    r.arrival_price = arrival_price;
    return r;
}

Executor::Executor(std::unique_ptr<IExecutionAlgorithm> algorithm)
    : algorithm_(std::move(algorithm)) {}

Result<bool> Executor::submit(const ExecutionRequest& request, oms::OrderId parent_id,
                              Price arrival_price) {
    if (algorithm_ == nullptr) return fail(bad("executor has no algorithm"));
    if (parent_id == oms::kNoOrder) return fail(bad("execution needs a parent order id"));
    if (plans_.contains(oms::value_of(parent_id))) {
        return fail(
            bad("duplicate execution for parent order", std::to_string(oms::value_of(parent_id))));
    }
    if (auto ok = request.validate(); !ok) return fail(ok.error());

    auto schedule = algorithm_->plan(request);
    if (!schedule) return fail(schedule.error());

    ExecutionPlan plan;
    plan.parent_id = parent_id;
    plan.request = request;
    plan.schedule = std::move(*schedule);
    plan.progress.remaining = request.parent.quantity();
    plan.state = ExecutionState::Pending;
    plan.started = request.window_begin;
    plan.arrival_price = arrival_price;

    plans_.emplace(oms::value_of(parent_id), std::move(plan));
    ++stats_.executions_started;
    return true;
}

Result<oms::OrderId> Executor::emit_child(ExecutionPlan& plan, const ChildOrder& child,
                                          Timestamp now, engine::OrderSink& sink) {
    if (!child.valid()) return fail(bad("algorithm produced an invalid child order"));

    LifecycleTimes times;
    // The child's decision instant is NOW, not the parent's. A child decided at
    // the parent's timestamp would claim to have been decided before the market
    // data that motivated it existed.
    times.decision_time = now;

    const oms::OrderId child_id = sink.next_order_id();

    Result<oms::Order> order = fail(bad("unhandled child order type"));
    switch (child.type) {
        case oms::OrderType::Market:
            order = oms::Order::market(child_id, child.instrument, child.side, child.quantity,
                                       times, child.time_in_force, plan.parent_id);
            break;
        case oms::OrderType::Limit:
            order =
                oms::Order::limit(child_id, child.instrument, child.side, child.quantity,
                                  *child.limit_price, times, child.time_in_force, plan.parent_id);
            break;
        case oms::OrderType::Stop:
            order = oms::Order::stop(child_id, child.instrument, child.side, child.quantity,
                                     *child.stop_price, times, child.time_in_force, plan.parent_id);
            break;
        case oms::OrderType::StopLimit:
            order = oms::Order::stop_limit(child_id, child.instrument, child.side, child.quantity,
                                           *child.stop_price, *child.limit_price, times,
                                           child.time_in_force, plan.parent_id);
            break;
    }
    if (!order) return fail(order.error());

    // THE ONLY ROUTE TO THE VENUE. OrderSink::submit runs the Phase 3 risk gate
    // on every child; the executor holds no broker reference.
    const oms::Order priced = order->with_arrival_price(plan.arrival_price);
    auto submitted = sink.submit(priced);
    if (!submitted) {
        // A risk rejection is COUNTED, not fatal. The execution continues and
        // will try again on the next event -- a limit that risk refuses now may
        // be acceptable once the book has moved.
        ++stats_.child_orders_rejected;
        return fail(submitted.error());
    }

    plan.children.push_back(child_id);
    plan.working_child = child_id;
    child_to_parent_[oms::value_of(child_id)] = oms::value_of(plan.parent_id);
    ++plan.progress.slices_released;
    ++stats_.child_orders_emitted;

    // Determinism hash over what was actually sent.
    const std::int64_t ns = now.time_since_epoch().count();
    hash_bytes(emitted_hash_, &ns, sizeof(ns));
    const std::uint32_t inst = index_of(child.instrument);
    hash_bytes(emitted_hash_, &inst, sizeof(inst));
    const double qty = child.quantity.get();
    hash_bytes(emitted_hash_, &qty, sizeof(qty));
    const auto type = static_cast<std::uint8_t>(child.type);
    hash_bytes(emitted_hash_, &type, sizeof(type));
    if (child.limit_price.has_value()) {
        const double px = child.limit_price->get();
        hash_bytes(emitted_hash_, &px, sizeof(px));
    }
    return child_id;
}

Result<std::size_t> Executor::on_market(InstrumentId instrument, const ExecutionContext& ctx,
                                        engine::OrderSink& sink) {
    if (algorithm_ == nullptr) return fail(bad("executor has no algorithm"));

    std::size_t emitted = 0;
    // std::map iteration: parents are visited in id order, so two runs emit
    // children in the same sequence.
    for (auto& [key, plan] : plans_) {
        if (is_terminal(plan.state)) continue;
        if (plan.request->parent.instrument() != instrument) continue;

        // Completion is checked BEFORE releasing anything, so a fill that
        // completed the parent on the previous event cannot produce one more
        // child here.
        if (plan.progress.complete(plan.request->parent.quantity())) {
            plan.state = ExecutionState::Completed;
            plan.finished = ctx.now;
            ++stats_.executions_completed;
            continue;
        }

        // Only one child rests at a time. Sending another while one is working
        // would double the exposure the algorithm intended.
        if (plan.working_child.has_value()) continue;

        const auto child = algorithm_->next_child(*plan.request, plan.schedule, plan.progress, ctx);
        if (!child.has_value()) continue;

        auto sent = emit_child(plan, *child, ctx.now, sink);
        if (!sent) continue;  // counted as rejected; try again next event

        plan.state = ExecutionState::Working;
        ++emitted;
    }
    return emitted;
}

Result<bool> Executor::on_fill(const oms::Fill& fill) {
    // Attribute by the fill's own parent id when it carries one, falling back
    // to the child map. Both routes exist because a venue adapter may or may
    // not preserve the parent link.
    std::uint64_t parent_key = oms::value_of(fill.parent_id());
    if (parent_key == 0) {
        const auto it = child_to_parent_.find(oms::value_of(fill.order_id()));
        if (it == child_to_parent_.end()) {
            return fail(bad("fill does not belong to any known execution",
                            std::to_string(oms::value_of(fill.order_id()))));
        }
        parent_key = it->second;
    }

    const auto it = plans_.find(parent_key);
    if (it == plans_.end()) {
        return fail(bad("fill references an unknown execution", std::to_string(parent_key)));
    }
    ExecutionPlan& plan = it->second;

    const double filled = fill.quantity().get();
    if (filled <= 0.0) return fail(bad("fill has no quantity"));

    plan.progress.filled = plan.progress.filled + Qty{filled};
    plan.progress.remaining =
        Qty{std::max(0.0, plan.request->parent.quantity().get() - plan.progress.filled.get())};
    plan.progress.filled_notional =
        plan.progress.filled_notional + Notional{fill.price().get() * filled};
    plan.progress.costs = plan.progress.costs + fill.total_cost();
    ++plan.progress.slices_filled;

    // The resting child is released once it is done, which is what lets an
    // iceberg refresh: a new clip is shown only when the previous one filled.
    if (plan.working_child.has_value() && *plan.working_child == fill.order_id()) {
        plan.working_child.reset();
    }

    if (plan.progress.complete(plan.request->parent.quantity())) {
        plan.state = ExecutionState::Completed;
        plan.finished = fill.fill_time();
        ++stats_.executions_completed;
    }
    return true;
}

Result<bool> Executor::cancel(oms::OrderId parent_id, engine::OrderSink& sink, std::string reason) {
    const auto it = plans_.find(oms::value_of(parent_id));
    if (it == plans_.end()) return fail(bad("cancel for an unknown execution"));

    ExecutionPlan& plan = it->second;
    if (is_terminal(plan.state)) {
        return fail(
            bad("execution is already in terminal state " + std::string{to_string(plan.state)}));
    }

    // Cancel the resting child FIRST, then mark the execution terminal. The
    // other order would leave a live child belonging to an execution that no
    // longer exists.
    if (plan.working_child.has_value()) {
        (void)sink.cancel(*plan.working_child);
        plan.working_child.reset();
    }
    plan.state = ExecutionState::Cancelled;
    ++stats_.executions_cancelled;
    (void)reason;
    return true;
}

std::size_t Executor::expire_stale(Timestamp now, engine::OrderSink& sink) {
    std::size_t expired = 0;
    for (auto& [key, plan] : plans_) {
        if (is_terminal(plan.state)) continue;
        if (now < plan.request->window_end) continue;

        if (plan.working_child.has_value()) {
            (void)sink.cancel(*plan.working_child);
            plan.working_child.reset();
        }
        // The outstanding quantity is ABANDONED, not dumped at market. Dumping
        // converts a patient execution into the worst possible one at the worst
        // possible moment -- the end of its own window, when everyone else's
        // window is also closing.
        plan.state = ExecutionState::Expired;
        plan.finished = now;
        ++stats_.executions_expired;
        ++expired;
    }
    return expired;
}

const ExecutionPlan* Executor::find(oms::OrderId parent_id) const noexcept {
    const auto it = plans_.find(oms::value_of(parent_id));
    return it == plans_.end() ? nullptr : &it->second;
}

std::vector<oms::OrderId> Executor::active() const {
    std::vector<oms::OrderId> out;
    for (const auto& [key, plan] : plans_) {
        if (!is_terminal(plan.state)) out.push_back(plan.parent_id);
    }
    return out;
}

std::uint64_t Executor::content_hash() const noexcept {
    return emitted_hash_;
}

void Executor::reset() noexcept {
    plans_.clear();
    child_to_parent_.clear();
    stats_ = ExecutionStatistics{};
    emitted_hash_ = 0xcbf29ce484222325ULL;
}

}  // namespace ptl::algo
