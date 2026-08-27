#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <chrono>

#include "ptl/execution/broker.hpp"
#include "ptl/portfolio/portfolio.hpp"
#include "support/ptl_catch.hpp"

using namespace ptl;
using namespace ptl::portfolio;
using namespace std::chrono;

namespace {

Timestamp at(const char* iso) {
    Timestamp ts{};
    REQUIRE(parse_timestamp(iso, ts));
    return ts;
}

constexpr InstrumentId kSpy{0};

/// Fills can only come from a BrokerSimulator -- the constructor is private and
/// the simulator is its sole friend. This helper drives a real one, which is
/// itself the test that the restriction holds: there is no other way to get a
/// Fill, not even here.
class FillFactory {
public:
    FillFactory()
        : clock_(at("2024-07-02T14:53:00Z")),
          costs_(execution::CostConfig{0.0, 0.0, Bps{0}, Bps{0}, Bps{0}, 0.0, 0.5, 1.0, Bps{0}}),
          latency_(execution::LatencyConfig{}),
          broker_(clock_, costs_, latency_, DeterministicRng{1},
                  execution::FillConfig{1.0, false, 0.5, true}) {}

    oms::Fill make(Side side, double qty, double price) {
        LifecycleTimes t;
        t.decision_time = clock_.now();
        auto order = oms::Order::market(oms::OrderId{++next_}, kSpy, side, Qty{qty}, t);
        REQUIRE(order.has_value());
        REQUIRE(broker_.submit(*order).has_value());

        execution::MarketState st;
        st.bid = Price{price};
        st.ask = Price{price};
        st.interval_volume = Volume{1e9};
        st.has_quote = true;

        clock_.advance_by(seconds{1});
        auto fills = broker_.on_market(kSpy, st, clock_.now());
        REQUIRE(fills.has_value());
        REQUIRE(fills->size() == 1);
        return fills->front();
    }

private:
    SimulatedClock clock_;
    execution::StandardCostModel costs_;
    execution::StandardLatencyModel latency_;
    execution::BrokerSimulator broker_;
    std::uint64_t next_ = 0;
};

}  // namespace

