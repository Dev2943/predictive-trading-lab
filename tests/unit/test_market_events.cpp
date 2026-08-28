#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <limits>

#include "ptl/market/event.hpp"
#include "support/ptl_catch.hpp"

using namespace ptl;
using namespace ptl::market;
using namespace std::chrono;

namespace {

Timestamp at(const char* iso) {
    Timestamp ts{};
    REQUIRE(parse_timestamp(iso, ts));
    return ts;
}

constexpr InstrumentId kSpy{0};

Bar good_bar(const char* left_edge = "2024-07-02T14:52:00Z") {
    auto b = Bar::from_left_edge(kSpy, at(left_edge), minutes{1}, Price{100.0}, Price{101.0},
                                 Price{99.0}, Price{100.5}, Volume{1000.0});
    REQUIRE(b.has_value());
    return *b;
}

}  // namespace

// ---------------------------------------------------------------------------
// The left-edge / right-edge convention
// ---------------------------------------------------------------------------

TEST_CASE("a left-edge bar closes one timeframe after its stamp", "[market][bar][leakage]") {
    // THE HIGHEST-RISK ITEM IN PHASE 2 (ADR-0001 bar timestamp policy).
    //
    // Alpaca stamps a minute bar with the START of its interval. Treating that
    // stamp as a close time is a one-minute lookahead introduced by the data
    // layer itself -- exactly the bug class this project exists to prevent.
    const Bar b = good_bar("2024-07-02T14:52:00Z");
    REQUIRE(to_iso8601(b.open_time()) == "2024-07-02T14:52:00.000000000Z");
    REQUIRE(to_iso8601(b.close_time()) == "2024-07-02T14:53:00.000000000Z");
    REQUIRE(b.timeframe() == minutes{1});
}

TEST_CASE("a bar comes into existence at its close not its open", "[market][bar][leakage]") {
    // exchange_time is the instant the aggregate exists. A system that set it
    // to the open would let a strategy act on a bar a full interval before its
    // contents were determined.
    const Bar b = good_bar("2024-07-02T14:52:00Z");
    REQUIRE(b.time().exchange_time == b.close_time());
    REQUIRE(b.time().exchange_time != b.open_time());
    REQUIRE(b.time().receive_time >= b.time().exchange_time);
}

TEST_CASE("feed latency pushes receive time past exchange time", "[market][bar][leakage]") {
    auto b = Bar::from_left_edge(kSpy, at("2024-07-02T14:52:00Z"), minutes{1}, Price{100.0},
                                 Price{101.0}, Price{99.0}, Price{100.5}, Volume{1000.0},
                                 milliseconds{250});
    REQUIRE(b.has_value());
    REQUIRE(b->time().receive_time - b->time().exchange_time == milliseconds{250});
    REQUIRE(b->time().feed_latency() == milliseconds{250});
}

TEST_CASE("a right-edge bar names its convention rather than subtracting at the call site",
          "[market][bar]") {
    auto b = Bar::from_right_edge(kSpy, at("2024-07-02T14:53:00Z"), minutes{1}, Price{100.0},
                                  Price{101.0}, Price{99.0}, Price{100.5}, Volume{1000.0});
    REQUIRE(b.has_value());
    REQUIRE(b->open_time() == at("2024-07-02T14:52:00Z"));
    REQUIRE(b->close_time() == at("2024-07-02T14:53:00Z"));
    // Both conventions must land on the same bar.
    REQUIRE(b->open_time() == good_bar().open_time());
    REQUIRE(b->close_time() == good_bar().close_time());
}

// ---------------------------------------------------------------------------
// Invalid bars cannot be constructed
// ---------------------------------------------------------------------------

