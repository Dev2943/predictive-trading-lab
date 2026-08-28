#include "ptl/sizing/sizer.hpp"

#include <algorithm>
#include <cmath>
#include <sstream>

namespace ptl::sizing {
namespace {

[[nodiscard]] Error bad(std::string message, std::string context = {}) {
    return make_error(ErrorCode::ValidationFailed, std::move(message), std::move(context));
}

}  // namespace

std::string_view to_string(SizingMethod m) noexcept {
    switch (m) {
        case SizingMethod::FixedShares:
            return "fixed_shares";
        case SizingMethod::FixedDollar:
            return "fixed_dollar";
        case SizingMethod::PercentCapital:
            return "percent_capital";
        case SizingMethod::VolatilityTarget:
            return "volatility_target";
        case SizingMethod::RiskParity:
            return "risk_parity";
        case SizingMethod::Kelly:
            return "kelly";
        case SizingMethod::FractionalKelly:
            return "fractional_kelly";
    }
    return "unknown";
}

std::string SizingDecision::describe() const {
    std::ostringstream ss;
    ss.precision(6);
    ss << std::fixed << "target=" << target_position.get() << " shares (" << target_notional.get()
       << ") weight " << raw_weight << " -> " << final_weight;
    if (capped) ss << "  [capped: " << cap_reason << "]";
    return ss.str();
}

double PositionSizer::kelly_weight(double edge, double variance) noexcept {
    // f* = edge / variance. Undefined without dispersion: a riskless positive
    // edge implies infinite leverage, which is a modelling artefact rather than
    // an opportunity, so zero is the safe answer.
    if (!is_finite(edge) || !is_finite(variance) || variance <= 0.0) return 0.0;
    const double f = edge / variance;
    return is_finite(f) ? f : 0.0;
}

double PositionSizer::risk_parity_weight(double volatility, double target_volatility) noexcept {
    // Inverse volatility: each position contributes roughly equal risk rather
    // than equal capital. A zero-volatility instrument would take infinite
    // weight, so it takes none.
    if (!is_finite(volatility) || volatility <= 0.0) return 0.0;
    const double w = target_volatility / volatility;
    return is_finite(w) ? w : 0.0;
}

double PositionSizer::raw_weight_for(const signal::Signal& sig, const SizingContext& ctx) const {
    const double equity = ctx.equity.get();
    const double price = ctx.reference_price.get();
    if (equity <= 0.0 || price <= 0.0) return 0.0;

    switch (cfg_.method) {
        case SizingMethod::FixedShares:
            return cfg_.fixed_shares * price / equity;

        case SizingMethod::FixedDollar:
            return cfg_.fixed_dollar.get() / equity;

        case SizingMethod::PercentCapital:
            return cfg_.percent_capital;

        case SizingMethod::VolatilityTarget: {
            // Scale so each position contributes the target volatility. A
            // quiet instrument gets more capital, a violent one less, which is
            // what stops a single name dominating portfolio risk.
            if (ctx.volatility <= 0.0) return 0.0;
            const double annual_vol = ctx.volatility * std::sqrt(cfg_.annualization_periods);
            if (annual_vol <= 0.0) return 0.0;
            return cfg_.target_volatility / annual_vol;
        }

        case SizingMethod::RiskParity: {
            const double annual_vol = ctx.volatility * std::sqrt(cfg_.annualization_periods);
            return risk_parity_weight(annual_vol, cfg_.target_volatility);
        }

        case SizingMethod::Kelly:
        case SizingMethod::FractionalKelly: {
            // Edge is the NET edge -- after costs. Sizing on gross edge would
            // allocate capital to trades that lose money once they are paid for.
            const double annual_vol = ctx.volatility * std::sqrt(cfg_.annualization_periods);
            const double variance = annual_vol * annual_vol;
            const double full = kelly_weight(sig.net_edge(), variance);
            const double scaled =
                cfg_.method == SizingMethod::Kelly ? full : full * cfg_.kelly_fraction;
            // Capped regardless of the estimated edge. Full Kelly is optimal
            // only if the edge is exact; with an overestimated edge it is
            // spectacularly destructive.
            return std::clamp(scaled, -cfg_.kelly_cap, cfg_.kelly_cap);
        }
    }
    return 0.0;
}

Result<SizingDecision> PositionSizer::size(const signal::Signal& sig,
                                           const SizingContext& ctx) const {
    if (!is_set(ctx.now)) return fail(bad("sizing context has no timestamp"));
    if (!is_finite(ctx.equity.get())) return fail(bad("equity is not finite"));
    if (ctx.reference_price.get() <= 0.0 || !is_finite(ctx.reference_price.get())) {
        return fail(bad("sizing needs a positive reference price"));
    }

    SizingDecision out;

    // A flat or unprofitable signal targets zero. Note this is a TARGET of
    // zero, not "leave the position alone": the rebalance engine will close an
    // existing position, which is the correct response to a signal that no
    // longer justifies holding it.
    if (sig.is_flat() || !sig.is_actionable()) {
        out.target_position = Qty{0.0};
        out.target_notional = Notional{0.0};
        return out;
    }

    const double equity = ctx.equity.get();
    if (equity <= 0.0) {
        out.cap_reason = "no equity";
        out.capped = true;
        return out;
    }

    double weight = raw_weight_for(sig, ctx);
    if (!is_finite(weight)) weight = 0.0;
    out.raw_weight = weight;

    const auto apply_cap = [&out](double limit, std::string_view reason, double& w) {
        if (std::abs(w) > limit) {
            w = std::copysign(limit, w);
            out.capped = true;
            out.cap_reason = std::string{reason};
        }
    };

    // --- limits, most specific first -----------------------------------------
    // Ordered so the recorded reason names the tightest binding constraint
    // rather than whichever happened to be checked first.
    apply_cap(cfg_.limits.max_position_weight, "max position weight", weight);

    if (cfg_.limits.max_position_notional.get() > 0.0) {
        const double limit = cfg_.limits.max_position_notional.get() / equity;
        apply_cap(limit, "max position notional", weight);
    }

    // Sector exposure counts what is ALREADY held in the sector, so a series of
    // individually-compliant positions cannot collectively breach the limit.
    if (ctx.sector >= 0 && cfg_.limits.max_sector_weight > 0.0) {
        const double used = std::abs(ctx.sector_exposure.get()) / equity;
        const double headroom = std::max(0.0, cfg_.limits.max_sector_weight - used);
        apply_cap(headroom, "max sector weight", weight);
    }

    // Gross leverage headroom, likewise net of what the book already carries.
    if (cfg_.limits.max_gross_leverage > 0.0) {
        const double used = std::abs(ctx.existing_gross.get()) / equity;
        const double headroom = std::max(0.0, cfg_.limits.max_gross_leverage - used);
        apply_cap(headroom, "max gross leverage", weight);
    }

    if (cfg_.limits.max_net_leverage > 0.0) {
        const double signed_weight = weight * static_cast<double>(sign_of(sig.direction()));
        const double projected_net = std::abs(ctx.existing_net.get() / equity + signed_weight);
        if (projected_net > cfg_.limits.max_net_leverage) {
            const double headroom = std::max(
                0.0, cfg_.limits.max_net_leverage - std::abs(ctx.existing_net.get() / equity));
            apply_cap(headroom, "max net leverage", weight);
        }
    }

    out.final_weight = weight;

    const double notional = weight * equity;
    double shares = notional / ctx.reference_price.get();
    shares *= static_cast<double>(sign_of(sig.direction()));

    // Round toward zero, so rounding can never increase exposure past a limit
    // that was just applied.
    if (cfg_.lot_size > 0.0) {
        shares = std::trunc(shares / cfg_.lot_size) * cfg_.lot_size;
    }

    if (!is_finite(shares)) return fail(bad("computed share count is not finite"));

    out.target_position = Qty{shares};
    out.target_notional = Notional{shares * ctx.reference_price.get()};
    return out;
}

}  // namespace ptl::sizing
