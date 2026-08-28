#pragma once

/// \file order.hpp
/// Immutable orders with validating factories.
///
/// An Order is a VALUE: once constructed it never changes. Lifecycle state --
/// how much has filled, whether it is working or cancelled -- lives in the
/// OrderManager's record, not in the order itself. That split keeps the thing
/// we journal, hash and compare across a replay-versus-live diff genuinely
/// immutable, so two runs producing the same orders produce byte-identical
/// order records.
///
/// Per ADR-0002 there is NO NaN sentinel. An absent limit price is
/// std::optional, because NaN is unordered and `!(a < b)` does not imply
/// `a >= b` -- which would make two marketability checks a reader considers
/// equivalent take opposite branches.

#include <cstdint>
#include <optional>
#include <string_view>

#include "ptl/core/result.hpp"
#include "ptl/core/time.hpp"
#include "ptl/core/types.hpp"

namespace ptl::oms {

enum class OrderId : std::uint64_t {};
inline constexpr OrderId kNoOrder{0};

[[nodiscard]] constexpr std::uint64_t value_of(OrderId id) noexcept {
    return static_cast<std::uint64_t>(id);
}

enum class OrderType : std::uint8_t { Market, Limit, Stop, StopLimit };
enum class TimeInForce : std::uint8_t { Day, ImmediateOrCancel, GoodTillCancel };

[[nodiscard]] std::string_view to_string(OrderType) noexcept;
[[nodiscard]] std::string_view to_string(TimeInForce) noexcept;

class Order {
public:
    /// Cross the spread immediately on arrival.
    [[nodiscard]] static Result<Order> market(OrderId id, InstrumentId instrument, Side side,
                                              Qty quantity, LifecycleTimes times,
                                              TimeInForce tif = TimeInForce::Day,
                                              OrderId parent = kNoOrder);

    /// Rest at `limit` or better.
    [[nodiscard]] static Result<Order> limit(OrderId id, InstrumentId instrument, Side side,
                                             Qty quantity, Price limit_price, LifecycleTimes times,
                                             TimeInForce tif = TimeInForce::Day,
                                             OrderId parent = kNoOrder);

    /// Becomes a market order once `stop` trades through.
    [[nodiscard]] static Result<Order> stop(OrderId id, InstrumentId instrument, Side side,
                                            Qty quantity, Price stop_price, LifecycleTimes times,
                                            TimeInForce tif = TimeInForce::Day,
                                            OrderId parent = kNoOrder);

    [[nodiscard]] static Result<Order> stop_limit(OrderId id, InstrumentId instrument, Side side,
                                                  Qty quantity, Price stop_price, Price limit_price,
                                                  LifecycleTimes times,
                                                  TimeInForce tif = TimeInForce::Day,
                                                  OrderId parent = kNoOrder);

    [[nodiscard]] OrderId id() const noexcept { return id_; }
    [[nodiscard]] OrderId parent_id() const noexcept { return parent_; }
    [[nodiscard]] bool is_child() const noexcept { return parent_ != kNoOrder; }
    [[nodiscard]] InstrumentId instrument() const noexcept { return instrument_; }
    [[nodiscard]] Side side() const noexcept { return side_; }
    [[nodiscard]] Qty quantity() const noexcept { return quantity_; }
    [[nodiscard]] OrderType type() const noexcept { return type_; }
    [[nodiscard]] TimeInForce time_in_force() const noexcept { return tif_; }

    /// Engaged only for Limit and StopLimit. ADR-0002: absence is absence.
    [[nodiscard]] const std::optional<Price>& limit_price() const noexcept { return limit_price_; }
    [[nodiscard]] const std::optional<Price>& stop_price() const noexcept { return stop_price_; }

    [[nodiscard]] const LifecycleTimes& times() const noexcept { return times_; }
    [[nodiscard]] Timestamp decision_time() const noexcept { return times_.decision_time; }

    /// The decision-time reference price, and the benchmark implementation
    /// shortfall is measured against. Set by the signal layer when the order is
    /// created, never by the venue.
    [[nodiscard]] Price arrival_price() const noexcept { return arrival_price_; }

    /// Returns a copy carrying an arrival price. Immutability preserved: the
    /// original is untouched.
    [[nodiscard]] Order with_arrival_price(Price p) const;

    /// Returns a copy with the arrival stage stamped. The chain invariant is
    /// re-checked, so an order cannot acquire an arrival at or before its own
    /// decision -- the no-same-bar rule, enforced on the order itself.
    [[nodiscard]] Result<Order> with_arrival(Timestamp arrival) const;

    /// Signed quantity: positive for a buy, negative for a sell.
    [[nodiscard]] Qty signed_quantity() const noexcept {
        return Qty{quantity_.get() * static_cast<double>(sign_of(side_))};
    }

    [[nodiscard]] bool is_marketable_against(Price touch) const noexcept;

private:
    Order() = default;
    [[nodiscard]] static Result<Order> make(OrderId, InstrumentId, Side, Qty, OrderType,
                                            std::optional<Price> limit, std::optional<Price> stop,
                                            LifecycleTimes, TimeInForce, OrderId parent);

    OrderId id_{kNoOrder};
    OrderId parent_{kNoOrder};
    InstrumentId instrument_{kInvalidInstrument};
    Side side_{Side::Buy};
    Qty quantity_{};
    OrderType type_{OrderType::Market};
    TimeInForce tif_{TimeInForce::Day};
    std::optional<Price> limit_price_{};
    std::optional<Price> stop_price_{};
    LifecycleTimes times_{};
    Price arrival_price_{};
};

}  // namespace ptl::oms
