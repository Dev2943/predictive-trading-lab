#pragma once

/// \file fill.hpp
/// An execution report.
///
/// A Fill can only be constructed by the BrokerSimulator. The constructor is
/// private and the simulator is its sole friend, so no other module -- not a
/// strategy, not the OMS, not a test helper reaching for a shortcut -- can
/// fabricate an execution. That single restriction is what makes the P&L
/// trustworthy: every dollar in the equity curve traces to one place.

#include <cstdint>
#include <string_view>

#include "ptl/core/time.hpp"
#include "ptl/core/types.hpp"
#include "ptl/oms/order.hpp"

namespace ptl::execution {
class BrokerSimulator;
}

namespace ptl::oms {

enum class Liquidity : std::uint8_t { Taker, Maker };

[[nodiscard]] std::string_view to_string(Liquidity) noexcept;

class Fill {
public:
    [[nodiscard]] OrderId order_id() const noexcept { return order_id_; }
    [[nodiscard]] OrderId parent_id() const noexcept { return parent_id_; }
    [[nodiscard]] InstrumentId instrument() const noexcept { return instrument_; }
    [[nodiscard]] Side side() const noexcept { return side_; }
    [[nodiscard]] Price price() const noexcept { return price_; }
    [[nodiscard]] Qty quantity() const noexcept { return quantity_; }
    [[nodiscard]] Notional commission() const noexcept { return commission_; }
    [[nodiscard]] Notional exchange_fee() const noexcept { return exchange_fee_; }
    [[nodiscard]] Notional total_cost() const noexcept { return commission_ + exchange_fee_; }
    [[nodiscard]] Liquidity liquidity() const noexcept { return liquidity_; }

    [[nodiscard]] const LifecycleTimes& times() const noexcept { return times_; }
    [[nodiscard]] Timestamp fill_time() const noexcept { return times_.fill_time; }

    /// Decision-time reference. Implementation shortfall is measured from here.
    [[nodiscard]] Price arrival_price() const noexcept { return arrival_price_; }

    /// Signed so that a buy is positive. Slippage is ALWAYS a cost when
    /// positive, for either side: a buy filling above arrival and a sell
    /// filling below both hurt, and a single sign convention keeps every
    /// aggregate downstream from needing a special case.
    [[nodiscard]] Bps slippage_bps() const noexcept;

    /// Signed cash impact including costs. Negative for a buy.
    [[nodiscard]] Notional cash_delta() const noexcept;

    /// Signed share delta: positive for a buy.
    [[nodiscard]] Qty signed_quantity() const noexcept {
        return Qty{quantity_.get() * static_cast<double>(sign_of(side_))};
    }

private:
    friend class execution::BrokerSimulator;
    Fill() = default;

    OrderId order_id_{kNoOrder};
    OrderId parent_id_{kNoOrder};
    InstrumentId instrument_{kInvalidInstrument};
    Side side_{Side::Buy};
    Price price_{};
    Qty quantity_{};
    Notional commission_{};
    Notional exchange_fee_{};
    Liquidity liquidity_{Liquidity::Taker};
    LifecycleTimes times_{};
    Price arrival_price_{};
};

}  // namespace ptl::oms
