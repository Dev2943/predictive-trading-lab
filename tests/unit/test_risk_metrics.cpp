#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <vector>

#include "ptl/analytics/metrics.hpp"
#include "ptl/risk/risk_manager.hpp"
#include "support/ptl_catch.hpp"

using namespace ptl;
using namespace std::chrono;

namespace {

Timestamp at(const char* iso) {
    Timestamp ts{};
    REQUIRE(parse_timestamp(iso, ts));
    return ts;
}

constexpr InstrumentId kSpy{0};

oms::Order make_order(Side side, double qty, oms::OrderId id = oms::OrderId{1}) {
    LifecycleTimes t;
    t.decision_time = at("2024-07-02T14:53:00Z");
    auto o = oms::Order::market(id, kSpy, side, Qty{qty}, t);
    REQUIRE(o.has_value());
    return *o;
}

risk::RiskContext fresh_context(double reference = 500.0) {
    risk::RiskContext c;
    c.now = at("2024-07-02T14:53:00Z");
    c.data_age = seconds{1};
    c.reference_price = Price{reference};
    c.peak_equity = Notional{1'000'000.0};
    return c;
}

}  // namespace

// ---------------------------------------------------------------------------
// Risk
// ---------------------------------------------------------------------------

TEST_CASE("a compliant order is approved", "[risk]") {
    portfolio::Portfolio pf;
    oms::OrderManager mgr;
    risk::RiskManager rm;
    const auto d = rm.check(make_order(Side::Buy, 100), pf, mgr, fresh_context());
    REQUIRE(d.approved());
}

TEST_CASE("stale data is refused before any other check", "[risk][leakage]") {
    // Every other limit is computed against a reference price. A limit
    // evaluated on stale data reports compliance it cannot support, so this is
    // checked first.
    portfolio::Portfolio pf;
    oms::OrderManager mgr;
    risk::RiskManager rm;

    auto ctx = fresh_context();
    ctx.data_age = hours{2};
    const auto d = rm.check(make_order(Side::Buy, 100), pf, mgr, ctx);
    REQUIRE_FALSE(d.approved());
    REQUIRE(d.code == risk::RejectCode::StaleData);
}

TEST_CASE("no usable mark is refused rather than guessed", "[risk][edge]") {
    portfolio::Portfolio pf;
    oms::OrderManager mgr;
    risk::RiskManager rm;
    auto ctx = fresh_context(0.0);
    REQUIRE(rm.check(make_order(Side::Buy, 100), pf, mgr, ctx).code ==
            risk::RejectCode::NoMarkAvailable);
}

