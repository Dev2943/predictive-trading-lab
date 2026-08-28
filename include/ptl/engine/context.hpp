#pragma once

/// \file context.hpp
/// The read-only view a strategy is given.
///
/// Deliberately has NO method that returns future data, no non-const access to
/// the event source, and no way to construct a Fill. A strategy physically
/// cannot look ahead, cannot fabricate an execution, and cannot mutate the
/// portfolio behind the engine's back -- not by discipline, but because the
/// surface does not exist.

#include <span>

#include "ptl/core/clock.hpp"
#include "ptl/core/types.hpp"
#include "ptl/market/calendar.hpp"
#include "ptl/oms/order.hpp"
#include "ptl/oms/order_manager.hpp"
#include "ptl/portfolio/portfolio.hpp"
#include "ptl/risk/risk_manager.hpp"

namespace ptl::engine {

/// Where a strategy sends orders. The engine implements this; the strategy only
/// ever sees the interface, so it cannot reach the venue directly.
class OrderSink {
public:
    OrderSink() = default;
    virtual ~OrderSink() = default;
    OrderSink(const OrderSink&) = delete;
    OrderSink& operator=(const OrderSink&) = delete;

protected:
    OrderSink(OrderSink&&) = default;
    OrderSink& operator=(OrderSink&&) = default;

public:
    /// \returns the assigned id, or the risk rejection that stopped it.
    [[nodiscard]] virtual Result<oms::OrderId> submit(const oms::Order&) = 0;
    [[nodiscard]] virtual Result<bool> cancel(oms::OrderId) = 0;
    /// A fresh id from the engine's monotonic counter -- never a global, so two
    /// runs assign identical ids.
    [[nodiscard]] virtual oms::OrderId next_order_id() = 0;
};

class StrategyContext {
public:
    StrategyContext(const IClock& clock, const portfolio::Portfolio& pf,
                    const oms::OrderManager& oms, const risk::RiskLimits& limits,
                    const market::Calendar* calendar) noexcept
        : clock_(&clock), pf_(&pf), oms_(&oms), limits_(&limits), calendar_(calendar) {}

    /// Simulated time in a replay, wall time when live. A strategy must never
    /// call system_clock::now() itself; this is the only clock it can see, and
    /// it is what makes the same code run in both modes.
    [[nodiscard]] Timestamp now() const noexcept { return clock_->now(); }

    [[nodiscard]] const portfolio::Portfolio& portfolio() const noexcept { return *pf_; }
    [[nodiscard]] const oms::OrderManager& orders() const noexcept { return *oms_; }
    [[nodiscard]] const risk::RiskLimits& limits() const noexcept { return *limits_; }
    [[nodiscard]] const market::Calendar* calendar() const noexcept { return calendar_; }

    [[nodiscard]] Qty position_of(InstrumentId id) const noexcept {
        const auto* p = pf_->position(id);
        return p == nullptr ? Qty{} : p->quantity();
    }

private:
    const IClock* clock_;
    const portfolio::Portfolio* pf_;
    const oms::OrderManager* oms_;
    const risk::RiskLimits* limits_;
    const market::Calendar* calendar_;
};

}  // namespace ptl::engine
