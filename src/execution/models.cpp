#include "ptl/execution/models.hpp"

#include <algorithm>
#include <cmath>

namespace ptl::execution {

Notional StandardCostModel::commission(Qty filled, Price price) const {
    if (filled.get() <= 0.0) return Notional{0.0};
    // Charged on the FILLED quantity, never the requested quantity. Charging on
    // the request is the classic double-count that makes partial fills look
    // more expensive than they are.
    const double per_share = cfg_.commission_per_share * filled.get();
    const double c = std::max(per_share, cfg_.minimum_commission);
    const double fee = cfg_.taker_fee_bps.get() * 1e-4 * price.get() * filled.get();
    return Notional{(c + fee) * cfg_.cost_multiplier};
}

Bps StandardCostModel::half_spread(const MarketState& state) const {
    if (!state.has_quote) {
        // Bar-only mode: no quote exists, so the spread is SYNTHESISED and the
        // run must say so. This is a documented degraded fallback, not a
        // measurement.
        return Bps{cfg_.fallback_half_spread_bps.get() * cfg_.cost_multiplier};
    }
    const double mid = state.mid().get();
    if (!is_finite(mid) || mid <= 0.0) return Bps{0.0};
    const double half = (state.ask.get() - state.bid.get()) * 0.5;
    return Bps{(half / mid) * 1e4 * cfg_.cost_multiplier};
}

Bps StandardCostModel::market_impact(Qty quantity, const MarketState& state) const {
    const double q = std::abs(quantity.get());
    const double v = state.interval_volume.get();
    // Zero interval volume yields zero impact rather than infinity. Zero-volume
    // minutes are real; an inf here would propagate into a fill price and then
    // into the P&L.
    if (q <= 0.0 || v <= 0.0) return Bps{0.0};

    const double participation = q / v;
    const double sigma = state.intraday_volatility > 0.0 ? state.intraday_volatility : 0.01;
    // Intraday square-root law: c * sigma * (Q/V)^alpha, expressed in bps.
    const double impact =
        cfg_.impact_coefficient * sigma * std::pow(participation, cfg_.impact_exponent) * 1e4;
    if (!is_finite(impact)) return Bps{0.0};
    return Bps{impact * cfg_.cost_multiplier};
}

Bps StandardCostModel::stochastic_slippage(DeterministicRng& rng) const {
    if (cfg_.stochastic_slippage_bps.get() <= 0.0) return Bps{0.0};
    // Half-normal: symmetric draw, absolute value taken, so slippage is ALWAYS
    // a cost. A signed draw would let random noise pay the strategy roughly
    // half the time, which flatters every result for free.
    const double draw = std::abs(rng.normal(0.0, cfg_.stochastic_slippage_bps.get()));
    return Bps{draw * cfg_.cost_multiplier};
}

namespace {

[[nodiscard]] Duration jittered(Duration base, double sigma, DeterministicRng& rng) {
    if (sigma <= 0.0) return base;
    // Lognormal: latency is positive and right-skewed. A normal multiplier
    // could go negative, which would mean an order arriving before it was sent.
    const double factor = std::exp(rng.normal(0.0, sigma));
    const auto scaled = static_cast<std::int64_t>(static_cast<double>(base.count()) * factor);
    return Duration{std::max<std::int64_t>(0, scaled)};
}

}  // namespace

Duration StandardLatencyModel::decision_to_arrival(DeterministicRng& rng) const {
    return jittered(cfg_.strategy_compute, cfg_.jitter_sigma, rng) +
           jittered(cfg_.order_transmission, cfg_.jitter_sigma, rng) +
           jittered(cfg_.exchange_processing, cfg_.jitter_sigma, rng);
}

Duration StandardLatencyModel::arrival_to_ack(DeterministicRng& rng) const {
    return jittered(cfg_.acknowledgement, cfg_.jitter_sigma, rng);
}

}  // namespace ptl::execution
