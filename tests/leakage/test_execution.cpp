#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <chrono>

#include "ptl/execution/broker.hpp"
#include "support/ptl_catch.hpp"

using namespace ptl;
using namespace ptl::execution;
using namespace std::chrono;

namespace {

Timestamp at(const char* iso) {
    Timestamp ts{};
    REQUIRE(parse_timestamp(iso, ts));
    return ts;
}

constexpr InstrumentId kSpy{0};

MarketState quoted(double bid, double ask, double vol = 1e6, double size = 1e6) {
    MarketState s;
    s.bid = Price{bid};
    s.ask = Price{ask};
    s.bid_size = Qty{size};
    s.ask_size = Qty{size};
    s.interval_volume = Volume{vol};
    s.intraday_volatility = 0.01;
    s.has_quote = true;
    return s;
}

struct Harness {
    SimulatedClock clock{at("2024-07-02T14:53:00Z")};
    StandardCostModel costs;
    StandardLatencyModel latency;
    BrokerSimulator broker;
    std::uint64_t next = 0;

    explicit Harness(CostConfig cc = CostConfig{}, FillConfig fc = FillConfig{},
                     LatencyConfig lc = LatencyConfig{})
        : costs(cc), latency(lc), broker(clock, costs, latency, DeterministicRng{42}, fc) {}

    oms::Order order(Side side, double qty, std::optional<double> limit = std::nullopt) {
        LifecycleTimes t;
        t.decision_time = clock.now();
        auto o =
            limit.has_value()
                ? oms::Order::limit(oms::OrderId{++next}, kSpy, side, Qty{qty}, Price{*limit}, t)
                : oms::Order::market(oms::OrderId{++next}, kSpy, side, Qty{qty}, t);
        REQUIRE(o.has_value());
        return *o;
    }
};

}  // namespace

TEST_CASE("an order cannot fill on the event that created it", "[execution][leakage]") {
    // THE NO-SAME-BAR RULE AT THE VENUE. Latency means an order submitted at
    // time T arrives strictly after T, so matching at T produces nothing.
    Harness h;
    const auto o = h.order(Side::Buy, 100);
    const auto arrival = h.broker.submit(o);
    REQUIRE(arrival.has_value());
    REQUIRE(*arrival > o.decision_time());

    auto same_instant = h.broker.on_market(kSpy, quoted(499.99, 500.01), h.clock.now());
    REQUIRE(same_instant.has_value());
    REQUIRE(same_instant->empty());

    // Only after the arrival instant does it fill.
    h.clock.advance_by(seconds{1});
    auto later = h.broker.on_market(kSpy, quoted(499.99, 500.01), h.clock.now());
    REQUIRE(later.has_value());
    REQUIRE(later->size() == 1);
}

TEST_CASE("zero-latency configuration still cannot fill instantly", "[execution][leakage]") {
    // Even with every latency component set to zero, arrival must be strictly
    // after the decision. The venue enforces this independently of anything
    // upstream.
    LatencyConfig zero;
    zero.market_data = Duration::zero();
    zero.strategy_compute = Duration::zero();
    zero.order_transmission = Duration::zero();
    zero.exchange_processing = Duration::zero();
    zero.acknowledgement = Duration::zero();

    Harness h{CostConfig{}, FillConfig{}, zero};
    const auto o = h.order(Side::Buy, 100);
    const auto arrival = h.broker.submit(o);
    REQUIRE(arrival.has_value());
    REQUIRE(*arrival > o.decision_time());
}

TEST_CASE("a buy crosses to the ask and a sell to the bid", "[execution][leakage]") {
    CostConfig cc;
    cc.stochastic_slippage_bps = Bps{0.0};
    cc.impact_coefficient = 0.0;
    cc.commission_per_share = 0.0;
    cc.minimum_commission = 0.0;

    Harness h{cc};
    REQUIRE(h.broker.submit(h.order(Side::Buy, 100)).has_value());
    REQUIRE(h.broker.submit(h.order(Side::Sell, 100)).has_value());
    h.clock.advance_by(seconds{1});

    auto fills = h.broker.on_market(kSpy, quoted(499.0, 501.0), h.clock.now());
    REQUIRE(fills->size() == 2);
    REQUIRE((*fills)[0].price().get() == Catch::Approx(501.0));  // buy pays the ask
    REQUIRE((*fills)[1].price().get() == Catch::Approx(499.0));  // sell hits the bid
}

