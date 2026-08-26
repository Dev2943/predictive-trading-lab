#include "ptl/oms/order_manager.hpp"

#include <algorithm>

namespace ptl::oms {
namespace {

[[nodiscard]] Error bad(std::string message, std::string context = {}) {
    return make_error(ErrorCode::ValidationFailed, std::move(message), std::move(context));
}

}  // namespace

std::string_view to_string(OrderState s) noexcept {
    switch (s) {
        case OrderState::New:
            return "new";
        case OrderState::PendingNew:
            return "pending_new";
        case OrderState::Working:
            return "working";
        case OrderState::PartiallyFilled:
            return "partially_filled";
        case OrderState::Filled:
            return "filled";
        case OrderState::PendingCancel:
            return "pending_cancel";
        case OrderState::Cancelled:
            return "cancelled";
        case OrderState::Rejected:
            return "rejected";
        case OrderState::Expired:
            return "expired";
    }
    return "unknown";
}

bool is_terminal(OrderState s) noexcept {
    return s == OrderState::Filled || s == OrderState::Cancelled || s == OrderState::Rejected ||
           s == OrderState::Expired;
}

bool is_legal_transition(OrderState from, OrderState to) noexcept {
    // A terminal state is terminal. Without this, a late fill arriving after a
    // cancel would quietly reopen the order and double the position.
    if (is_terminal(from)) return false;
    if (from == to) return false;

    switch (from) {
        case OrderState::New:
            return to == OrderState::PendingNew || to == OrderState::Rejected ||
                   to == OrderState::Cancelled;
        case OrderState::PendingNew:
            return to == OrderState::Working || to == OrderState::Rejected ||
                   to == OrderState::PartiallyFilled || to == OrderState::Filled ||
                   to == OrderState::Cancelled;
        case OrderState::Working:
            return to == OrderState::PartiallyFilled || to == OrderState::Filled ||
                   to == OrderState::PendingCancel || to == OrderState::Cancelled ||
                   to == OrderState::Expired;
        case OrderState::PartiallyFilled:
            return to == OrderState::Filled || to == OrderState::PendingCancel ||
                   to == OrderState::Cancelled || to == OrderState::Expired;
        case OrderState::PendingCancel:
            // A fill can still land while a cancel is in flight -- that race is
            // real at a venue, and modelling it is what makes the simulator
            // honest about cancel latency.
            return to == OrderState::Cancelled || to == OrderState::Filled ||
                   to == OrderState::PartiallyFilled;
        default:
            return false;
    }
}

std::optional<Price> OrderRecord::average_price() const noexcept {
    if (filled_quantity.get() <= 0.0) return std::nullopt;
    return Price{filled_notional.get() / filled_quantity.get()};
}

OrderId OrderManager::next_id() noexcept {
    return static_cast<OrderId>(next_++);
}

Result<OrderId> OrderManager::submit(const Order& order) {
    const auto key = value_of(order.id());
    if (key == 0) return fail(bad("cannot submit an order with no id"));
    if (records_.contains(key)) {
        return fail(bad("duplicate order id", std::to_string(key)));
    }
    if (order.is_child() && !records_.contains(value_of(order.parent_id()))) {
        // A child whose parent is unknown would make parent-level fill
        // aggregation silently incomplete.
        return fail(bad("child order references an unknown parent",
                        std::to_string(value_of(order.parent_id()))));
    }
    // Keep the counter ahead of any externally supplied id, so a later
    // next_id() cannot collide with one already in the book.
    next_ = std::max(next_, key + 1);
    records_.emplace(key, OrderRecord{order, OrderState::New, Qty{}, Notional{}, Notional{}, {}});
    return order.id();
}

Result<bool> OrderManager::transition(OrderId id, OrderState to, std::string reason) {
    const auto it = records_.find(value_of(id));
    if (it == records_.end()) return fail(bad("unknown order", std::to_string(value_of(id))));

    OrderRecord& rec = it->second;
    if (!is_legal_transition(rec.state, to)) {
        return fail(bad(std::string{"illegal order transition "} +
                            std::string{to_string(rec.state)} + " -> " + std::string{to_string(to)},
                        std::to_string(value_of(id))));
    }
    rec.state = to;
    if (!reason.empty()) rec.reject_reason = std::move(reason);
    return true;
}

Result<bool> OrderManager::apply_fill(const Fill& fill) {
    const auto it = records_.find(value_of(fill.order_id()));
    if (it == records_.end()) {
        return fail(
            bad("fill references an unknown order", std::to_string(value_of(fill.order_id()))));
    }
    OrderRecord& rec = it->second;

    if (is_terminal(rec.state)) {
        return fail(bad("fill arrived for an order in terminal state " +
                        std::string{to_string(rec.state)}));
    }
    if (fill.instrument() != rec.order.instrument() || fill.side() != rec.order.side()) {
        return fail(bad("fill does not match its order instrument or side"));
    }
    // Overfill is a simulator bug, and one that would silently inflate the
    // position. Refusing here is how it gets found.
    const double after = rec.filled_quantity.get() + fill.quantity().get();
    if (after > rec.order.quantity().get() + 1e-9) {
        return fail(bad("fill would overfill the order: " + std::to_string(after) + " of " +
                        std::to_string(rec.order.quantity().get())));
    }
    if (fill.fill_time() <= rec.order.decision_time()) {
        // The no-same-bar rule, once more at the accounting boundary.
        return fail(bad("fill time is not after the decision that produced it"));
    }

    rec.filled_quantity = Qty{after};
    rec.filled_notional =
        rec.filled_notional + Notional{fill.price().get() * fill.quantity().get()};
    rec.total_cost = rec.total_cost + fill.total_cost();

    const OrderState next = rec.complete() ? OrderState::Filled : OrderState::PartiallyFilled;
    if (rec.state != next) {
        if (!is_legal_transition(rec.state, next)) {
            return fail(bad("fill implies an illegal transition from " +
                            std::string{to_string(rec.state)}));
        }
        rec.state = next;
    }
    return true;
}

Result<OrderId> OrderManager::cancel_replace(OrderId original, const Order& replacement) {
    const auto it = records_.find(value_of(original));
    if (it == records_.end()) return fail(bad("unknown order for cancel/replace"));
    if (is_terminal(it->second.state)) {
        return fail(bad("cannot replace an order in terminal state " +
                        std::string{to_string(it->second.state)}));
    }
    if (replacement.instrument() != it->second.order.instrument() ||
        replacement.side() != it->second.order.side()) {
        return fail(bad("replacement must keep the same instrument and side"));
    }

    // Cancel FIRST, then submit. Doing it the other way round leaves a window
    // in which both orders are live, which at a venue is a double position.
    if (auto c = transition(original, OrderState::Cancelled, "replaced"); !c) {
        return fail(c.error());
    }
    return submit(replacement);
}

const OrderRecord* OrderManager::find(OrderId id) const noexcept {
    const auto it = records_.find(value_of(id));
    return it == records_.end() ? nullptr : &it->second;
}

std::vector<OrderId> OrderManager::working() const {
    std::vector<OrderId> out;
    for (const auto& [key, rec] : records_) {
        if (!is_terminal(rec.state)) out.push_back(static_cast<OrderId>(key));
    }
    return out;
}

std::vector<OrderId> OrderManager::children_of(OrderId parent) const {
    std::vector<OrderId> out;
    for (const auto& [key, rec] : records_) {
        if (rec.order.parent_id() == parent) out.push_back(static_cast<OrderId>(key));
    }
    return out;
}

Qty OrderManager::exposure_of(InstrumentId instrument) const noexcept {
    // Signed remaining quantity across live orders: what the position would
    // become if every working order filled. Risk checks need this rather than
    // the current position, or a burst of orders each individually inside the
    // limit could collectively breach it.
    double total = 0.0;
    for (const auto& [key, rec] : records_) {
        if (rec.order.instrument() != instrument || is_terminal(rec.state)) continue;
        total += rec.remaining().get() * static_cast<double>(sign_of(rec.order.side()));
    }
    return Qty{total};
}

void OrderManager::reset() noexcept {
    records_.clear();
    next_ = 1;
}

}  // namespace ptl::oms
