#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <chrono>

#include "ptl/oms/order_manager.hpp"
#include "support/ptl_catch.hpp"

using namespace ptl;
using namespace ptl::oms;
using namespace std::chrono;

namespace {

Timestamp at(const char* iso) {
    Timestamp ts{};
    REQUIRE(parse_timestamp(iso, ts));
    return ts;
}

constexpr InstrumentId kSpy{0};

LifecycleTimes decided(const char* iso = "2024-07-02T14:53:00Z") {
    LifecycleTimes t;
    t.decision_time = at(iso);
    return t;
}

}  // namespace

TEST_CASE("market orders carry no prices", "[oms][order]") {
    auto o = Order::market(OrderId{1}, kSpy, Side::Buy, Qty{100}, decided());
    REQUIRE(o.has_value());
    REQUIRE(o->type() == OrderType::Market);
    // ADR-0002: absence is std::optional, never a NaN sentinel.
    REQUIRE_FALSE(o->limit_price().has_value());
    REQUIRE_FALSE(o->stop_price().has_value());
    REQUIRE(o->signed_quantity().get() == 100.0);
}

TEST_CASE("order type and price engagement must agree", "[oms][order][validation]") {
    // Type is the authority; price engagement is a consequence. Enforcing the
    // agreement here means the two cannot drift apart later.
    REQUIRE_FALSE(
        Order::limit(OrderId{1}, kSpy, Side::Buy, Qty{100}, Price{0}, decided()).has_value());
    REQUIRE(Order::limit(OrderId{1}, kSpy, Side::Buy, Qty{100}, Price{540}, decided()).has_value());
    REQUIRE(Order::stop(OrderId{1}, kSpy, Side::Sell, Qty{100}, Price{530}, decided()).has_value());

    auto sl = Order::stop_limit(OrderId{1}, kSpy, Side::Sell, Qty{100}, Price{530}, Price{529},
                                decided());
    REQUIRE(sl.has_value());
    REQUIRE(sl->limit_price().has_value());
    REQUIRE(sl->stop_price().has_value());
}

TEST_CASE("marketability uses genuine complements not NaN", "[oms][order][leakage]") {
    // With a NaN sentinel, `touch <= limit` and `touch > limit` would BOTH be
    // false and two equivalent-looking checks would take opposite branches
    // (ADR-0002). With optional they are true complements.
    auto buy = Order::limit(OrderId{1}, kSpy, Side::Buy, Qty{100}, Price{540}, decided());
    REQUIRE(buy->is_marketable_against(Price{539.99}));
    REQUIRE(buy->is_marketable_against(Price{540.0}));
    REQUIRE_FALSE(buy->is_marketable_against(Price{540.01}));

    auto sell = Order::limit(OrderId{2}, kSpy, Side::Sell, Qty{100}, Price{540}, decided());
    REQUIRE(sell->is_marketable_against(Price{540.01}));
    REQUIRE_FALSE(sell->is_marketable_against(Price{539.99}));

    // A market order is always marketable.
    auto mkt = Order::market(OrderId{3}, kSpy, Side::Buy, Qty{100}, decided());
    REQUIRE(mkt->is_marketable_against(Price{1e9}));
}

TEST_CASE("orders are rejected without a decision time", "[oms][order][leakage]") {
    // Without a decision anchor nothing downstream could check that the fill
    // came after it, and implementation shortfall would have no benchmark.
    LifecycleTimes empty;
    REQUIRE_FALSE(Order::market(OrderId{1}, kSpy, Side::Buy, Qty{100}, empty).has_value());
}

TEST_CASE("an order cannot arrive at or before its own decision", "[oms][order][leakage]") {
    // THE NO-SAME-BAR RULE, enforced on the order itself.
    auto o = Order::market(OrderId{1}, kSpy, Side::Buy, Qty{100}, decided());
    REQUIRE_FALSE(o->with_arrival(o->decision_time()).has_value());
    REQUIRE_FALSE(o->with_arrival(o->decision_time() - seconds{1}).has_value());
    REQUIRE(o->with_arrival(o->decision_time() + nanoseconds{1}).has_value());
}

TEST_CASE("orders are immutable under modification", "[oms][order]") {
    auto o = Order::market(OrderId{1}, kSpy, Side::Buy, Qty{100}, decided());
    const Order original = *o;
    const Order priced = original.with_arrival_price(Price{544.0});

    REQUIRE(priced.arrival_price().get() == Catch::Approx(544.0));
    // The original is untouched: what we journal and diff never mutates.
    REQUIRE(original.arrival_price().get() == 0.0);
}

TEST_CASE("invalid quantities are refused", "[oms][order][validation]") {
    REQUIRE_FALSE(Order::market(OrderId{1}, kSpy, Side::Buy, Qty{0}, decided()).has_value());
    REQUIRE_FALSE(Order::market(OrderId{1}, kSpy, Side::Buy, Qty{-5}, decided()).has_value());
    REQUIRE_FALSE(Order::market(kNoOrder, kSpy, Side::Buy, Qty{100}, decided()).has_value());
    REQUIRE_FALSE(
        Order::market(OrderId{1}, kInvalidInstrument, Side::Buy, Qty{100}, decided()).has_value());
}