TEST_CASE("malformed bars are rejected at construction", "[market][bar][validation]") {
    const Timestamp t = at("2024-07-02T14:52:00Z");
    const auto make = [&](Price o, Price h, Price l, Price c, Volume v) {
        return Bar::from_left_edge(kSpy, t, minutes{1}, o, h, l, c, v);
    };

    // low above high
    REQUIRE_FALSE(make(Price{100}, Price{99}, Price{101}, Price{100}, Volume{1}).has_value());
    // open outside the range
    REQUIRE_FALSE(make(Price{105}, Price{101}, Price{99}, Price{100}, Volume{1}).has_value());
    // close outside the range
    REQUIRE_FALSE(make(Price{100}, Price{101}, Price{99}, Price{98}, Volume{1}).has_value());
    // non-positive price: not a market condition for any instrument in scope
    REQUIRE_FALSE(make(Price{0}, Price{101}, Price{0}, Price{100}, Volume{1}).has_value());
    REQUIRE_FALSE(make(Price{-1}, Price{101}, Price{-2}, Price{100}, Volume{1}).has_value());
    // negative volume
    REQUIRE_FALSE(make(Price{100}, Price{101}, Price{99}, Price{100}, Volume{-1}).has_value());
    // non-finite: inf and NaN do not throw, they propagate and poison a Sharpe
    const double inf = std::numeric_limits<double>::infinity();
    const double nan = std::numeric_limits<double>::quiet_NaN();
    REQUIRE_FALSE(make(Price{100}, Price{inf}, Price{99}, Price{100}, Volume{1}).has_value());
    REQUIRE_FALSE(make(Price{100}, Price{101}, Price{99}, Price{nan}, Volume{1}).has_value());
    REQUIRE_FALSE(make(Price{100}, Price{101}, Price{99}, Price{100}, Volume{nan}).has_value());
}

TEST_CASE("a bar with no instrument or no timeframe is rejected", "[market][bar][validation]") {
    const Timestamp t = at("2024-07-02T14:52:00Z");
    REQUIRE_FALSE(Bar::from_left_edge(kInvalidInstrument, t, minutes{1}, Price{100}, Price{101},
                                      Price{99}, Price{100}, Volume{1})
                      .has_value());
    REQUIRE_FALSE(Bar::from_left_edge(kSpy, t, Duration::zero(), Price{100}, Price{101}, Price{99},
                                      Price{100}, Volume{1})
                      .has_value());
    REQUIRE_FALSE(Bar::from_left_edge(kSpy, kNoTimestamp, minutes{1}, Price{100}, Price{101},
                                      Price{99}, Price{100}, Volume{1})
                      .has_value());
    // Negative latency would mean receiving an event before the venue sent it.
    REQUIRE_FALSE(Bar::from_left_edge(kSpy, t, minutes{1}, Price{100}, Price{101}, Price{99},
                                      Price{100}, Volume{1}, -milliseconds{1})
                      .has_value());
}

TEST_CASE("zero volume is accepted", "[market][bar][validation]") {
    // Zero-volume minutes are real for XLE and TLT near the open. Rejecting
    // them would delete legitimate data; the VALIDATOR flags long runs of them.
    auto b = Bar::from_left_edge(kSpy, at("2024-07-02T14:52:00Z"), minutes{1}, Price{100},
                                 Price{100}, Price{100}, Price{100}, Volume{0});
    REQUIRE(b.has_value());
}

// ---------------------------------------------------------------------------
// Quote and trade
// ---------------------------------------------------------------------------

TEST_CASE("quote derives mid and spread", "[market][quote]") {
    auto q = Quote::create(kSpy, at("2024-07-02T14:52:00Z"), Price{99.99}, Qty{500}, Price{100.01},
                           Qty{300});
    REQUIRE(q.has_value());
    REQUIRE(q->mid().get() == Catch::Approx(100.0));
    REQUIRE(q->spread().get() == Catch::Approx(0.02));
    REQUIRE(q->spread_bps().get() == Catch::Approx(2.0));
}

