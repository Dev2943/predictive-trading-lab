#pragma once

/// \file order_manager.hpp
/// Order lifecycle: identity, parent/child, and a validated state machine.
///
/// The OMS owns mutable lifecycle state so that Order itself can stay
/// immutable. It also enforces legal transitions: a filled order cannot go back
/// to working, a cancelled one cannot fill. Illegal transitions are errors
/// rather than silent no-ops, because a partial-fill accounting bug that
/// manifests as a state anomaly is far cheaper to find here than three
/// subsystems downstream in the P&L.

#include <cstdint>
#include <map>
#include <optional>
#include <string_view>
#include <vector>

#include "ptl/core/result.hpp"
#include "ptl/oms/fill.hpp"
#include "ptl/oms/order.hpp"

namespace ptl::oms {

enum class OrderState : std::uint8_t {
    New,              ///< created, not yet sent
    PendingNew,       ///< sent, awaiting acknowledgement
    Working,          ///< live at the venue
    PartiallyFilled,  ///< live, some quantity done
    Filled,           ///< terminal
    PendingCancel,    ///< cancel requested
    Cancelled,        ///< terminal
    Rejected,         ///< terminal
    Expired,          ///< terminal
};

[[nodiscard]] std::string_view to_string(OrderState) noexcept;
[[nodiscard]] bool is_terminal(OrderState) noexcept;

/// Whether `from -> to` is legal. Exposed so the transition table itself can be
/// tested exhaustively rather than only along the paths a scenario happens to
/// exercise.
[[nodiscard]] bool is_legal_transition(OrderState from, OrderState to) noexcept;

struct OrderRecord {
    Order order;
    OrderState state{OrderState::New};
    Qty filled_quantity{};
    Notional filled_notional{};
    Notional total_cost{};
    std::string reject_reason;

    [[nodiscard]] Qty remaining() const noexcept { return order.quantity() - filled_quantity; }
    [[nodiscard]] bool complete() const noexcept {
        return filled_quantity.get() >= order.quantity().get();
    }
    /// Quantity-weighted average fill price, or nullopt if nothing filled.
    [[nodiscard]] std::optional<Price> average_price() const noexcept;
};

class OrderManager {
public:
    /// Monotonic ids from a per-manager counter, never a global. Two runs
    /// therefore assign identical ids, which is what lets order records be
    /// compared across a replay-versus-live diff.
    [[nodiscard]] OrderId next_id() noexcept;

    [[nodiscard]] Result<OrderId> submit(const Order& order);
    [[nodiscard]] Result<bool> transition(OrderId id, OrderState to, std::string reason = {});
    [[nodiscard]] Result<bool> apply_fill(const Fill& fill);

    /// Cancel/replace as an atomic pair: the old order is cancelled and the new
    /// one submitted, sharing a parent. Doing it as two independent calls
    /// leaves a window where both are live, which at the venue is a double
    /// position.
    [[nodiscard]] Result<OrderId> cancel_replace(OrderId original, const Order& replacement);

    [[nodiscard]] const OrderRecord* find(OrderId id) const noexcept;
    [[nodiscard]] std::vector<OrderId> working() const;
    [[nodiscard]] std::vector<OrderId> children_of(OrderId parent) const;

    [[nodiscard]] std::size_t size() const noexcept { return records_.size(); }
    [[nodiscard]] Qty exposure_of(InstrumentId instrument) const noexcept;

    void reset() noexcept;

private:
    // std::map, not unordered_map: iteration order is part of the determinism
    // contract, and OrderId is dense and monotonic so ordered lookup is cheap.
    std::map<std::uint64_t, OrderRecord> records_;
    std::uint64_t next_ = 1;
};

}  // namespace ptl::oms
