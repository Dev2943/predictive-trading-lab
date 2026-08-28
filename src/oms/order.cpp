#include "ptl/oms/order.hpp"

#include <cmath>

#include "ptl/oms/fill.hpp"

namespace ptl::oms {
namespace {

[[nodiscard]] Error bad(std::string message) {
    return make_error(ErrorCode::ValidationFailed, std::move(message));
}

[[nodiscard]] bool sane_price(Price p) noexcept {
    return is_finite(p.get()) && p.get() > 0.0;
}

}  // namespace

std::string_view to_string(OrderType t) noexcept {
    switch (t) {
        case OrderType::Market:
            return "market";
        case OrderType::Limit:
            return "limit";
        case OrderType::Stop:
            return "stop";
        case OrderType::StopLimit:
            return "stop_limit";
    }
    return "unknown";
}

std::string_view to_string(TimeInForce t) noexcept {
    switch (t) {
        case TimeInForce::Day:
            return "day";
        case TimeInForce::ImmediateOrCancel:
            return "ioc";
        case TimeInForce::FillOrKill:
            return "fok";
        case TimeInForce::GoodTillCancel:
            return "gtc";
    }
    return "unknown";
}

Result<Order> Order::make(OrderId id, InstrumentId instrument, Side side, Qty quantity,
                          OrderType type, std::optional<Price> limit, std::optional<Price> stop,
                          LifecycleTimes times, TimeInForce tif, OrderId parent) {
    if (id == kNoOrder) return fail(bad("order has no id"));
    if (instrument == kInvalidInstrument) return fail(bad("order has no instrument"));
    if (!is_finite(quantity.get()) || quantity.get() <= 0.0) {
        return fail(bad("order quantity must be positive and finite"));
    }
    if (!is_set(times.decision_time)) {
        // Without a decision time there is no point-in-time anchor: nothing
        // downstream could check that the fill came after the decision, and
        // implementation shortfall would have no benchmark.
        return fail(bad("order has no decision_time"));
    }
    if (const auto v = validate_chain(times); v.has_value()) {
        return fail(bad("order timestamp chain is invalid: " + v->describe()));
    }

    // Order TYPE is the authority; price engagement is a consequence of it, not
    // an independent fact. Enforcing the agreement here means the two cannot
    // drift apart later.
    const bool needs_limit = type == OrderType::Limit || type == OrderType::StopLimit;
    const bool needs_stop = type == OrderType::Stop || type == OrderType::StopLimit;

    if (needs_limit && (!limit.has_value() || !sane_price(*limit))) {
        return fail(bad(std::string{to_string(type)} + " order needs a positive limit price"));
    }
    if (!needs_limit && limit.has_value()) {
        return fail(bad(std::string{to_string(type)} + " order must not carry a limit price"));
    }
    if (needs_stop && (!stop.has_value() || !sane_price(*stop))) {
        return fail(bad(std::string{to_string(type)} + " order needs a positive stop price"));
    }
    if (!needs_stop && stop.has_value()) {
        return fail(bad(std::string{to_string(type)} + " order must not carry a stop price"));
    }

    Order o;
    o.id_ = id;
    o.parent_ = parent;
    o.instrument_ = instrument;
    o.side_ = side;
    o.quantity_ = quantity;
    o.type_ = type;
    o.tif_ = tif;
    o.limit_price_ = limit;
    o.stop_price_ = stop;
    o.times_ = times;
    return o;
}

Result<Order> Order::market(OrderId id, InstrumentId instrument, Side side, Qty quantity,
                            LifecycleTimes times, TimeInForce tif, OrderId parent) {
    return make(id, instrument, side, quantity, OrderType::Market, std::nullopt, std::nullopt,
                times, tif, parent);
}

Result<Order> Order::limit(OrderId id, InstrumentId instrument, Side side, Qty quantity,
                           Price limit_price, LifecycleTimes times, TimeInForce tif,
                           OrderId parent) {
    return make(id, instrument, side, quantity, OrderType::Limit, limit_price, std::nullopt, times,
                tif, parent);
}

Result<Order> Order::stop(OrderId id, InstrumentId instrument, Side side, Qty quantity,
                          Price stop_price, LifecycleTimes times, TimeInForce tif, OrderId parent) {
    return make(id, instrument, side, quantity, OrderType::Stop, std::nullopt, stop_price, times,
                tif, parent);
}

Result<Order> Order::stop_limit(OrderId id, InstrumentId instrument, Side side, Qty quantity,
                                Price stop_price, Price limit_price, LifecycleTimes times,
                                TimeInForce tif, OrderId parent) {
    return make(id, instrument, side, quantity, OrderType::StopLimit, limit_price, stop_price,
                times, tif, parent);
}

Order Order::with_arrival_price(Price p) const {
    Order copy = *this;
    copy.arrival_price_ = p;
    return copy;
}

Result<Order> Order::with_arrival(Timestamp arrival) const {
    Order copy = *this;
    copy.times_.arrival_time = arrival;
    // Re-check rather than trust. This is where the no-same-bar rule lands on
    // the order itself: an arrival at or before the decision that produced it
    // is refused here, not discovered later in the fill.
    if (const auto v = validate_chain(copy.times_); v.has_value()) {
        return fail(bad("arrival stamp violates the chain: " + v->describe()));
    }
    return copy;
}

bool Order::is_marketable_against(Price touch) const noexcept {
    switch (type_) {
        case OrderType::Market:
        case OrderType::Stop:
            return true;  // once triggered, a stop is a market order
        case OrderType::Limit:
        case OrderType::StopLimit:
            if (!limit_price_.has_value()) return false;
            // No NaN anywhere in this comparison, by construction (ADR-0002),
            // so the two branches are genuine complements.
            return side_ == Side::Buy ? touch <= *limit_price_ : touch >= *limit_price_;
    }
    return false;
}

std::string_view to_string(Liquidity l) noexcept {
    return l == Liquidity::Maker ? "maker" : "taker";
}

Bps Fill::slippage_bps() const noexcept {
    if (!is_finite(arrival_price_.get()) || arrival_price_.get() <= 0.0) return Bps{0.0};
    // Signed so POSITIVE ALWAYS MEANS COST, for either side. A buy filling
    // above arrival and a sell filling below both hurt; one convention spares
    // every downstream aggregate a special case.
    const double raw = (price_.get() / arrival_price_.get() - 1.0) * 1e4;
    return Bps{raw * static_cast<double>(sign_of(side_))};
}

Notional Fill::cash_delta() const noexcept {
    // A buy consumes cash; a sell produces it. Costs always consume.
    const double gross = price_.get() * quantity_.get() * static_cast<double>(sign_of(side_));
    return Notional{-gross - commission_.get() - exchange_fee_.get()};
}

}  // namespace ptl::oms