TEST_CASE("costs always move the fill against the trader", "[execution][leakage]") {
    // Slippage is drawn half-normal precisely so it can never pay the strategy.
    // A signed draw would flatter every result for free roughly half the time.
    CostConfig cc;
    cc.stochastic_slippage_bps = Bps{5.0};
    cc.impact_coefficient = 0.0;

    Harness h{cc};
    REQUIRE(h.broker.submit(h.order(Side::Buy, 100)).has_value());
    REQUIRE(h.broker.submit(h.order(Side::Sell, 100)).has_value());
    h.clock.advance_by(seconds{1});

    auto fills = h.broker.on_market(kSpy, quoted(499.0, 501.0), h.clock.now());
    REQUIRE(fills->size() == 2);
    REQUIRE((*fills)[0].price().get() >= 501.0);  // buy never better than the ask
    REQUIRE((*fills)[1].price().get() <= 499.0);  // sell never better than the bid
}

TEST_CASE("a non-marketable limit does not fill on a touch", "[execution][leakage]") {
    // A bar whose low equals your limit says NOTHING about whether you traded.
    // This is where the project declines to claim queue-position realism it
    // cannot support (ADR-0001, ADR-0003).
    Harness h;
    REQUIRE(h.broker.submit(h.order(Side::Buy, 100, 495.0)).has_value());
    h.clock.advance_by(seconds{1});

    auto fills = h.broker.on_market(kSpy, quoted(499.0, 501.0), h.clock.now());
    REQUIRE(fills->empty());
    REQUIRE(h.broker.stats().passive_no_fill > 0);

    // Once the market comes to it, it fills.
    auto crossed = h.broker.on_market(kSpy, quoted(493.0, 494.0), h.clock.now());
    REQUIRE(crossed->size() == 1);
}

TEST_CASE("fills are capped by the participation rate", "[execution][leakage]") {
    FillConfig fc;
    fc.max_participation_rate = 0.10;
    fc.respect_displayed_size = false;

    Harness h{CostConfig{}, fc};
    REQUIRE(h.broker.submit(h.order(Side::Buy, 1000)).has_value());
    h.clock.advance_by(seconds{1});

    // Only 5000 shares traded this interval, so at most 500 are available.
    auto fills = h.broker.on_market(kSpy, quoted(499.0, 501.0, 5000.0), h.clock.now());
    REQUIRE(fills->size() == 1);
    REQUIRE(fills->front().quantity().get() == Catch::Approx(500.0));
    REQUIRE(h.broker.stats().participation_capped > 0);
    REQUIRE(h.broker.stats().partial_fills > 0);
}

TEST_CASE("zero interval volume means no fill not an unconstrained one",
          "[execution][leakage][edge]") {
    // Zero-volume minutes are real for XLE and TLT. Treating "no participation
    // limit computed" as "unlimited" would fill an arbitrary size into a market
    // where nothing traded.
    Harness h;
    REQUIRE(h.broker.submit(h.order(Side::Buy, 1000)).has_value());
    h.clock.advance_by(seconds{1});
    auto fills = h.broker.on_market(kSpy, quoted(499.0, 501.0, 0.0), h.clock.now());
    REQUIRE(fills->empty());
}

TEST_CASE("fills are capped by displayed size", "[execution]") {
    FillConfig fc;
    fc.respect_displayed_size = true;
    fc.max_participation_rate = 1.0;

    Harness h{CostConfig{}, fc};
    REQUIRE(h.broker.submit(h.order(Side::Buy, 1000)).has_value());
    h.clock.advance_by(seconds{1});
    auto fills = h.broker.on_market(kSpy, quoted(499.0, 501.0, 1e9, 300.0), h.clock.now());
    REQUIRE(fills->front().quantity().get() == Catch::Approx(300.0));
    REQUIRE(h.broker.stats().displayed_size_capped > 0);
}

TEST_CASE("an IOC remainder is cancelled rather than left resting", "[execution]") {
    FillConfig fc;
    fc.respect_displayed_size = true;
    Harness h{CostConfig{}, fc};

    LifecycleTimes t;
    t.decision_time = h.clock.now();
    auto o = oms::Order::market(oms::OrderId{1}, kSpy, Side::Buy, Qty{1000}, t,
                                oms::TimeInForce::ImmediateOrCancel);
    REQUIRE(h.broker.submit(*o).has_value());
    h.clock.advance_by(seconds{1});

    auto fills = h.broker.on_market(kSpy, quoted(499.0, 501.0, 1e9, 100.0), h.clock.now());
    REQUIRE(fills->front().quantity().get() == Catch::Approx(100.0));
    REQUIRE(h.broker.pending_count() == 0);
}

