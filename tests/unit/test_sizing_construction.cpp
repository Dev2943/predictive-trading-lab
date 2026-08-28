#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cmath>

#include "ptl/construction/rebalance.hpp"
#include "ptl/execution/broker.hpp"
#include "ptl/sizing/sizer.hpp"
#include "support/ptl_catch.hpp"

using namespace ptl;
using namespace ptl::sizing;
using namespace std::chrono;

namespace {

Timestamp at(const char* iso) {
    Timestamp ts{};
    REQUIRE(parse_timestamp(iso, ts));
    return ts;
}

constexpr InstrumentId kSpy{0};
constexpr InstrumentId kQqq{1};

signal::Signal long_signal(double edge = 0.01, double confidence = 0.8) {
    auto s = signal::Signal::create(at("2024-07-02T15:00:00Z"), kSpy, signal::Direction::Long, edge,
                                    confidence, minutes{15}, 0xABCD);
    REQUIRE(s.has_value());
    return *s;
}

SizingContext context(double equity = 1'000'000.0, double vol = 0.001) {
    SizingContext ctx;
    ctx.now = at("2024-07-02T15:00:00Z");
    ctx.equity = Notional{equity};
    ctx.reference_price = Price{500.0};
    ctx.volatility = vol;
    return ctx;
}

/// Fills come only from a BrokerSimulator; this drives a real one.
oms::Fill make_fill(Side side, double qty, double price, InstrumentId instrument = kSpy) {
    static std::uint64_t next = 0;
    SimulatedClock clock{at("2024-07-02T14:00:00Z")};
    execution::CostConfig cc;
    cc.commission_per_share = 0.0;
    cc.minimum_commission = 0.0;
    cc.stochastic_slippage_bps = Bps{0.0};
    cc.impact_coefficient = 0.0;
    execution::StandardCostModel costs{cc};
    execution::StandardLatencyModel latency;
    execution::FillConfig fc;
    fc.max_participation_rate = 1.0;
    fc.respect_displayed_size = false;
    execution::BrokerSimulator broker{clock, costs, latency, DeterministicRng{1}, fc};

    LifecycleTimes times;
    times.decision_time = clock.now();
    auto order = oms::Order::market(oms::OrderId{++next}, instrument, side, Qty{qty}, times);
    REQUIRE(broker.submit(*order).has_value());

    execution::MarketState st;
    st.bid = Price{price};
    st.ask = Price{price};
    st.interval_volume = Volume{1e9};
    st.has_quote = true;
    clock.advance_by(seconds{1});
    auto fills = broker.on_market(instrument, st, clock.now());
    REQUIRE(fills.has_value());
    REQUIRE(fills->size() == 1);
    return fills->front();
}

}  // namespace

// ---------------------------------------------------------------------------
// Sizing
// ---------------------------------------------------------------------------