TEST_CASE("a crossed quote is rejected", "[market][quote][validation]") {
    // A CONSOLIDATED top-of-book snapshot should never be crossed. A negative
    // spread would flow into a negative transaction cost -- a strategy that
    // appears to be paid for trading.
    REQUIRE_FALSE(
        Quote::create(kSpy, at("2024-07-02T14:52:00Z"), Price{100.05}, Qty{1}, Price{99.95}, Qty{1})
            .has_value());
    // Locked (bid == ask) is unusual but not impossible; it is accepted.
    REQUIRE(
        Quote::create(kSpy, at("2024-07-02T14:52:00Z"), Price{100.0}, Qty{1}, Price{100.0}, Qty{1})
            .has_value());
}

TEST_CASE("quote staleness is signed so future quotes are detectable", "[market][quote][leakage]") {
    auto q = Quote::create(kSpy, at("2024-07-02T14:52:00Z"), Price{99.99}, Qty{1}, Price{100.01},
                           Qty{1});
    REQUIRE(q->staleness_at(at("2024-07-02T14:52:30Z")) == seconds{30});
    // Negative means the snapshot is in the FUTURE relative to the query, which
    // a consumer must treat as unusable: using it is lookahead.
    REQUIRE(q->staleness_at(at("2024-07-02T14:51:30Z")) < Duration::zero());
}

TEST_CASE("trade aggressor is optional rather than invented", "[market][trade][validation]") {
    // Signing a trade needs a contemporaneous quote and a rule. Inventing a
    // side here would be a modelling assumption dressed up as data.
    auto t = Trade::create(kSpy, at("2024-07-02T14:52:00Z"), Price{100.0}, Qty{100});
    REQUIRE(t.has_value());
    REQUIRE_FALSE(t->aggressor().has_value());
    REQUIRE(t->notional_value().get() == Catch::Approx(10000.0));

    auto signed_trade =
        Trade::create(kSpy, at("2024-07-02T14:52:00Z"), Price{100.0}, Qty{100}, Side::Buy);
    REQUIRE(signed_trade->aggressor() == Side::Buy);

    REQUIRE_FALSE(
        Trade::create(kSpy, at("2024-07-02T14:52:00Z"), Price{100.0}, Qty{0}).has_value());
}

TEST_CASE("corporate actions validate their magnitudes", "[market][corpaction]") {
    REQUIRE(CorporateAction::split(kSpy, at("2024-07-02"), 2.0).has_value());
    REQUIRE(CorporateAction::split(kSpy, at("2024-07-02"), 0.1).has_value());  // reverse
    REQUIRE_FALSE(CorporateAction::split(kSpy, at("2024-07-02"), 0.0).has_value());
    REQUIRE_FALSE(CorporateAction::split(kSpy, at("2024-07-02"), -1.0).has_value());

    auto d = CorporateAction::cash_dividend(kSpy, at("2024-06-21"), Notional{1.76});
    REQUIRE(d.has_value());
    REQUIRE(d->kind() == CorporateActionKind::CashDividend);
    REQUIRE_FALSE(CorporateAction::cash_dividend(kSpy, at("2024-06-21"), Notional{0}).has_value());
}

// ---------------------------------------------------------------------------
// The event variant
// ---------------------------------------------------------------------------

TEST_CASE("event accessors work uniformly across the variant", "[market][event]") {
    const MarketEvent bar_ev = good_bar();
    REQUIRE(exchange_time_of(bar_ev) == at("2024-07-02T14:53:00Z"));
    REQUIRE(instrument_of(bar_ev) == kSpy);
    REQUIRE(kind_name(bar_ev) == "bar");

    SessionEvent se;
    se.time.exchange_time = at("2024-07-02T13:30:00Z");
    se.time.receive_time = se.time.exchange_time;
    se.kind = SessionEventKind::Open;
    const MarketEvent sess_ev = se;
    REQUIRE(exchange_time_of(sess_ev) == at("2024-07-02T13:30:00Z"));
    // Session and timer events are not per-symbol.
    REQUIRE(instrument_of(sess_ev) == kInvalidInstrument);
    REQUIRE(kind_name(sess_ev) == "session_open");
}