TEST_CASE("market impact grows with participation and stays finite", "[execution][edge]") {
    StandardCostModel costs{CostConfig{}};
    auto st = quoted(499.0, 501.0, 100000.0);

    const double small = costs.market_impact(Qty{100}, st).get();
    const double large = costs.market_impact(Qty{10000}, st).get();
    REQUIRE(large > small);
    REQUIRE(is_finite(small));

    // Zero volume yields zero impact, not infinity.
    st.interval_volume = Volume{0.0};
    REQUIRE(costs.market_impact(Qty{100}, st).get() == 0.0);
}

TEST_CASE("the cost multiplier scales every component", "[execution]") {
    // The mandated 0.5x/1x/2x/3x sensitivity sweep needs one knob.
    CostConfig base;
    CostConfig doubled = base;
    doubled.cost_multiplier = 2.0;

    StandardCostModel a{base};
    StandardCostModel b{doubled};
    auto st = quoted(499.0, 501.0);

    REQUIRE(b.commission(Qty{1000}, Price{500}).get() ==
            Catch::Approx(2.0 * a.commission(Qty{1000}, Price{500}).get()));
    REQUIRE(b.market_impact(Qty{1000}, st).get() ==
            Catch::Approx(2.0 * a.market_impact(Qty{1000}, st).get()));
}

TEST_CASE("raising costs cannot improve a fill", "[execution][property]") {
    // Cost monotonicity: the research names this as a mandatory test.
    const auto price_at = [](double multiplier) {
        CostConfig cc;
        cc.cost_multiplier = multiplier;
        Harness h{cc};
        (void)h.broker.submit(h.order(Side::Buy, 100));
        h.clock.advance_by(seconds{1});
        auto fills = h.broker.on_market(kSpy, quoted(499.0, 501.0), h.clock.now());
        REQUIRE(fills->size() == 1);
        return fills->front().price().get() + fills->front().total_cost().get();
    };
    REQUIRE(price_at(3.0) >= price_at(1.0));
    REQUIRE(price_at(1.0) >= price_at(0.5));
}

TEST_CASE("the same seed produces identical fills", "[execution][determinism]") {
    const auto run = [] {
        Harness h;
        (void)h.broker.submit(h.order(Side::Buy, 100));
        (void)h.broker.submit(h.order(Side::Sell, 50));
        h.clock.advance_by(seconds{1});
        auto fills = h.broker.on_market(kSpy, quoted(499.0, 501.0), h.clock.now());
        std::vector<double> prices;
        for (const auto& f : *fills) prices.push_back(f.price().get());
        return prices;
    };
    REQUIRE(run() == run());
}

TEST_CASE("slippage is signed so positive always means cost", "[execution][property]") {
    // A buy filling above arrival and a sell filling below both HURT. One
    // convention spares every downstream aggregate a special case.
    CostConfig cc;
    cc.stochastic_slippage_bps = Bps{0.0};
    cc.impact_coefficient = 0.0;

    Harness h{cc};
    auto buy = h.order(Side::Buy, 100).with_arrival_price(Price{500.0});
    auto sell = h.order(Side::Sell, 100).with_arrival_price(Price{500.0});
    REQUIRE(h.broker.submit(buy).has_value());
    REQUIRE(h.broker.submit(sell).has_value());
    h.clock.advance_by(seconds{1});

    auto fills = h.broker.on_market(kSpy, quoted(499.0, 501.0), h.clock.now());
    REQUIRE(fills->size() == 2);
    // Buy filled at 501 vs arrival 500: +20 bps of cost.
    REQUIRE((*fills)[0].slippage_bps().get() == Catch::Approx(20.0));
    // Sell filled at 499 vs arrival 500: also +20 bps of COST, not -20.
    REQUIRE((*fills)[1].slippage_bps().get() == Catch::Approx(20.0));
}

TEST_CASE("an order decided in the future is rejected", "[execution][leakage]") {
    Harness h;
    LifecycleTimes t;
    t.decision_time = h.clock.now() + hours{1};
    auto o = oms::Order::market(oms::OrderId{1}, kSpy, Side::Buy, Qty{100}, t);
    REQUIRE_FALSE(h.broker.submit(*o).has_value());
    REQUIRE(h.broker.stats().orders_rejected == 1);
}