TEST_CASE("fixed sizing methods behave as named", "[sizing]") {
    SizingConfig cfg;
    cfg.method = SizingMethod::FixedShares;
    cfg.fixed_shares = 100.0;
    auto shares = PositionSizer{cfg}.size(long_signal(), context());
    REQUIRE(shares.has_value());
    REQUIRE(shares->target_position.get() == Catch::Approx(100.0));

    cfg.method = SizingMethod::FixedDollar;
    cfg.fixed_dollar = Notional{50'000.0};
    auto dollars = PositionSizer{cfg}.size(long_signal(), context());
    REQUIRE(dollars->target_position.get() == Catch::Approx(100.0));  // 50k / 500

    cfg.method = SizingMethod::PercentCapital;
    cfg.percent_capital = 0.05;
    auto percent = PositionSizer{cfg}.size(long_signal(), context());
    REQUIRE(percent->target_notional.get() == Catch::Approx(50'000.0));
}

TEST_CASE("volatility targeting sizes down a violent instrument", "[sizing][property]") {
    // A quiet instrument gets more capital, a violent one less. That is what
    // stops a single name dominating portfolio risk.
    SizingConfig cfg;
    cfg.method = SizingMethod::VolatilityTarget;
    cfg.target_volatility = 0.10;
    cfg.limits.max_position_weight = 1.0;
    cfg.limits.max_position_notional = Notional{1e12};
    cfg.limits.max_gross_leverage = 100.0;
    cfg.limits.max_net_leverage = 100.0;

    const PositionSizer sizer{cfg};
    auto quiet = sizer.size(long_signal(), context(1'000'000.0, 0.0005));
    auto wild = sizer.size(long_signal(), context(1'000'000.0, 0.005));
    REQUIRE(quiet.has_value());
    REQUIRE(wild.has_value());
    REQUIRE(std::abs(quiet->target_notional.get()) > std::abs(wild->target_notional.get()));
}

TEST_CASE("zero volatility yields no position rather than infinite", "[sizing][edge]") {
    SizingConfig cfg;
    cfg.method = SizingMethod::VolatilityTarget;
    auto d = PositionSizer{cfg}.size(long_signal(), context(1'000'000.0, 0.0));
    REQUIRE(d.has_value());
    REQUIRE(d->target_position.get() == 0.0);
    REQUIRE(is_finite(d->target_notional.get()));
}

TEST_CASE("Kelly is fractional and capped", "[sizing][kelly][property]") {
    // Full Kelly is optimal only if the edge is exact, which it never is. With
    // an overestimated edge it is spectacularly destructive.
    REQUIRE(PositionSizer::kelly_weight(0.02, 0.04) == Catch::Approx(0.5));
    // No dispersion means no defined Kelly: a riskless positive edge implies
    // infinite leverage, which is a modelling artefact.
    REQUIRE(PositionSizer::kelly_weight(0.02, 0.0) == 0.0);
    REQUIRE(is_finite(PositionSizer::kelly_weight(1e9, 1e-12)));

    SizingConfig full;
    full.method = SizingMethod::Kelly;
    full.kelly_cap = 0.20;
    SizingConfig quarter;
    quarter.method = SizingMethod::FractionalKelly;
    quarter.kelly_fraction = 0.25;
    quarter.kelly_cap = 0.20;

    // An enormous edge must still respect the cap.
    const auto huge = long_signal(0.5, 0.99);
    auto full_size = PositionSizer{full}.size(huge, context());
    REQUIRE(std::abs(full_size->final_weight) <= 0.20 + 1e-12);
}

TEST_CASE("risk parity weights inversely to volatility", "[sizing]") {
    REQUIRE(PositionSizer::risk_parity_weight(0.20, 0.10) == Catch::Approx(0.5));
    REQUIRE(PositionSizer::risk_parity_weight(0.05, 0.10) == Catch::Approx(2.0));
    REQUIRE(PositionSizer::risk_parity_weight(0.0, 0.10) == 0.0);
}

TEST_CASE("exposure limits bind and report why", "[sizing][risk]") {
    SizingConfig cfg;
    cfg.method = SizingMethod::PercentCapital;
    cfg.percent_capital = 0.50;
    cfg.limits.max_position_weight = 0.10;

    auto d = PositionSizer{cfg}.size(long_signal(), context());
    REQUIRE(d.has_value());
    REQUIRE(d->capped);
    REQUIRE(d->final_weight == Catch::Approx(0.10));
    REQUIRE(d->cap_reason.find("position weight") != std::string::npos);
}

TEST_CASE("sector and gross headroom account for existing exposure", "[sizing][risk][property]") {
    // A series of individually-compliant positions must not collectively
    // breach the limit.
    SizingConfig cfg;
    cfg.method = SizingMethod::PercentCapital;
    cfg.percent_capital = 0.20;
    cfg.limits.max_position_weight = 1.0;
    cfg.limits.max_sector_weight = 0.30;
    cfg.limits.max_gross_leverage = 1.0;

    auto ctx = context();
    ctx.sector = 1;
    ctx.sector_exposure = Notional{250'000.0};  // 25% already used
    auto d = PositionSizer{cfg}.size(long_signal(), ctx);
    REQUIRE(d->capped);
    // Only 5% of headroom remains in the sector.
    REQUIRE(d->final_weight == Catch::Approx(0.05));

    auto gross_ctx = context();
    gross_ctx.existing_gross = Notional{950'000.0};
    auto g = PositionSizer{cfg}.size(long_signal(), gross_ctx);
    REQUIRE(g->final_weight == Catch::Approx(0.05));
}

TEST_CASE("a flat or unprofitable signal targets zero", "[sizing][leakage]") {
    // A TARGET of zero, not "leave the position alone": the rebalance engine
    // will close an existing position, which is the correct response to a
    // signal that no longer justifies holding it.
    SizingConfig cfg;
    const PositionSizer sizer{cfg};

    auto flat = sizer.size(signal::Signal::flat(at("2024-07-02T15:00:00Z"), kSpy, 1), context());
    REQUIRE(flat->target_position.get() == 0.0);

    signal::CostEstimate costs;
    costs.half_spread = 1.0;  // dwarfs any edge
    auto unprofitable =
        signal::Signal::create(at("2024-07-02T15:00:00Z"), kSpy, signal::Direction::Long, 0.001,
                               0.9, minutes{15}, 1, costs);
    REQUIRE(sizer.size(*unprofitable, context())->target_position.get() == 0.0);
}

TEST_CASE("rounding never increases exposure", "[sizing][property]") {
    // Rounding toward zero means a lot size can never push a position past a
    // limit that was just applied.
    SizingConfig cfg;
    cfg.method = SizingMethod::FixedShares;
    cfg.fixed_shares = 100.0;
    cfg.lot_size = 7.0;
    auto d = PositionSizer{cfg}.size(long_signal(), context());
    REQUIRE(d->target_position.get() == Catch::Approx(98.0));  // trunc(100/7)*7
    REQUIRE(std::abs(d->target_position.get()) <= 100.0);
}

TEST_CASE("sizing refuses an unusable reference price", "[sizing][validation]") {
    auto ctx = context();
    ctx.reference_price = Price{0.0};
    REQUIRE_FALSE(PositionSizer{}.size(long_signal(), ctx).has_value());
}

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

TEST_CASE("the rebalance engine emits deltas not targets", "[construction][leakage]") {
    // THE DELTA RULE. Emitting orders sized to the target would double a
    // position that was already half built -- the most expensive arithmetic
    // error available in this layer.
    portfolio::Portfolio pf;
    REQUIRE(pf.apply(make_fill(Side::Buy, 60.0, 500.0)).has_value());
    pf.mark(kSpy, Price{500.0}, Price{500.0});

    construction::TargetPortfolio targets{at("2024-07-02T15:00:00Z")};
    construction::TargetPosition t;
    t.instrument = kSpy;
    t.target_quantity = Qty{100.0};
    t.reference_price = Price{500.0};
    REQUIRE(targets.set(t).has_value());

    construction::RebalanceConfig cfg;
    cfg.min_trade_notional = Notional{0.0};
    auto plan = construction::RebalanceEngine{cfg}.plan(targets, pf);
    REQUIRE(plan.has_value());
    REQUIRE(plan->trades.size() == 1);
    // 100 target - 60 held = 40, NOT 100.
    REQUIRE(plan->trades[0].delta_quantity.get() == Catch::Approx(40.0));
    REQUIRE(plan->trades[0].side == Side::Buy);
}

TEST_CASE("an untargeted position is closed not orphaned", "[construction][leakage]") {
    // Iterating only the targets would leave a position open forever once its
    // instrument stopped being targeted.
    portfolio::Portfolio pf;
    REQUIRE(pf.apply(make_fill(Side::Buy, 100.0, 500.0)).has_value());
    pf.mark(kSpy, Price{500.0}, Price{500.0});

    construction::TargetPortfolio empty{at("2024-07-02T15:00:00Z")};
    construction::RebalanceConfig cfg;
    cfg.min_trade_notional = Notional{0.0};
    auto plan = construction::RebalanceEngine{cfg}.plan(empty, pf);
    REQUIRE(plan.has_value());
    REQUIRE(plan->trades.size() == 1);
    REQUIRE(plan->trades[0].delta_quantity.get() == Catch::Approx(-100.0));
    REQUIRE(plan->trades[0].side == Side::Sell);
}

TEST_CASE("the drift threshold suppresses pointless trades", "[construction][property]") {
    // Trading toward a barely-moved target pays the spread repeatedly for no
    // expected return.
    portfolio::Portfolio pf;
    REQUIRE(pf.apply(make_fill(Side::Buy, 100.0, 500.0)).has_value());
    pf.mark(kSpy, Price{500.0}, Price{500.0});

    construction::TargetPortfolio targets{at("2024-07-02T15:00:00Z")};
    construction::TargetPosition t;
    t.instrument = kSpy;
    t.target_quantity = Qty{101.0};  // a 0.05% drift
    t.reference_price = Price{500.0};
    REQUIRE(targets.set(t).has_value());

    construction::RebalanceConfig partial;
    partial.mode = construction::RebalanceMode::Partial;
    partial.drift_threshold = 0.005;
    partial.min_trade_notional = Notional{0.0};
    auto skipped = construction::RebalanceEngine{partial}.plan(targets, pf);
    REQUIRE(skipped->actionable() == 0);
    REQUIRE(skipped->skipped == 1);

    // A full rebalance trades it anyway.
    construction::RebalanceConfig full = partial;
    full.mode = construction::RebalanceMode::Full;
    auto traded = construction::RebalanceEngine{full}.plan(targets, pf);
    REQUIRE(traded->actionable() == 1);
}

TEST_CASE("tiny trades are dropped", "[construction][edge]") {
    // The commission floor makes a tiny trade unprofitable however good the
    // signal.
    portfolio::Portfolio pf;
    pf.mark(kSpy, Price{500.0}, Price{500.0});

    construction::TargetPortfolio targets{at("2024-07-02T15:00:00Z")};
    construction::TargetPosition t;
    t.instrument = kSpy;
    t.target_quantity = Qty{1.0};  // $500
    t.reference_price = Price{500.0};
    REQUIRE(targets.set(t).has_value());

    construction::RebalanceConfig cfg;
    cfg.mode = construction::RebalanceMode::Full;
    cfg.min_trade_notional = Notional{1000.0};
    auto plan = construction::RebalanceEngine{cfg}.plan(targets, pf);
    REQUIRE(plan->actionable() == 0);
    REQUIRE(plan->trades[0].skip_reason.find("minimum trade size") != std::string::npos);
}

TEST_CASE("an excessive turnover plan is refused", "[construction][risk]") {
    // A pathological signal flip would otherwise trade the entire book at once,
    // paying enormous costs on a single model update.
    portfolio::Portfolio pf{portfolio::PortfolioConfig{Notional{100'000.0}}};
    pf.mark(kSpy, Price{500.0}, Price{500.0});
    pf.mark(kQqq, Price{500.0}, Price{500.0});

    construction::TargetPortfolio targets{at("2024-07-02T15:00:00Z")};
    for (const auto instrument : {kSpy, kQqq}) {
        construction::TargetPosition t;
        t.instrument = instrument;
        t.target_quantity = Qty{200.0};  // $100k each on $100k equity
        t.reference_price = Price{500.0};
        REQUIRE(targets.set(t).has_value());
    }

    construction::RebalanceConfig cfg;
    cfg.mode = construction::RebalanceMode::Full;
    cfg.max_turnover_per_rebalance = 0.50;
    auto plan = construction::RebalanceEngine{cfg}.plan(targets, pf);
    REQUIRE_FALSE(plan.has_value());
    REQUIRE(plan.error().message.find("turn over") != std::string::npos);
}

TEST_CASE("duplicate targets are refused", "[construction][validation]") {
    construction::TargetPortfolio targets{at("2024-07-02T15:00:00Z")};
    construction::TargetPosition t;
    t.instrument = kSpy;
    t.target_quantity = Qty{100.0};
    t.reference_price = Price{500.0};
    REQUIRE(targets.set(t).has_value());
    REQUIRE_FALSE(targets.set(t).has_value());

    t.reference_price = Price{0.0};
    t.instrument = kQqq;
    REQUIRE_FALSE(targets.set(t).has_value());
}

TEST_CASE("orders are generated from the plan with correct sides", "[construction][oms]") {
    portfolio::Portfolio pf;
    REQUIRE(pf.apply(make_fill(Side::Buy, 100.0, 500.0)).has_value());
    pf.mark(kSpy, Price{500.0}, Price{500.0});

    construction::TargetPortfolio targets{at("2024-07-02T15:00:00Z")};
    construction::TargetPosition t;
    t.instrument = kSpy;
    t.target_quantity = Qty{20.0};  // reduce
    t.reference_price = Price{500.0};
    REQUIRE(targets.set(t).has_value());

    construction::RebalanceConfig cfg;
    cfg.mode = construction::RebalanceMode::Full;
    cfg.min_trade_notional = Notional{0.0};
    const construction::RebalanceEngine engine{cfg};
    auto plan = engine.plan(targets, pf);
    REQUIRE(plan.has_value());

    std::uint64_t next = 0;
    auto orders = engine.to_orders(*plan, at("2024-07-02T15:00:00Z"),
                                   [&next] { return oms::OrderId{++next}; });
    REQUIRE(orders.has_value());
    REQUIRE(orders->size() == 1);
    REQUIRE(orders->front().side() == Side::Sell);
    REQUIRE(orders->front().quantity().get() == Catch::Approx(80.0));
    REQUIRE(orders->front().type() == oms::OrderType::Market);
}

TEST_CASE("limit orders are offset away from the touch", "[construction][oms][property]") {
    // Offsetting the other way would cross the spread and make the limit a
    // market order in disguise.
    portfolio::Portfolio pf;
    pf.mark(kSpy, Price{500.0}, Price{500.0});

    construction::TargetPortfolio targets{at("2024-07-02T15:00:00Z")};
    construction::TargetPosition t;
    t.instrument = kSpy;
    t.target_quantity = Qty{100.0};
    t.reference_price = Price{500.0};
    REQUIRE(targets.set(t).has_value());

    construction::RebalanceConfig cfg;
    cfg.mode = construction::RebalanceMode::Full;
    cfg.order_type = oms::OrderType::Limit;
    cfg.limit_offset_bps = Bps{10.0};
    const construction::RebalanceEngine engine{cfg};
    auto plan = engine.plan(targets, pf);

    std::uint64_t next = 0;
    auto orders = engine.to_orders(*plan, at("2024-07-02T15:00:00Z"),
                                   [&next] { return oms::OrderId{++next}; });
    REQUIRE(orders.has_value());
    const auto& order = orders->front();
    REQUIRE(order.side() == Side::Buy);
    REQUIRE(order.limit_price().has_value());
    // A buy limit sits BELOW the reference.
    REQUIRE(order.limit_price()->get() < 500.0);
}

TEST_CASE("stop and stop-limit orders are supported", "[construction][oms]") {
    portfolio::Portfolio pf;
    pf.mark(kSpy, Price{500.0}, Price{500.0});
    construction::TargetPortfolio targets{at("2024-07-02T15:00:00Z")};
    construction::TargetPosition t;
    t.instrument = kSpy;
    t.target_quantity = Qty{100.0};
    t.reference_price = Price{500.0};
    REQUIRE(targets.set(t).has_value());

    for (const auto type : {oms::OrderType::Stop, oms::OrderType::StopLimit}) {
        construction::RebalanceConfig cfg;
        cfg.mode = construction::RebalanceMode::Full;
        cfg.order_type = type;
        const construction::RebalanceEngine engine{cfg};
        auto plan = engine.plan(targets, pf);
        std::uint64_t next = 0;
        auto orders = engine.to_orders(*plan, at("2024-07-02T15:00:00Z"),
                                       [&next] { return oms::OrderId{++next}; });
        REQUIRE(orders.has_value());
        REQUIRE(orders->front().type() == type);
        REQUIRE(orders->front().stop_price().has_value());
    }
}

TEST_CASE("rebalance planning is deterministic", "[construction][determinism]") {
    portfolio::Portfolio pf;
    REQUIRE(pf.apply(make_fill(Side::Buy, 37.0, 500.0)).has_value());
    pf.mark(kSpy, Price{500.0}, Price{500.0});

    const auto build = [&pf] {
        construction::TargetPortfolio targets{at("2024-07-02T15:00:00Z")};
        construction::TargetPosition t;
        t.instrument = kSpy;
        t.target_quantity = Qty{113.0};
        t.reference_price = Price{500.0};
        REQUIRE(targets.set(t).has_value());
        construction::RebalanceConfig cfg;
        cfg.mode = construction::RebalanceMode::Full;
        auto plan = construction::RebalanceEngine{cfg}.plan(targets, pf);
        REQUIRE(plan.has_value());
        return std::make_pair(plan->trades[0].delta_quantity.get(), plan->gross_turnover.get());
    };
    REQUIRE(build() == build());
}