TEST_CASE("the accounting identity holds after every fill", "[portfolio][accounting][property]") {
    // equity == cash + sum(quantity * mark), asserted at every snapshot. This
    // is the invariant every other number in the system rests on.
    Portfolio pf{PortfolioConfig{Notional{1'000'000.0}}};
    FillFactory ff;

    REQUIRE(pf.apply(ff.make(Side::Buy, 100, 500.0)).has_value());
    pf.mark(kSpy, Price{500.0}, Price{500.0});
    REQUIRE(pf.identity_holds());
    REQUIRE(pf.cash().get() == Catch::Approx(1'000'000.0 - 50'000.0));
    REQUIRE(pf.equity().get() == Catch::Approx(1'000'000.0));

    REQUIRE(pf.apply(ff.make(Side::Sell, 40, 510.0)).has_value());
    pf.mark(kSpy, Price{510.0}, Price{510.0});
    REQUIRE(pf.identity_holds());
    // 40 shares closed at +10 each.
    REQUIRE(pf.realized_pnl().get() == Catch::Approx(400.0));
}

TEST_CASE("a position reversal resets the cost basis", "[portfolio][accounting][property]") {
    // THE CASE THAT BREAKS NAIVE IMPLEMENTATIONS. Selling a long through zero
    // into a short must realise on the closing portion only, then RESET the
    // basis. Carrying the old average across would leave the short marked
    // against a long's entry price and every unrealised number afterwards
    // would be wrong.
    Portfolio pf;
    FillFactory ff;

    REQUIRE(pf.apply(ff.make(Side::Buy, 100, 500.0)).has_value());
    REQUIRE(pf.position(kSpy)->average_cost().get() == Catch::Approx(500.0));

    // Sell 150: closes 100 long at +10, opens 50 short at 510.
    REQUIRE(pf.apply(ff.make(Side::Sell, 150, 510.0)).has_value());
    const auto* pos = pf.position(kSpy);
    REQUIRE(pos->quantity().get() == Catch::Approx(-50.0));
    REQUIRE(pos->is_short());
    REQUIRE(pf.realized_pnl().get() == Catch::Approx(1000.0));
    // Basis RESET to the reversal price, not carried from the long.
    REQUIRE(pos->average_cost().get() == Catch::Approx(510.0));

    // A short profits when the price falls.
    pf.mark(kSpy, Price{500.0}, Price{500.0});
    REQUIRE(pos->unrealized_pnl(Price{500.0}).get() == Catch::Approx(500.0));
}

TEST_CASE("closing to flat zeroes the basis", "[portfolio][accounting]") {
    Portfolio pf;
    FillFactory ff;
    REQUIRE(pf.apply(ff.make(Side::Buy, 100, 500.0)).has_value());
    REQUIRE(pf.apply(ff.make(Side::Sell, 100, 505.0)).has_value());
    const auto* pos = pf.position(kSpy);
    REQUIRE(pos->is_flat());
    REQUIRE(pos->average_cost().get() == 0.0);
    REQUIRE(pf.realized_pnl().get() == Catch::Approx(500.0));
    REQUIRE(pf.unrealized_pnl().get() == 0.0);
}

TEST_CASE("partial reduction leaves the average cost unchanged", "[portfolio][accounting]") {
    Portfolio pf;
    FillFactory ff;
    REQUIRE(pf.apply(ff.make(Side::Buy, 100, 500.0)).has_value());
    REQUIRE(pf.apply(ff.make(Side::Buy, 100, 520.0)).has_value());
    REQUIRE(pf.position(kSpy)->average_cost().get() == Catch::Approx(510.0));

    REQUIRE(pf.apply(ff.make(Side::Sell, 50, 530.0)).has_value());
    // Weighted-average accounting: reducing does not move the basis.
    REQUIRE(pf.position(kSpy)->average_cost().get() == Catch::Approx(510.0));
    REQUIRE(pf.realized_pnl().get() == Catch::Approx(1000.0));
}

TEST_CASE("longs mark to bid and shorts to ask", "[portfolio][accounting][leakage]") {
    // Mid-marking overstates NAV by half a spread per unit of gross exposure on
    // EVERY bar, which compounds into a material and entirely fictional return.
    Portfolio liq{PortfolioConfig{Notional{1'000'000.0}, MarkMode::Liquidation}};
    FillFactory ff;
    REQUIRE(liq.apply(ff.make(Side::Buy, 100, 500.0)).has_value());
    liq.mark(kSpy, Price{499.0}, Price{501.0});
    // A long liquidates at the BID.
    REQUIRE(liq.position_value().get() == Catch::Approx(49'900.0));

    Portfolio mid{PortfolioConfig{Notional{1'000'000.0}, MarkMode::Mid}};
    FillFactory ff2;
    REQUIRE(mid.apply(ff2.make(Side::Buy, 100, 500.0)).has_value());
    mid.mark(kSpy, Price{499.0}, Price{501.0});
    REQUIRE(mid.position_value().get() == Catch::Approx(50'000.0));
    // The overstatement is exactly half the spread times the quantity.
    REQUIRE(mid.position_value().get() - liq.position_value().get() == Catch::Approx(100.0));
}

TEST_CASE("a short marks to ask", "[portfolio][accounting]") {
    Portfolio pf;
    FillFactory ff;
    REQUIRE(pf.apply(ff.make(Side::Sell, 100, 500.0)).has_value());
    pf.mark(kSpy, Price{499.0}, Price{501.0});
    // Buying back costs the ASK, so the short is marked there.
    REQUIRE(pf.position_value().get() == Catch::Approx(-50'100.0));
}

TEST_CASE("a split preserves economic value", "[portfolio][accounting][corpaction]") {
    // Shares scale up, basis scales down, and the product is invariant. This is
    // the synthetic-split invariant the research names explicitly.
    Portfolio pf;
    FillFactory ff;
    REQUIRE(pf.apply(ff.make(Side::Buy, 100, 500.0)).has_value());
    pf.mark(kSpy, Price{500.0}, Price{500.0});
    const double before = pf.equity().get();

    REQUIRE(pf.apply_split(kSpy, 2.0).has_value());
    const auto* pos = pf.position(kSpy);
    REQUIRE(pos->quantity().get() == Catch::Approx(200.0));
    REQUIRE(pos->average_cost().get() == Catch::Approx(250.0));
    // Marks scale too, or the position would show a fictional jump.
    REQUIRE(pf.equity().get() == Catch::Approx(before));
    REQUIRE(pf.identity_holds());
}

TEST_CASE("a dividend credits cash and a short pays it", "[portfolio][accounting][corpaction]") {
    // Treating a dividend as a price return without the cash leg is a classic
    // accounting error.
    Portfolio lng;
    FillFactory ff;
    REQUIRE(lng.apply(ff.make(Side::Buy, 100, 500.0)).has_value());
    const double cash_before = lng.cash().get();
    REQUIRE(lng.apply_dividend(kSpy, Notional{1.50}).has_value());
    REQUIRE(lng.cash().get() == Catch::Approx(cash_before + 150.0));

    Portfolio shrt;
    FillFactory ff2;
    REQUIRE(shrt.apply(ff2.make(Side::Sell, 100, 500.0)).has_value());
    const double short_cash = shrt.cash().get();
    REQUIRE(shrt.apply_dividend(kSpy, Notional{1.50}).has_value());
    REQUIRE(shrt.cash().get() == Catch::Approx(short_cash - 150.0));
}

TEST_CASE("leverage and buying power stay finite", "[portfolio][accounting][edge]") {
    Portfolio pf;
    // Zero exposure means zero leverage, and zero equity must yield zero rather
    // than infinity -- an inf would poison every risk check downstream.
    REQUIRE(pf.leverage() == 0.0);
    REQUIRE(is_finite(pf.leverage()));
    REQUIRE(pf.buying_power().get() == Catch::Approx(1'000'000.0));

    FillFactory ff;
    REQUIRE(pf.apply(ff.make(Side::Buy, 100, 500.0)).has_value());
    pf.mark(kSpy, Price{500.0}, Price{500.0});
    REQUIRE(pf.leverage() == Catch::Approx(0.05));
}

TEST_CASE("shorting can be disabled", "[portfolio][accounting]") {
    Portfolio pf{PortfolioConfig{Notional{1'000'000.0}, MarkMode::Liquidation, 0.0, false}};
    FillFactory ff;
    REQUIRE_FALSE(pf.apply(ff.make(Side::Sell, 100, 500.0)).has_value());
}

TEST_CASE("the equity curve refuses to move backwards", "[portfolio][accounting][determinism]") {
    // A backwards snapshot would corrupt every time-ordered metric computed
    // from the curve.
    Portfolio pf;
    REQUIRE(pf.snapshot(at("2024-07-02T20:00:00Z")).has_value());
    REQUIRE_FALSE(pf.snapshot(at("2024-07-01T20:00:00Z")).has_value());
    REQUIRE(pf.snapshot(at("2024-07-03T20:00:00Z")).has_value());
    REQUIRE(pf.equity_curve().size() == 2);
}

TEST_CASE("costs are charged on filled quantity only", "[portfolio][accounting]") {
    // Charging on the requested quantity is the classic double-count that makes
    // partial fills look more expensive than they are.
    execution::StandardCostModel costs{execution::CostConfig{}};
    REQUIRE(costs.commission(Qty{100}, Price{500}).get() == Catch::Approx(0.35));
    REQUIRE(costs.commission(Qty{1000}, Price{500}).get() == Catch::Approx(3.5));
    REQUIRE(costs.commission(Qty{0}, Price{500}).get() == 0.0);
}
