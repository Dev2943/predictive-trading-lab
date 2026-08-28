#pragma once

/// \file rebalance.hpp
/// Target portfolio, rebalancing, and delta order generation.
///
/// THE DELTA RULE. A target is a desired END STATE, never an increment. The
/// rebalance engine computes `target - current` and emits orders for the
/// difference only. Emitting orders sized to the target itself would double a
/// position that was already half built -- the single most expensive arithmetic
/// error available in this layer.
///
/// A DRIFT THRESHOLD sits in front of that. Trading every bar toward a target
/// that has barely moved pays the spread repeatedly for no expected return, so
/// a position is left alone until it has drifted far enough to be worth fixing.

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "ptl/core/result.hpp"
#include "ptl/core/types.hpp"
#include "ptl/oms/order.hpp"
#include "ptl/portfolio/portfolio.hpp"

namespace ptl::construction {

/// One instrument's desired end state.
struct TargetPosition {
    InstrumentId instrument{kInvalidInstrument};
    Qty target_quantity{};
    Price reference_price{};
    double target_weight = 0.0;
    /// Expected net edge that justified this target, carried through so an
    /// order can be traced back to the reasoning behind it.
    double net_edge = 0.0;
};

/// The desired book at one instant.
class TargetPortfolio {
public:
    explicit TargetPortfolio(Timestamp as_of) noexcept : as_of_(as_of) {}

    /// Duplicate instruments are refused: two targets for one name means the
    /// caller has a bug, and silently keeping the last would hide it.
    [[nodiscard]] Result<bool> set(const TargetPosition&);

    [[nodiscard]] Timestamp as_of() const noexcept { return as_of_; }
    [[nodiscard]] std::size_t size() const noexcept { return targets_.size(); }
    [[nodiscard]] const TargetPosition* find(InstrumentId) const noexcept;

    /// Ordered by instrument index, so iteration is reproducible.
    [[nodiscard]] const std::map<std::uint32_t, TargetPosition>& targets() const noexcept {
        return targets_;
    }

    [[nodiscard]] Notional gross_notional() const noexcept;
    [[nodiscard]] Notional net_notional() const noexcept;

private:
    Timestamp as_of_{kNoTimestamp};
    std::map<std::uint32_t, TargetPosition> targets_;
};

enum class RebalanceMode : std::uint8_t {
    /// Trade every instrument whose drift exceeds the threshold.
    Partial,
    /// Trade every instrument whose target differs at all.
    Full,
};

[[nodiscard]] std::string_view to_string(RebalanceMode) noexcept;

struct RebalanceConfig {
    RebalanceMode mode{RebalanceMode::Partial};

    /// Fraction of equity a position must drift before it is worth trading.
    /// Trading toward a barely-moved target pays the spread for nothing.
    double drift_threshold = 0.005;

    /// Orders below this notional are dropped: the commission floor makes a
    /// tiny trade unprofitable however good the signal.
    Notional min_trade_notional{500.0};

    /// Round share counts to this lot.
    double lot_size = 1.0;

    /// Estimated round-trip cost, for the turnover estimate.
    Bps estimated_cost_bps{3.0};

    /// Refuse to rebalance if the whole plan would turn over more than this
    /// multiple of equity in one pass. A pathological signal flip would
    /// otherwise trade the entire book at once.
    double max_turnover_per_rebalance = 0.50;

    oms::OrderType order_type{oms::OrderType::Market};
    /// Limit offset from the reference price, for non-market orders.
    Bps limit_offset_bps{5.0};
    oms::TimeInForce time_in_force{oms::TimeInForce::Day};
};

/// One instrument's planned trade.
struct RebalanceTrade {
    InstrumentId instrument{kInvalidInstrument};
    Qty current_quantity{};
    Qty target_quantity{};
    /// target - current. THE DELTA, never the target.
    Qty delta_quantity{};
    Side side{Side::Buy};
    Price reference_price{};
    Notional notional{};
    double drift = 0.0;
    bool skipped = false;
    std::string skip_reason;
};

struct RebalancePlan {
    Timestamp as_of{kNoTimestamp};
    std::vector<RebalanceTrade> trades;
    Notional gross_turnover{};
    Notional estimated_cost{};
    double turnover_ratio = 0.0;
    std::size_t skipped = 0;

    [[nodiscard]] std::size_t actionable() const noexcept;
    [[nodiscard]] std::string describe() const;
};

class RebalanceEngine {
public:
    explicit RebalanceEngine(RebalanceConfig cfg = {}) : cfg_(cfg) {}

    /// Compare a target book against the actual one and plan the difference.
    ///
    /// Pure: it reads the portfolio and mutates nothing, so the same inputs
    /// always produce the same plan.
    [[nodiscard]] Result<RebalancePlan> plan(const TargetPortfolio&,
                                             const portfolio::Portfolio&) const;

    /// Convert a plan into orders.
    ///
    /// \param next_id supplies ids from the OMS counter, so ids stay monotonic
    ///        and two runs assign identical ones.
    [[nodiscard]] Result<std::vector<oms::Order>> to_orders(
        const RebalancePlan&, Timestamp decision_time,
        const std::function<oms::OrderId()>& next_id) const;

    [[nodiscard]] const RebalanceConfig& config() const noexcept { return cfg_; }

private:
    RebalanceConfig cfg_;
};

}  // namespace ptl::construction
