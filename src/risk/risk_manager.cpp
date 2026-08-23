#include "ptl/risk/risk_manager.hpp"

#include <algorithm>
#include <cmath>

namespace ptl::risk {

std::string_view to_string(RejectCode c) noexcept {
    switch (c) {
        case RejectCode::Approved:
            return "approved";
        case RejectCode::MaxPositionExceeded:
            return "max_position_exceeded";
        case RejectCode::MaxOrderNotional:
            return "max_order_notional";
        case RejectCode::MaxGrossExposure:
            return "max_gross_exposure";
        case RejectCode::MaxNetExposure:
            return "max_net_exposure";
        case RejectCode::MaxLeverage:
            return "max_leverage";
        case RejectCode::InsufficientBuyingPower:
            return "insufficient_buying_power";
        case RejectCode::ShortingDisabled:
            return "shorting_disabled";
        case RejectCode::PriceCollar:
            return "price_collar";
        case RejectCode::StaleData:
            return "stale_data";
        case RejectCode::DrawdownKillSwitch:
            return "drawdown_kill_switch";
        case RejectCode::ConcentrationLimit:
            return "concentration_limit";
        case RejectCode::TurnoverBudget:
            return "turnover_budget";
        case RejectCode::NoMarkAvailable:
            return "no_mark_available";
    }
    return "unknown";
}

std::string RiskDecision::describe() const {
    std::string out{to_string(code)};
    if (!detail.empty()) out += ": " + detail;
    return out;
}

namespace {

[[nodiscard]] RiskDecision reject(RejectCode code, std::string detail) {
    return RiskDecision{code, std::move(detail), Qty{}};
}

}  // namespace

RiskDecision RiskManager::check(const oms::Order& order, const portfolio::Portfolio& pf,
                                const oms::OrderManager& oms, const RiskContext& ctx) const {
    // --- data freshness ------------------------------------------------------
    // Checked FIRST. Every other limit is computed against a reference price,
    // and a limit evaluated on stale data is worse than no limit at all: it
    // reports compliance it cannot support.
    if (ctx.data_age > limits_.max_data_staleness) {
        return reject(RejectCode::StaleData,
                      "market data is " + std::to_string(ctx.data_age.count() / 1'000'000'000) +
                          "s old, limit is " +
                          std::to_string(limits_.max_data_staleness.count() / 1'000'000'000) + "s");
    }
    const double ref = ctx.reference_price.get();
    if (!is_finite(ref) || ref <= 0.0) {
        return reject(RejectCode::NoMarkAvailable, "no usable reference price for this instrument");
    }

    // --- kill switch ---------------------------------------------------------
    const double equity = pf.equity().get();
    if (ctx.peak_equity.get() > 0.0 && is_finite(equity)) {
        const double dd = 1.0 - (equity / ctx.peak_equity.get());
        if (dd > limits_.max_drawdown_pct) {
            return reject(RejectCode::DrawdownKillSwitch,
                          "drawdown " + std::to_string(dd) + " exceeds " +
                              std::to_string(limits_.max_drawdown_pct));
        }
    }

    // --- shorting ------------------------------------------------------------
    const auto* pos = pf.position(order.instrument());
    const double current = pos != nullptr ? pos->quantity().get() : 0.0;
    // Working orders count toward exposure. Without this, a burst of orders
    // each individually inside the limit could collectively breach it.
    const double in_flight = oms.exposure_of(order.instrument()).get();
    const double projected = current + in_flight + order.signed_quantity().get();

    if (!limits_.allow_short && projected < -1e-9) {
        return reject(RejectCode::ShortingDisabled, "order would create a short position");
    }

    // --- order size ----------------------------------------------------------
    const double order_notional = std::abs(order.quantity().get() * ref);
    if (order_notional > limits_.max_order_notional.get()) {
        return reject(RejectCode::MaxOrderNotional,
                      std::to_string(order_notional) + " exceeds " +
                          std::to_string(limits_.max_order_notional.get()));
    }

    // --- position size -------------------------------------------------------
    const double projected_notional = std::abs(projected * ref);
    if (projected_notional > limits_.max_position_notional.get()) {
        return reject(RejectCode::MaxPositionExceeded,
                      "projected position " + std::to_string(projected_notional) + " exceeds " +
                          std::to_string(limits_.max_position_notional.get()));
    }

    // --- concentration -------------------------------------------------------
    if (equity > 0.0 && projected_notional / equity > limits_.max_concentration) {
        return reject(RejectCode::ConcentrationLimit,
                      "single-instrument weight " + std::to_string(projected_notional / equity) +
                          " exceeds " + std::to_string(limits_.max_concentration));
    }

    // --- portfolio leverage --------------------------------------------------
    if (equity > 0.0) {
        const double projected_gross =
            pf.gross_exposure().get() + std::abs(order.quantity().get() * ref);
        if (projected_gross / equity > limits_.max_gross_leverage) {
            return reject(RejectCode::MaxGrossExposure,
                          "gross leverage " + std::to_string(projected_gross / equity) +
                              " exceeds " + std::to_string(limits_.max_gross_leverage));
        }
        const double projected_net =
            std::abs(pf.net_exposure().get() + order.signed_quantity().get() * ref);
        if (projected_net / equity > limits_.max_net_leverage) {
            return reject(RejectCode::MaxNetExposure,
                          "net leverage " + std::to_string(projected_net / equity) + " exceeds " +
                              std::to_string(limits_.max_net_leverage));
        }
    }

    // --- price collar --------------------------------------------------------
    if (order.limit_price().has_value()) {
        const double dev = std::abs(to_bps(*order.limit_price(), ctx.reference_price).get());
        if (dev > limits_.price_collar.get()) {
            return reject(RejectCode::PriceCollar, "limit price is " + std::to_string(dev) +
                                                       " bps from the mark, collar is " +
                                                       std::to_string(limits_.price_collar.get()));
        }
    }

    // --- buying power --------------------------------------------------------
    // Only a BUY consumes cash. A sale that reduces an existing long produces
    // cash, and refusing it for lack of buying power would trap the portfolio.
    if (order.side() == Side::Buy && order_notional > pf.buying_power().get()) {
        return reject(RejectCode::InsufficientBuyingPower,
                      "needs " + std::to_string(order_notional) + ", has " +
                          std::to_string(pf.buying_power().get()));
    }

    // --- turnover budget -----------------------------------------------------
    if (equity > 0.0 && limits_.max_daily_turnover > 0.0) {
        const double projected_turnover = ctx.turnover_today.get() + order_notional;
        if (projected_turnover / equity > limits_.max_daily_turnover) {
            return reject(RejectCode::TurnoverBudget,
                          "daily turnover " + std::to_string(projected_turnover / equity) +
                              "x equity exceeds " + std::to_string(limits_.max_daily_turnover));
        }
    }

    return RiskDecision{RejectCode::Approved, {}, order.quantity()};
}

void RiskManager::record(const RiskDecision& decision) {
    // Approvals are counted too: "how many orders did risk see?" is as useful a
    // question as "how many did it stop?".
    ++rejections_[static_cast<std::uint8_t>(decision.code)];
}

std::size_t RiskManager::rejection_count() const noexcept {
    std::size_t total = 0;
    for (const auto& [code, n] : rejections_) {
        if (code != static_cast<std::uint8_t>(RejectCode::Approved)) total += n;
    }
    return total;
}

std::size_t RiskManager::rejection_count(RejectCode c) const noexcept {
    const auto it = rejections_.find(static_cast<std::uint8_t>(c));
    return it == rejections_.end() ? 0 : it->second;
}

}  // namespace ptl::risk
