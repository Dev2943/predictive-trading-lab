#include "ptl/construction/rebalance.hpp"

#include <algorithm>
#include <cmath>
#include <sstream>

namespace ptl::construction {
namespace {

[[nodiscard]] Error bad(std::string message, std::string context = {}) {
    return make_error(ErrorCode::ValidationFailed, std::move(message), std::move(context));
}

}  // namespace

std::string_view to_string(RebalanceMode m) noexcept {
    return m == RebalanceMode::Full ? "full" : "partial";
}

Result<bool> TargetPortfolio::set(const TargetPosition& target) {
    if (target.instrument == kInvalidInstrument) {
        return fail(bad("target has no instrument"));
    }
    if (!is_finite(target.target_quantity.get())) {
        return fail(bad("target quantity is not finite"));
    }
    if (target.reference_price.get() <= 0.0 || !is_finite(target.reference_price.get())) {
        return fail(bad("target needs a positive reference price"));
    }
    if (targets_.contains(index_of(target.instrument))) {
        // Two targets for one name means the caller has a bug. Keeping the last
        // silently would hide it and produce a book nobody intended.
        return fail(
            bad("duplicate target for instrument", std::to_string(index_of(target.instrument))));
    }
    targets_.emplace(index_of(target.instrument), target);
    return true;
}

const TargetPosition* TargetPortfolio::find(InstrumentId instrument) const noexcept {
    const auto it = targets_.find(index_of(instrument));
    return it == targets_.end() ? nullptr : &it->second;
}

Notional TargetPortfolio::gross_notional() const noexcept {
    double total = 0.0;
    for (const auto& [key, t] : targets_) {
        total += std::abs(t.target_quantity.get() * t.reference_price.get());
    }
    return Notional{total};
}

Notional TargetPortfolio::net_notional() const noexcept {
    double total = 0.0;
    for (const auto& [key, t] : targets_) {
        total += t.target_quantity.get() * t.reference_price.get();
    }
    return Notional{total};
}

std::size_t RebalancePlan::actionable() const noexcept {
    return static_cast<std::size_t>(std::count_if(
        trades.begin(), trades.end(), [](const RebalanceTrade& t) { return !t.skipped; }));
}

std::string RebalancePlan::describe() const {
    std::ostringstream ss;
    ss.precision(2);
    ss << std::fixed << "rebalance at " << to_iso8601(as_of) << ": " << actionable() << " trades, "
       << skipped << " skipped, turnover " << gross_turnover.get() << " (" << turnover_ratio
       << "x equity), estimated cost " << estimated_cost.get();
    return ss.str();
}

Result<RebalancePlan> RebalanceEngine::plan(const TargetPortfolio& targets,
                                            const portfolio::Portfolio& pf) const {
    RebalancePlan out;
    out.as_of = targets.as_of();

    const double equity = pf.equity().get();
    if (!is_finite(equity) || equity <= 0.0) {
        return fail(bad("cannot rebalance a portfolio with non-positive equity"));
    }

    // Union of held and targeted instruments, in instrument order.
    //
    // BOTH SIDES MATTER. Iterating only the targets would leave a position
    // open forever once its instrument stopped being targeted -- the classic
    // way a book accumulates orphans nobody intended to hold.
    std::map<std::uint32_t, bool> universe;
    for (const auto& [key, t] : targets.targets()) universe[key] = true;
    for (const auto& [key, pos] : pf.positions()) {
        if (!pos.is_flat()) universe[key] = true;
    }

    double gross_turnover = 0.0;

    for (const auto& [key, present] : universe) {
        const auto instrument = static_cast<InstrumentId>(key);
        const auto* target = targets.find(instrument);
        const auto* position = pf.position(instrument);

        RebalanceTrade trade;
        trade.instrument = instrument;
        trade.current_quantity = position != nullptr ? position->quantity() : Qty{0.0};
        trade.target_quantity = target != nullptr ? target->target_quantity : Qty{0.0};

        // Price: the target's reference when it has one, otherwise the
        // portfolio's mark. An untargeted position still needs a price to size
        // its own liquidation.
        if (target != nullptr) {
            trade.reference_price = target->reference_price;
        } else if (const auto mark = pf.mark_price(instrument); mark.has_value()) {
            trade.reference_price = *mark;
        } else {
            trade.skipped = true;
            trade.skip_reason = "no reference price available";
            ++out.skipped;
            out.trades.push_back(trade);
            continue;
        }

        // THE DELTA. target - current, never the target itself: emitting orders
        // sized to the target would double a position already half built.
        const double delta = trade.target_quantity.get() - trade.current_quantity.get();
        trade.delta_quantity = Qty{delta};
        trade.side = delta >= 0.0 ? Side::Buy : Side::Sell;
        trade.notional = Notional{std::abs(delta) * trade.reference_price.get()};
        trade.drift = trade.notional.get() / equity;

        if (delta == 0.0) {
            trade.skipped = true;
            trade.skip_reason = "already at target";
            ++out.skipped;
            out.trades.push_back(trade);
            continue;
        }

        if (cfg_.mode == RebalanceMode::Partial && trade.drift < cfg_.drift_threshold) {
            // Trading toward a barely-moved target pays the spread repeatedly
            // for no expected return.
            trade.skipped = true;
            trade.skip_reason = "drift " + std::to_string(trade.drift) + " below threshold";
            ++out.skipped;
            out.trades.push_back(trade);
            continue;
        }

        if (trade.notional.get() < cfg_.min_trade_notional.get()) {
            // The commission floor makes a tiny trade unprofitable however good
            // the signal.
            trade.skipped = true;
            trade.skip_reason = "below the minimum trade size";
            ++out.skipped;
            out.trades.push_back(trade);
            continue;
        }

        // Round toward zero so rounding never increases exposure.
        if (cfg_.lot_size > 0.0) {
            const double rounded =
                std::trunc(trade.delta_quantity.get() / cfg_.lot_size) * cfg_.lot_size;
            if (rounded == 0.0) {
                trade.skipped = true;
                trade.skip_reason = "rounds to zero shares";
                ++out.skipped;
                out.trades.push_back(trade);
                continue;
            }
            trade.delta_quantity = Qty{rounded};
            trade.notional = Notional{std::abs(rounded) * trade.reference_price.get()};
        }

        gross_turnover += trade.notional.get();
        out.trades.push_back(trade);
    }

    out.gross_turnover = Notional{gross_turnover};
    out.turnover_ratio = gross_turnover / equity;
    out.estimated_cost = Notional{gross_turnover * cfg_.estimated_cost_bps.get() * 1e-4};

    if (cfg_.max_turnover_per_rebalance > 0.0 &&
        out.turnover_ratio > cfg_.max_turnover_per_rebalance) {
        // A pathological signal flip would otherwise trade the entire book in
        // one pass, paying enormous costs on a single model update.
        return fail(bad("rebalance would turn over " + std::to_string(out.turnover_ratio) +
                        "x equity, limit is " + std::to_string(cfg_.max_turnover_per_rebalance)));
    }
    return out;
}

Result<std::vector<oms::Order>> RebalanceEngine::to_orders(
    const RebalancePlan& plan, Timestamp decision_time,
    const std::function<oms::OrderId()>& next_id) const {
    if (!is_set(decision_time)) return fail(bad("order generation needs a decision time"));
    if (next_id == nullptr) return fail(bad("order generation needs an id source"));

    std::vector<oms::Order> orders;
    orders.reserve(plan.actionable());

    for (const auto& trade : plan.trades) {
        if (trade.skipped) continue;

        LifecycleTimes times;
        times.decision_time = decision_time;

        const Qty quantity{std::abs(trade.delta_quantity.get())};
        const double direction = trade.side == Side::Buy ? 1.0 : -1.0;

        Result<oms::Order> order = fail(bad("unhandled order type"));
        switch (cfg_.order_type) {
            case oms::OrderType::Market:
                order = oms::Order::market(next_id(), trade.instrument, trade.side, quantity, times,
                                           cfg_.time_in_force);
                break;
            case oms::OrderType::Limit: {
                // Offset AWAY from the touch: a buy limit below the reference,
                // a sell limit above. Offsetting the other way would cross the
                // spread and make the limit a market order in disguise.
                const Price limit = apply_bps(trade.reference_price, cfg_.limit_offset_bps,
                                              -static_cast<int>(direction));
                order = oms::Order::limit(next_id(), trade.instrument, trade.side, quantity, limit,
                                          times, cfg_.time_in_force);
                break;
            }
            case oms::OrderType::Stop: {
                const Price stop = apply_bps(trade.reference_price, cfg_.limit_offset_bps,
                                             static_cast<int>(direction));
                order = oms::Order::stop(next_id(), trade.instrument, trade.side, quantity, stop,
                                         times, cfg_.time_in_force);
                break;
            }
            case oms::OrderType::StopLimit: {
                const Price stop = apply_bps(trade.reference_price, cfg_.limit_offset_bps,
                                             static_cast<int>(direction));
                const Price limit =
                    apply_bps(stop, cfg_.limit_offset_bps, static_cast<int>(direction));
                order = oms::Order::stop_limit(next_id(), trade.instrument, trade.side, quantity,
                                               stop, limit, times, cfg_.time_in_force);
                break;
            }
        }

        if (!order) return fail(order.error());
        orders.push_back(order->with_arrival_price(trade.reference_price));
    }
    return orders;
}

}  // namespace ptl::construction
