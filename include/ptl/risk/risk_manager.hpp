#pragma once

/// \file risk_manager.hpp
/// Pre-trade risk gate.
///
/// Sits between signal generation and the OMS. Every order passes through it,
/// and it has NO side effects on the portfolio -- it inspects and decides. That
/// separation is what makes risk decisions reproducible: given the same
/// portfolio and the same order, the answer is always the same.
///
/// Rejections are COUNTED AND REPORTED, never silently dropped. A suppressed
/// order that vanishes without trace makes a backtest quietly diverge from
/// paper trading, and the divergence is invisible precisely because nothing
/// recorded it.

#include <chrono>
#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "ptl/core/types.hpp"
#include "ptl/oms/order.hpp"
#include "ptl/oms/order_manager.hpp"
#include "ptl/portfolio/portfolio.hpp"

namespace ptl::risk {

enum class RejectCode : std::uint8_t {
    Approved,
    MaxPositionExceeded,
    MaxOrderNotional,
    MaxGrossExposure,
    MaxNetExposure,
    MaxLeverage,
    InsufficientBuyingPower,
    ShortingDisabled,
    PriceCollar,
    StaleData,
    DrawdownKillSwitch,
    ConcentrationLimit,
    TurnoverBudget,
    NoMarkAvailable,
};

[[nodiscard]] std::string_view to_string(RejectCode) noexcept;

struct RiskLimits {
    Notional max_order_notional{250'000.0};
    Notional max_position_notional{500'000.0};
    double max_gross_leverage = 2.0;
    double max_net_leverage = 1.0;
    /// Fraction of equity any single instrument may represent.
    double max_concentration = 0.25;
    /// Reject if the order price deviates more than this from the current mark.
    Bps price_collar{500.0};
    /// Data older than this cannot be traded on.
    Duration max_data_staleness{std::chrono::minutes{5}};
    /// Peak-to-trough drawdown that halts all new risk.
    double max_drawdown_pct = 0.25;
    /// Daily turnover budget as a multiple of equity.
    double max_daily_turnover = 1.0;
    bool allow_short = true;
};

struct RiskDecision {
    RejectCode code{RejectCode::Approved};
    std::string detail;
    /// Non-zero when the order is permitted at a reduced size rather than
    /// refused outright.
    Qty adjusted_quantity{};

    [[nodiscard]] bool approved() const noexcept { return code == RejectCode::Approved; }
    [[nodiscard]] bool resized() const noexcept {
        return approved() && adjusted_quantity.get() > 0.0;
    }
    [[nodiscard]] std::string describe() const;
};

/// Everything the gate needs that is not the order or the portfolio.
struct RiskContext {
    Timestamp now{kNoTimestamp};
    /// Age of the most recent data for this instrument.
    Duration data_age{Duration::zero()};
    /// Reference price for collar and notional checks.
    Price reference_price{};
    /// Highest equity seen so far, for the drawdown kill switch.
    Notional peak_equity{};
    /// Turnover already transacted today.
    Notional turnover_today{};
};

class RiskManager {
public:
    explicit RiskManager(RiskLimits limits = {}) : limits_(limits) {}

    /// Pure inspection: no state is mutated, so the same inputs always give the
    /// same answer.
    [[nodiscard]] RiskDecision check(const oms::Order& order, const portfolio::Portfolio& pf,
                                     const oms::OrderManager& oms, const RiskContext& ctx) const;

    /// Record an outcome for reporting. Separate from check() so that the
    /// decision itself stays side-effect free.
    void record(const RiskDecision& decision);

    [[nodiscard]] std::size_t rejection_count() const noexcept;
    [[nodiscard]] std::size_t rejection_count(RejectCode) const noexcept;
    [[nodiscard]] const std::map<std::uint8_t, std::size_t>& rejections() const noexcept {
        return rejections_;
    }
    [[nodiscard]] const RiskLimits& limits() const noexcept { return limits_; }
    void reset() noexcept { rejections_.clear(); }

private:
    RiskLimits limits_;
    std::map<std::uint8_t, std::size_t> rejections_;
};

}  // namespace ptl::risk