// ---------------------------------------------------------------------------
// State machine
// ---------------------------------------------------------------------------

TEST_CASE("terminal states are terminal", "[oms][state]") {
    // Without this, a late fill arriving after a cancel would quietly reopen
    // the order and double the position.
    for (const auto terminal :
         {OrderState::Filled, OrderState::Cancelled, OrderState::Rejected, OrderState::Expired}) {
        REQUIRE(is_terminal(terminal));
        for (const auto to : {OrderState::New, OrderState::Working, OrderState::Filled,
                              OrderState::PartiallyFilled}) {
            INFO(to_string(terminal) << " -> " << to_string(to));
            REQUIRE_FALSE(is_legal_transition(terminal, to));
        }
    }
}

TEST_CASE("the normal lifecycle is legal end to end", "[oms][state]") {
    REQUIRE(is_legal_transition(OrderState::New, OrderState::PendingNew));
    REQUIRE(is_legal_transition(OrderState::PendingNew, OrderState::Working));
    REQUIRE(is_legal_transition(OrderState::Working, OrderState::PartiallyFilled));
    REQUIRE(is_legal_transition(OrderState::PartiallyFilled, OrderState::Filled));
    // A fill can still land while a cancel is in flight -- that race is real at
    // a venue, and modelling it is what makes cancel latency honest.
    REQUIRE(is_legal_transition(OrderState::PendingCancel, OrderState::Filled));
    REQUIRE_FALSE(is_legal_transition(OrderState::Working, OrderState::New));
    REQUIRE_FALSE(is_legal_transition(OrderState::Working, OrderState::Working));
}

TEST_CASE("the order manager rejects duplicates and unknown parents", "[oms][manager]") {
    OrderManager mgr;
    auto o = Order::market(mgr.next_id(), kSpy, Side::Buy, Qty{100}, decided());
    REQUIRE(mgr.submit(*o).has_value());
    REQUIRE_FALSE(mgr.submit(*o).has_value());  // duplicate id

    auto orphan = Order::market(OrderId{999}, kSpy, Side::Buy, Qty{10}, decided(), TimeInForce::Day,
                                OrderId{12345});
    // A child with an unknown parent would make parent-level aggregation
    // silently incomplete.
    REQUIRE_FALSE(mgr.submit(*orphan).has_value());
}

TEST_CASE("ids are monotonic and never collide with supplied ones", "[oms][manager]") {
    OrderManager mgr;
    auto supplied = Order::market(OrderId{100}, kSpy, Side::Buy, Qty{1}, decided());
    REQUIRE(mgr.submit(*supplied).has_value());
    // The counter jumps ahead so a later next_id() cannot collide.
    REQUIRE(value_of(mgr.next_id()) > 100);
}

TEST_CASE("cancel/replace cancels before submitting", "[oms][manager]") {
    // Doing it the other way round leaves a window in which both orders are
    // live, which at a venue is a double position.
    OrderManager mgr;
    auto original = Order::limit(mgr.next_id(), kSpy, Side::Buy, Qty{100}, Price{540}, decided());
    REQUIRE(mgr.submit(*original).has_value());
    // New -> Working directly is ILLEGAL: an order must be acknowledged first.
    REQUIRE_FALSE(mgr.transition(original->id(), OrderState::Working).has_value());
    REQUIRE(mgr.transition(original->id(), OrderState::PendingNew).has_value());
    REQUIRE(mgr.transition(original->id(), OrderState::Working).has_value());

    auto replacement = Order::limit(mgr.next_id(), kSpy, Side::Buy, Qty{50}, Price{541}, decided());
    auto newid = mgr.cancel_replace(original->id(), *replacement);
    REQUIRE(newid.has_value());
    REQUIRE(mgr.find(original->id())->state == OrderState::Cancelled);
    REQUIRE(mgr.find(*newid) != nullptr);

    // Replacing a terminal order, or changing side, is refused.
    REQUIRE_FALSE(mgr.cancel_replace(original->id(), *replacement).has_value());
    auto wrong_side = Order::limit(mgr.next_id(), kSpy, Side::Sell, Qty{50}, Price{541}, decided());
    REQUIRE_FALSE(mgr.cancel_replace(*newid, *wrong_side).has_value());
}

TEST_CASE("working exposure counts in-flight orders", "[oms][manager][risk]") {
    // Without this, a burst of orders each individually inside a position limit
    // could collectively breach it.
    OrderManager mgr;
    for (int i = 0; i < 3; ++i) {
        auto o = Order::market(mgr.next_id(), kSpy, Side::Buy, Qty{100}, decided());
        REQUIRE(mgr.submit(*o).has_value());
        REQUIRE(mgr.transition(o->id(), OrderState::PendingNew).has_value());
        REQUIRE(mgr.transition(o->id(), OrderState::Working).has_value());
    }
    REQUIRE(mgr.exposure_of(kSpy).get() == Catch::Approx(300.0));

    auto sell = Order::market(mgr.next_id(), kSpy, Side::Sell, Qty{50}, decided());
    REQUIRE(mgr.submit(*sell).has_value());
    REQUIRE(mgr.exposure_of(kSpy).get() == Catch::Approx(250.0));
    REQUIRE(mgr.working().size() == 4);
}
