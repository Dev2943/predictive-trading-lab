#include "ptl/paper/broker.hpp"

#include <algorithm>
#include <cmath>
#include <sstream>

namespace ptl::paper {

std::string_view to_string(AckStatus s) noexcept {
    switch (s) {
        case AckStatus::Accepted:
            return "accepted";
        case AckStatus::Rejected:
            return "rejected";
        case AckStatus::Pending:
            return "pending";
    }
    return "unknown";
}

std::string_view to_string(RejectReason r) noexcept {
    switch (r) {
        case RejectReason::None:
            return "none";
        case RejectReason::NotConnected:
            return "not_connected";
        case RejectReason::ExceedsVenueLimit:
            return "exceeds_venue_limit";
        case RejectReason::InsufficientBuyingPower:
            return "insufficient_buying_power";
        case RejectReason::MarginCall:
            return "margin_call";
        case RejectReason::ShortNotPermitted:
            return "short_not_permitted";
        case RejectReason::AccountSuspended:
            return "account_suspended";
        case RejectReason::SimulatorRejected:
            return "simulator_rejected";
    }
    return "unknown";
}

std::string BrokerStats::describe() const {
    std::ostringstream ss;
    ss << "paper broker: " << submitted << " submitted, " << accepted << " accepted, " << rejected
       << " rejected, " << cancelled << " cancelled, " << fills_routed << " fills routed\n";
    for (const auto& [reason, count] : rejections_by_reason) {
        ss << "  " << to_string(static_cast<RejectReason>(reason)) << ": " << count << '\n';
    }
    return ss.str();
}

PaperBroker::PaperBroker(const IClock& clock, execution::BrokerSimulator& simulator,
                         const PaperAccount& account, PaperBrokerConfig config) noexcept
    : clock_(&clock), simulator_(&simulator), account_(&account), config_(config) {}

Result<bool> PaperBroker::connect() {
    // A paper venue cannot fail to connect, but the state still exists: a
    // session that never exercises its connect path will exercise it for the
    // first time against a real venue.
    connected_ = true;
    return true;
}

void PaperBroker::disconnect() noexcept {
    connected_ = false;
}

RejectReason PaperBroker::screen(const oms::Order& order) const {
    if (!connected_) return RejectReason::NotConnected;

    const Price reference =
        order.limit_price().has_value() ? *order.limit_price() : order.arrival_price();
    const double notional = order.quantity().get() * reference.get();

    if (config_.max_order_notional.get() > 0.0 && is_finite(notional) &&
        notional > config_.max_order_notional.get()) {
        return RejectReason::ExceedsVenueLimit;
    }

    if (config_.enforce_buying_power) {
        // reduces_risk is FALSE here. The adapter sees an order, not a position
        // delta, and assuming an order shrinks the book would let a
        // doubling-down order through under a margin call. The conservative
        // reading is the safe one.
        if (auto ok = account_->can_accept(Notional{notional}, order.side(), false); !ok) {
            switch (account_->status()) {
                case AccountStatus::Suspended:
                    return RejectReason::AccountSuspended;
                case AccountStatus::MarginCall:
                case AccountStatus::Liquidation:
                    return RejectReason::MarginCall;
                case AccountStatus::Active:
                    break;
            }
            return RejectReason::InsufficientBuyingPower;
        }
    }
    return RejectReason::None;
}

Result<OrderAck> PaperBroker::submit(const oms::Order& order) {
    ++stats_.submitted;

    OrderAck ack;
    ack.order_id = order.id();
    ack.acknowledged_at = clock_->now() + config_.ack_latency;

    const auto record_rejection = [this, &ack](RejectReason reason) {
        ack.status = AckStatus::Rejected;
        ack.reject_reason = std::string{to_string(reason)};
        ++stats_.rejected;
        ++stats_.rejections_by_reason[static_cast<std::uint8_t>(reason)];
    };

    if (const auto reason = screen(order); reason != RejectReason::None) {
        record_rejection(reason);
        if (reason == RejectReason::NotConnected) {
            // A disconnected venue is an ERROR, not a rejection: the order
            // never reached anyone, and the caller must be able to tell that
            // apart from a venue that considered and refused it.
            return fail(make_error(ErrorCode::IoError, "cannot submit while disconnected"));
        }
        return ack;
    }

    if (config_.reject_every_n > 0 && stats_.submitted % config_.reject_every_n == 0) {
        record_rejection(RejectReason::SimulatorRejected);
        return ack;
    }

    // Accepted: hand it to the simulator, which remains the SOLE source of
    // fills. The adapter never constructs one.
    auto submitted = simulator_->submit(order);
    if (!submitted) {
        record_rejection(RejectReason::SimulatorRejected);
        ack.reject_reason = submitted.error().message;
        return ack;
    }

    ack.status = AckStatus::Accepted;
    ++stats_.accepted;
    working_[oms::value_of(order.id())] = true;
    return ack;
}

Result<bool> PaperBroker::cancel(oms::OrderId id) {
    if (!connected_) {
        return fail(make_error(ErrorCode::IoError, "cannot cancel while disconnected"));
    }
    auto cancelled = simulator_->cancel(id);
    if (!cancelled) return fail(cancelled.error());
    ++stats_.cancelled;
    working_.erase(oms::value_of(id));
    return true;
}

void PaperBroker::route(std::vector<oms::Fill> fills) {
    for (auto& fill : fills) {
        working_.erase(oms::value_of(fill.order_id()));
        pending_.push_back(std::move(fill));
    }
}

std::vector<oms::Fill> PaperBroker::poll_fills() {
    std::vector<oms::Fill> out;
    out.reserve(pending_.size());
    while (!pending_.empty()) {
        out.push_back(std::move(pending_.front()));
        pending_.pop_front();
    }
    stats_.fills_routed += out.size();
    return out;
}

std::vector<oms::OrderId> PaperBroker::working_orders() const {
    std::vector<oms::OrderId> out;
    out.reserve(working_.size());
    // std::map: ordered, so a recovery comparison is reproducible.
    for (const auto& [id, live] : working_) {
        if (live) out.push_back(static_cast<oms::OrderId>(id));
    }
    return out;
}

}  // namespace ptl::paper
