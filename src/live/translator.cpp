#include "ptl/live/translator.hpp"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <limits>
#include <sstream>

namespace ptl::live {
namespace {

[[nodiscard]] Error bad(std::string message, std::string context = {}) {
    return make_error(ErrorCode::ValidationFailed, std::move(message), std::move(context));
}

[[nodiscard]] std::string json_escape(std::string_view in) {
    std::string out;
    out.reserve(in.size() + 8);
    for (const char c : in) {
        switch (c) {
            case '"':
                out += "\\\"";
                break;
            case '\\':
                out += "\\\\";
                break;
            case '\n':
                out += "\\n";
                break;
            default:
                out += c;
        }
    }
    return out;
}

/// Prices on the wire are round-trip exact. A price rounded to two decimals in
/// the request is a DIFFERENT ORDER from the one risk approved, and the
/// difference is invisible in the log.
[[nodiscard]] std::string wire_number(double v) {
    std::ostringstream ss;
    ss << std::setprecision(std::numeric_limits<double>::max_digits10) << v;
    return ss.str();
}

}  // namespace

std::string OrderUpdate::describe() const {
    std::ostringstream ss;
    switch (kind) {
        case Kind::Accepted:
            ss << "accepted";
            break;
        case Kind::Rejected:
            ss << "rejected";
            break;
        case Kind::Cancelled:
            ss << "cancelled";
            break;
        case Kind::Replaced:
            ss << "replaced";
            break;
        case Kind::PartiallyFilled:
            ss << "partially_filled";
            break;
        case Kind::Filled:
            ss << "filled";
            break;
        case Kind::Ignored:
            ss << "ignored";
            break;
    }
    ss << " order=" << oms::value_of(order_id);
    if (is_fill()) ss << " qty=" << filled_quantity.get() << " px=" << fill_price.get();
    if (!reason.empty()) ss << " (" << reason << ')';
    return ss.str();
}

// ---------------------------------------------------------------------------
// AlpacaOrderTranslator
// ---------------------------------------------------------------------------

Result<std::string_view> AlpacaOrderTranslator::type_string(oms::OrderType type) noexcept {
    switch (type) {
        case oms::OrderType::Market:
            return std::string_view{"market"};
        case oms::OrderType::Limit:
            return std::string_view{"limit"};
        case oms::OrderType::Stop:
            return std::string_view{"stop"};
        case oms::OrderType::StopLimit:
            return std::string_view{"stop_limit"};
    }
    return fail(bad("unknown order type"));
}

Result<std::string_view> AlpacaOrderTranslator::tif_string(oms::TimeInForce tif) noexcept {
    switch (tif) {
        case oms::TimeInForce::Day:
            return std::string_view{"day"};
        case oms::TimeInForce::GoodTillCancel:
            return std::string_view{"gtc"};
        case oms::TimeInForce::ImmediateOrCancel:
            return std::string_view{"ioc"};
        case oms::TimeInForce::FillOrKill:
            return std::string_view{"fok"};
    }
    return fail(bad("unknown time in force"));
}

std::string AlpacaOrderTranslator::client_order_id(oms::OrderId id) const {
    // DETERMINISTIC. The same order must always produce the same id, or a
    // reconnect cannot match its own outstanding requests against the venue's
    // open-order list.
    return config_.client_id_prefix + std::to_string(oms::value_of(id));
}

Result<bool> AlpacaOrderTranslator::supports(const oms::Order& order) const {
    // Checked BEFORE encoding, so an unsupported combination is refused with a
    // reason rather than quietly downgraded into an order the strategy never
    // asked for.
    if (order.quantity().get() <= 0.0) {
        return fail(bad("order quantity must be positive"));
    }
    if (!config_.allow_fractional) {
        const double q = order.quantity().get();
        if (std::abs(q - std::round(q)) > 1e-9) {
            return fail(
                bad("this venue configuration does not accept fractional "
                    "quantities",
                    wire_number(q)));
        }
    }

    auto type = type_string(order.type());
    if (!type) return fail(type.error());
    auto tif = tif_string(order.time_in_force());
    if (!tif) return fail(tif.error());

    // Alpaca accepts IOC and FOK on limit orders only. Downgrading to Day here
    // would leave a resting order the strategy expected to expire instantly.
    const bool immediate = order.time_in_force() == oms::TimeInForce::ImmediateOrCancel ||
                           order.time_in_force() == oms::TimeInForce::FillOrKill;
    if (immediate && order.type() == oms::OrderType::Market) {
        return fail(
            bad("this venue does not accept IOC or FOK on a market order; send "
                "a marketable limit instead"));
    }

    if (order.type() == oms::OrderType::Limit || order.type() == oms::OrderType::StopLimit) {
        if (!order.limit_price().has_value()) {
            return fail(bad("a limit order needs a limit price"));
        }
        if (order.limit_price()->get() <= 0.0) {
            return fail(bad("limit price must be positive"));
        }
    }
    if (order.type() == oms::OrderType::Stop || order.type() == oms::OrderType::StopLimit) {
        if (!order.stop_price().has_value()) {
            return fail(bad("a stop order needs a stop price"));
        }
        if (order.stop_price()->get() <= 0.0) {
            return fail(bad("stop price must be positive"));
        }
    }
    return true;
}

Result<EncodedRequest> AlpacaOrderTranslator::encode_new(const oms::Order& order) const {
    if (auto ok = supports(order); !ok) return fail(ok.error());

    auto type = type_string(order.type());
    auto tif = tif_string(order.time_in_force());

    // The venue speaks symbols; we speak instrument ids. Without a table the
    // translation cannot be done, and guessing a symbol would send an order for
    // the wrong security.
    std::string symbol;
    if (instruments_ != nullptr) {
        symbol = std::string{instruments_->symbol(order.instrument())};
    }
    if (symbol.empty()) {
        return fail(bad("no symbol is known for instrument " +
                        std::to_string(index_of(order.instrument())) + "; refusing to guess"));
    }

    EncodedRequest request;
    request.order_id = order.id();
    request.client_order_id = client_order_id(order.id());

    std::ostringstream ss;
    ss << "{\"symbol\": \"" << json_escape(symbol) << "\", \"qty\": \""
       << wire_number(order.quantity().get()) << "\", \"side\": \""
       << (order.side() == Side::Buy ? "buy" : "sell") << "\", \"type\": \"" << *type
       << "\", \"time_in_force\": \"" << *tif << "\", \"client_order_id\": \""
       << json_escape(request.client_order_id) << '"';

    if (order.limit_price().has_value()) {
        ss << ", \"limit_price\": \"" << wire_number(order.limit_price()->get()) << '"';
    }
    if (order.stop_price().has_value()) {
        ss << ", \"stop_price\": \"" << wire_number(order.stop_price()->get()) << '"';
    }
    if (config_.extended_hours) ss << ", \"extended_hours\": true";
    ss << '}';

    request.payload = ss.str();
    return request;
}

Result<EncodedRequest> AlpacaOrderTranslator::encode_cancel(oms::OrderId id,
                                                            std::string_view venue_order_id) const {
    if (venue_order_id.empty()) {
        // Cancelling without the venue's id would require matching on our own,
        // which the venue does not accept for cancels. Refusing beats sending a
        // request that silently does nothing.
        return fail(bad("cannot cancel before the venue has assigned an order id",
                        std::to_string(oms::value_of(id))));
    }
    EncodedRequest request;
    request.order_id = id;
    request.client_order_id = client_order_id(id);
    request.payload =
        "{\"action\": \"cancel\", \"order_id\": \"" + json_escape(venue_order_id) + "\"}";
    return request;
}

Result<EncodedRequest> AlpacaOrderTranslator::encode_replace(
    const oms::Order& amended, std::string_view venue_order_id) const {
    if (venue_order_id.empty()) {
        return fail(bad("cannot replace before the venue has assigned an order id"));
    }
    if (auto ok = supports(amended); !ok) return fail(ok.error());

    // A genuine replace, not cancel-then-new: a venue that supports replace
    // keeps queue priority, and emulating it with two messages loses that
    // silently.
    EncodedRequest request;
    request.order_id = amended.id();
    request.client_order_id = client_order_id(amended.id());

    std::ostringstream ss;
    ss << "{\"action\": \"replace\", \"order_id\": \"" << json_escape(venue_order_id)
       << "\", \"qty\": \"" << wire_number(amended.quantity().get()) << '"';
    if (amended.limit_price().has_value()) {
        ss << ", \"limit_price\": \"" << wire_number(amended.limit_price()->get()) << '"';
    }
    if (amended.stop_price().has_value()) {
        ss << ", \"stop_price\": \"" << wire_number(amended.stop_price()->get()) << '"';
    }
    ss << '}';
    request.payload = ss.str();
    return request;
}

// ---------------------------------------------------------------------------
// LiveOrderListener
// ---------------------------------------------------------------------------

Result<bool> LiveOrderListener::track(std::string client_order_id, oms::OrderId id) {
    if (client_order_id.empty()) return fail(bad("client order id cannot be empty"));
    if (id == oms::kNoOrder) return fail(bad("cannot track an invalid order id"));

    const auto existing = by_client_id_.find(client_order_id);
    if (existing != by_client_id_.end()) {
        if (existing->second == id) return true;  // idempotent re-registration
        // Two orders under one client id means a reply could be attributed to
        // the wrong one, which would apply a fill to a position we do not hold.
        return fail(bad("client order id is already tracking a different order", client_order_id));
    }
    by_client_id_.emplace(std::move(client_order_id), id);
    return true;
}

void LiveOrderListener::forget(std::string_view client_order_id) noexcept {
    const auto it = by_client_id_.find(client_order_id);
    if (it != by_client_id_.end()) by_client_id_.erase(it);
}

std::optional<oms::OrderId> LiveOrderListener::resolve(std::string_view client_order_id) const {
    const auto it = by_client_id_.find(client_order_id);
    if (it == by_client_id_.end()) return std::nullopt;
    return it->second;
}

void LiveOrderListener::clear() noexcept {
    by_client_id_.clear();
}

Result<OrderUpdate> LiveOrderListener::interpret(const BrokerMessage& message) const {
    OrderUpdate update;
    update.venue_order_id = message.venue_order_id;
    update.venue_fill_id = message.venue_fill_id;
    update.venue_time = message.venue_time;
    update.reason = message.text;

    switch (message.kind) {
        case MessageKind::OrderAccepted:
            update.kind = OrderUpdate::Kind::Accepted;
            break;
        case MessageKind::OrderRejected:
            update.kind = OrderUpdate::Kind::Rejected;
            break;
        case MessageKind::OrderCancelled:
            update.kind = OrderUpdate::Kind::Cancelled;
            break;
        case MessageKind::OrderReplaced:
            update.kind = OrderUpdate::Kind::Replaced;
            break;
        case MessageKind::PartialFill:
            update.kind = OrderUpdate::Kind::PartiallyFilled;
            break;
        case MessageKind::Fill:
            update.kind = OrderUpdate::Kind::Filled;
            break;
        default:
            // A quote or account update arriving here is normal, not an error.
            update.kind = OrderUpdate::Kind::Ignored;
            return update;
    }

    const auto resolved = resolve(message.client_order_id);
    if (!resolved.has_value()) {
        // An update for an order we are not tracking. This happens legitimately
        // after a restart, and it is REPORTED rather than guessed at: applying
        // a fill to an order we cannot identify would corrupt the portfolio.
        return fail(bad(
            "venue update references an untracked order",
            message.client_order_id.empty() ? message.venue_order_id : message.client_order_id));
    }
    update.order_id = *resolved;

    if (update.is_fill()) {
        if (!(message.quantity.get() > 0.0)) {
            return fail(bad("venue reported a fill with non-positive quantity"));
        }
        if (!(message.price.get() > 0.0)) {
            return fail(bad("venue reported a fill with non-positive price"));
        }
        update.filled_quantity = message.quantity;
        update.fill_price = message.price;
        update.commission = message.commission;
        update.exchange_fee = message.exchange_fee;
    }
    return update;
}

std::vector<std::pair<std::string, std::uint64_t>> LiveOrderListener::snapshot() const {
    std::vector<std::pair<std::string, std::uint64_t>> out;
    out.reserve(by_client_id_.size());
    // std::map: ordered, so a persisted snapshot is byte-identical between runs
    // and can be diffed after an incident.
    for (const auto& [client_id, order_id] : by_client_id_) {
        out.emplace_back(client_id, oms::value_of(order_id));
    }
    return out;
}

Result<bool> LiveOrderListener::restore(
    const std::vector<std::pair<std::string, std::uint64_t>>& entries) {
    by_client_id_.clear();
    for (const auto& [client_id, order_id] : entries) {
        if (auto ok = track(client_id, static_cast<oms::OrderId>(order_id)); !ok) {
            return ok;
        }
    }
    return true;
}

}  // namespace ptl::live
