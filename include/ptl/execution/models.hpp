#pragma once

/// \file models.hpp
/// Cost, latency and fill assumptions -- every one of them explicit.
///
/// These are ENGINEERING APPROXIMATIONS, not calibrated institutional models.
/// Every parameter is configurable and every result must be reported alongside
/// a cost-multiplier sweep (0.5x / 1x / 2x / 3x), because a backtest whose
/// conclusion flips at 2x costs has not found alpha, it has found a cost
/// assumption.

#include <chrono>
#include <cstdint>

#include "ptl/core/rng.hpp"
#include "ptl/core/types.hpp"
#include "ptl/market/quote.hpp"

namespace ptl::execution {

struct MarketState {
    Price bid{};
    Price ask{};
    Qty bid_size{};
    Qty ask_size{};
    Volume interval_volume{};
    Volume average_daily_volume{};
    double intraday_volatility = 0.0;
    bool has_quote = false;

    [[nodiscard]] Price mid() const noexcept { return Price{(bid.get() + ask.get()) * 0.5}; }
    /// The price a marketable order of `side` must cross to.
    [[nodiscard]] Price touch(Side side) const noexcept { return side == Side::Buy ? ask : bid; }
};

struct CostConfig {
    /// Interactive-Brokers-shaped default; configurable.
    double commission_per_share = 0.0035;
    double minimum_commission = 0.35;
    Bps taker_fee_bps{0.0};
    Bps maker_rebate_bps{0.0};

    /// Random component, applied symmetrically and always as a cost.
    Bps stochastic_slippage_bps{0.25};

    /// Square-root impact: bps = c * sigma_intraday * (Q / V_interval)^alpha.
    /// The research's intraday form. c defaults to the conservative 0.10.
    double impact_coefficient = 0.10;
    double impact_exponent = 0.5;

    /// Scales every cost at once, for the mandated sensitivity sweep.
    double cost_multiplier = 1.0;

    /// Synthesised half-spread when only bars are available and no quote exists.
    Bps fallback_half_spread_bps{1.0};
};

class ICostModel {
public:
    ICostModel() = default;
    virtual ~ICostModel() = default;
    ICostModel(const ICostModel&) = delete;
    ICostModel& operator=(const ICostModel&) = delete;

protected:
    ICostModel(ICostModel&&) = default;
    ICostModel& operator=(ICostModel&&) = default;

public:
    [[nodiscard]] virtual Notional commission(Qty filled, Price price) const = 0;
    [[nodiscard]] virtual Bps half_spread(const MarketState&) const = 0;
    [[nodiscard]] virtual Bps market_impact(Qty quantity, const MarketState&) const = 0;
    [[nodiscard]] virtual Bps stochastic_slippage(DeterministicRng&) const = 0;
};

class StandardCostModel final : public ICostModel {
public:
    explicit StandardCostModel(CostConfig cfg = {}) : cfg_(cfg) {}

    [[nodiscard]] Notional commission(Qty filled, Price price) const override;
    [[nodiscard]] Bps half_spread(const MarketState&) const override;
    [[nodiscard]] Bps market_impact(Qty quantity, const MarketState&) const override;
    [[nodiscard]] Bps stochastic_slippage(DeterministicRng&) const override;

    [[nodiscard]] const CostConfig& config() const noexcept { return cfg_; }

private:
    CostConfig cfg_;
};

// ---------------------------------------------------------------------------
// Latency
// ---------------------------------------------------------------------------

/// Five components, not one. Market-data latency is a different animal from
/// order latency: it shifts when the strategy is permitted to SEE a quote,
/// whereas the others only delay the order.
struct LatencyConfig {
    Duration market_data{std::chrono::microseconds{500}};
    Duration strategy_compute{std::chrono::microseconds{250}};
    Duration order_transmission{std::chrono::microseconds{900}};
    Duration exchange_processing{std::chrono::microseconds{100}};
    Duration acknowledgement{std::chrono::microseconds{700}};
    /// Multiplicative lognormal jitter. Zero makes latency exactly fixed, which
    /// is the right default for a first-pass deterministic backtest.
    double jitter_sigma = 0.0;
};

class ILatencyModel {
public:
    ILatencyModel() = default;
    virtual ~ILatencyModel() = default;
    ILatencyModel(const ILatencyModel&) = delete;
    ILatencyModel& operator=(const ILatencyModel&) = delete;

protected:
    ILatencyModel(ILatencyModel&&) = default;
    ILatencyModel& operator=(ILatencyModel&&) = default;

public:
    [[nodiscard]] virtual Duration decision_to_arrival(DeterministicRng&) const = 0;
    [[nodiscard]] virtual Duration arrival_to_ack(DeterministicRng&) const = 0;
    [[nodiscard]] virtual Duration market_data_latency() const = 0;
};

class StandardLatencyModel final : public ILatencyModel {
public:
    explicit StandardLatencyModel(LatencyConfig cfg = {}) : cfg_(cfg) {}

    [[nodiscard]] Duration decision_to_arrival(DeterministicRng&) const override;
    [[nodiscard]] Duration arrival_to_ack(DeterministicRng&) const override;
    [[nodiscard]] Duration market_data_latency() const override { return cfg_.market_data; }

    [[nodiscard]] const LatencyConfig& config() const noexcept { return cfg_; }

private:
    LatencyConfig cfg_;
};

// ---------------------------------------------------------------------------
// Fill model
// ---------------------------------------------------------------------------

/// How much of an order can fill, and at what price.
///
/// ⚠ THIS IS NOT QUEUE-POSITION MODELLING, and the project never claims it is.
/// Queue position cannot be established from bars or from sampled top-of-book
/// data -- it needs order-level (L3) feeds and even then the reconstruction is
/// imperfect. See ADR-0001, "Honest project claim", and ADR-0003.
///
/// What this model does instead is bound fills conservatively:
///   - a marketable order crosses the spread and pays impact;
///   - fills are capped by displayed size and by a participation rate;
///   - a non-marketable limit does NOT fill merely because a price was touched.
struct FillConfig {
    /// Maximum share of the interval's volume one order may take.
    double max_participation_rate = 0.05;

    /// Cap fills at displayed top-of-book size when a quote exists.
    bool respect_displayed_size = true;

    /// Fraction of a resting limit's quantity assumed to fill when the market
    /// trades through it. Strictly a haircut expressing "we were somewhere in a
    /// queue we cannot observe" -- deliberately pessimistic and deliberately
    /// not called a queue model.
    double passive_fill_ratio = 0.5;

    /// Require a trade-through, not merely a touch, for a passive fill. A bar
    /// whose low equals your limit says nothing about whether you traded.
    bool require_trade_through = true;
};

}  // namespace ptl::execution