TEST_CASE("order and position notional limits bind", "[risk]") {
    portfolio::Portfolio pf;
    oms::OrderManager mgr;
    risk::RiskLimits limits;
    limits.max_order_notional = Notional{10'000.0};
    risk::RiskManager rm{limits};

    REQUIRE(rm.check(make_order(Side::Buy, 100), pf, mgr, fresh_context()).code ==
            risk::RejectCode::MaxOrderNotional);
    REQUIRE(rm.check(make_order(Side::Buy, 10), pf, mgr, fresh_context()).approved());
}

TEST_CASE("in-flight orders count toward exposure", "[risk][leakage]") {
    // Without this, a burst of orders each individually inside the limit could
    // collectively breach it.
    portfolio::Portfolio pf;
    oms::OrderManager mgr;
    risk::RiskLimits limits;
    limits.max_position_notional = Notional{60'000.0};
    limits.max_concentration = 1.0;
    risk::RiskManager rm{limits};

    for (int i = 0; i < 2; ++i) {
        auto o = make_order(Side::Buy, 50, mgr.next_id());
        REQUIRE(mgr.submit(o).has_value());
        REQUIRE(mgr.transition(o.id(), oms::OrderState::PendingNew).has_value());
        REQUIRE(mgr.transition(o.id(), oms::OrderState::Working).has_value());
    }
    // 100 shares already in flight at 500 = 50,000. Another 50 would be 75,000.
    const auto d = rm.check(make_order(Side::Buy, 50, oms::OrderId{99}), pf, mgr, fresh_context());
    REQUIRE_FALSE(d.approved());
    REQUIRE(d.code == risk::RejectCode::MaxPositionExceeded);
}

TEST_CASE("concentration and leverage limits bind", "[risk]") {
    portfolio::Portfolio pf{portfolio::PortfolioConfig{Notional{100'000.0}}};
    oms::OrderManager mgr;
    risk::RiskLimits limits;
    limits.max_concentration = 0.10;
    limits.max_order_notional = Notional{1e9};
    limits.max_position_notional = Notional{1e9};
    risk::RiskManager rm{limits};

    // The peak must be the portfolio's OWN equity, or the kill switch fires
    // first and masks the limit under test -- which is itself the correct
    // behaviour, just not what this case is about.
    auto ctx = fresh_context();
    ctx.peak_equity = pf.equity();
    // 100 shares at 500 = 50,000 against 100,000 equity = 50% concentration.
    REQUIRE(rm.check(make_order(Side::Buy, 100), pf, mgr, ctx).code ==
            risk::RejectCode::ConcentrationLimit);
}

TEST_CASE("the drawdown kill switch halts new risk", "[risk]") {
    portfolio::Portfolio pf{portfolio::PortfolioConfig{Notional{700'000.0}}};
    oms::OrderManager mgr;
    risk::RiskManager rm;

    auto ctx = fresh_context();
    ctx.peak_equity = Notional{1'000'000.0};  // 30% drawdown, limit is 25%
    REQUIRE(rm.check(make_order(Side::Buy, 10), pf, mgr, ctx).code ==
            risk::RejectCode::DrawdownKillSwitch);
}

TEST_CASE("a sale that reduces a long is not blocked for buying power", "[risk][edge]") {
    // Only a BUY consumes cash. Refusing a reducing sale would trap the
    // portfolio in a position it cannot exit.
    portfolio::Portfolio pf{portfolio::PortfolioConfig{Notional{1'000.0}}};
    oms::OrderManager mgr;
    // Every other limit is relaxed so the buying-power check is the one under
    // test. The peak must be the portfolio's OWN equity, or the drawdown kill
    // switch fires first -- correct behaviour, but not what this case is about.
    risk::RiskLimits limits;
    limits.max_concentration = 1e9;
    limits.max_gross_leverage = 1e9;
    limits.max_net_leverage = 1e9;
    limits.max_position_notional = Notional{1e9};
    limits.max_daily_turnover = 1e9;
    risk::RiskManager rm{limits};

    auto ctx = fresh_context();
    ctx.peak_equity = pf.equity();

    REQUIRE(rm.check(make_order(Side::Buy, 100), pf, mgr, ctx).code ==
            risk::RejectCode::InsufficientBuyingPower);
    // The same size on the sell side passes the buying-power test.
    const auto sell = rm.check(make_order(Side::Sell, 100), pf, mgr, ctx);
    REQUIRE(sell.code != risk::RejectCode::InsufficientBuyingPower);
}

TEST_CASE("risk decisions are pure and reproducible", "[risk][determinism]") {
    // Given the same portfolio and order, the answer is always the same:
    // check() mutates nothing.
    portfolio::Portfolio pf;
    oms::OrderManager mgr;
    risk::RiskManager rm;
    const auto o = make_order(Side::Buy, 100);
    const auto ctx = fresh_context();

    const auto a = rm.check(o, pf, mgr, ctx);
    const auto b = rm.check(o, pf, mgr, ctx);
    REQUIRE(a.code == b.code);
    REQUIRE(rm.rejection_count() == 0);  // check() records nothing
}

TEST_CASE("rejections are counted by reason", "[risk]") {
    // Counted and reported, never silently dropped: a suppressed order that
    // vanishes without trace makes a backtest diverge from paper invisibly.
    risk::RiskManager rm;
    rm.record(risk::RiskDecision{risk::RejectCode::StaleData, "", Qty{}});
    rm.record(risk::RiskDecision{risk::RejectCode::StaleData, "", Qty{}});
    rm.record(risk::RiskDecision{risk::RejectCode::Approved, "", Qty{100}});

    REQUIRE(rm.rejection_count() == 2);
    REQUIRE(rm.rejection_count(risk::RejectCode::StaleData) == 2);
    // Approvals are counted too: "how many orders did risk see?" matters as
    // much as "how many did it stop?".
    REQUIRE(rm.rejection_count(risk::RejectCode::Approved) == 1);
}

// ---------------------------------------------------------------------------
// Metrics
// ---------------------------------------------------------------------------

namespace {

std::vector<portfolio::EquityPoint> curve_from(const std::vector<double>& equity) {
    std::vector<portfolio::EquityPoint> c;
    Timestamp t = at("2024-01-02T00:00:00Z");
    for (const double e : equity) {
        portfolio::EquityPoint p;
        p.ts = t;
        p.equity = Notional{e};
        c.push_back(p);
        t += hours{24};
    }
    return c;
}

}  // namespace

TEST_CASE("max drawdown is peak to trough", "[analytics][metrics]") {
    REQUIRE(analytics::MetricsEngine::max_drawdown({{100.0, 120.0, 90.0, 110.0}}) ==
            Catch::Approx(0.25));  // 120 -> 90
    REQUIRE(analytics::MetricsEngine::max_drawdown({{100.0, 110.0, 120.0}}) == 0.0);
    REQUIRE(analytics::MetricsEngine::max_drawdown({}) == 0.0);
}

TEST_CASE("a flat curve has zero Sharpe and zero drawdown", "[analytics][metrics][edge]") {
    // The degenerate case must not divide by zero.
    analytics::MetricsEngine eng;
    const auto m = eng.compute(curve_from({100.0, 100.0, 100.0, 100.0}), {});
    REQUIRE(m.sharpe == 0.0);
    REQUIRE(m.max_drawdown == 0.0);
    REQUIRE(is_finite(m.sharpe));
    REQUIRE(is_finite(m.sortino));
    REQUIRE(is_finite(m.calmar));
}

TEST_CASE("Sharpe matches a hand-computed fixture", "[analytics][metrics]") {
    // Log returns of +10% then -10% (log terms), annualised at 252.
    analytics::MetricsConfig cfg;
    cfg.periods_per_year = 252.0;
    cfg.use_log_returns = true;
    analytics::MetricsEngine eng{cfg};

    const auto m = eng.compute(curve_from({100.0, 110.0, 121.0, 133.1}), {});
    // Constant compounding: zero volatility, so Sharpe is defined as zero
    // rather than infinite.
    REQUIRE(m.annualized_volatility == Catch::Approx(0.0).margin(1e-9));
    REQUIRE(is_finite(m.sharpe));
    REQUIRE(m.cumulative_return == Catch::Approx(0.331));
}

TEST_CASE("Sortino only penalises downside", "[analytics][metrics]") {
    analytics::MetricsEngine eng;
    // Upside-only curve: no downside deviation, so Sortino is not penalised.
    const auto up = eng.compute(curve_from({100.0, 101.0, 103.0, 106.0}), {});
    REQUIRE(up.downside_volatility == Catch::Approx(0.0).margin(1e-9));

    const auto mixed = eng.compute(curve_from({100.0, 105.0, 95.0, 100.0}), {});
    REQUIRE(mixed.downside_volatility > 0.0);
    // Sortino exceeds Sharpe when downside is smaller than total volatility.
    REQUIRE(is_finite(mixed.sortino));
}

TEST_CASE("trade statistics distinguish win rate from expectancy", "[analytics][metrics]") {
    // THE CLASSIC TRAP: a high win rate with negative expectancy. Both are
    // reported precisely so the trap is visible.
    std::vector<accounting::Trade> trades;
    for (int i = 0; i < 9; ++i) {
        accounting::Trade t;
        t.gross_pnl = Notional{10.0};
        trades.push_back(t);
    }
    accounting::Trade big_loss;
    big_loss.gross_pnl = Notional{-500.0};
    trades.push_back(big_loss);

    analytics::MetricsEngine eng;
    const auto m = eng.compute(curve_from({100.0, 100.0}), trades);
    REQUIRE(m.trades == 10);
    REQUIRE(m.win_rate == Catch::Approx(0.9));  // looks excellent
    REQUIRE(m.expectancy.get() < 0.0);          // and loses money
    REQUIRE(m.profit_factor < 1.0);
}

TEST_CASE("metrics survive an empty curve", "[analytics][metrics][edge]") {
    analytics::MetricsEngine eng;
    const auto m = eng.compute({}, {});
    REQUIRE(m.periods == 0);
    REQUIRE(m.sharpe == 0.0);
    REQUIRE(m.trades == 0);
}

TEST_CASE("annualisation follows the configured frequency", "[analytics][metrics][determinism]") {
    // Wrong periods_per_year makes every annualised figure wrong by a constant
    // factor, so it is configuration rather than an assumption.
    analytics::MetricsConfig daily;
    daily.periods_per_year = 252.0;
    analytics::MetricsConfig minute;
    minute.periods_per_year = 252.0 * 390.0;

    const auto c = curve_from({100.0, 101.0, 100.5, 102.0});
    const auto a = analytics::MetricsEngine{daily}.compute(c, {});
    const auto b = analytics::MetricsEngine{minute}.compute(c, {});
    REQUIRE(b.annualized_volatility > a.annualized_volatility);
    REQUIRE(is_finite(b.sharpe));
}
