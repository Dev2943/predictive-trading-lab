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

market::Quote quote_at(const char* iso, double bid, double ask, double size = 1e6) {
    auto q = market::Quote::create(kSpy, at(iso), Price{bid}, Qty{size}, Price{ask}, Qty{size});
    REQUIRE(q.has_value());
    return *q;
}

/// A venue with costs disabled, so fill prices are exactly the touch.
struct Venue {
    SimulatedClock clock{at("2024-07-02T15:00:00Z")};
    StandardCostModel costs;
    StandardLatencyModel latency;
    BrokerSimulator broker;
    QuoteBook book;
    std::uint64_t next = 0;

    explicit Venue(
        FillConfig fc = FillConfig{}, CostConfig cc =
                                          [] {
                                              CostConfig c;
                                              c.commission_per_share = 0.0;
                                              c.minimum_commission = 0.0;
                                              c.stochastic_slippage_bps = Bps{0.0};
                                              c.impact_coefficient = 0.0;
                                              return c;
                                          }())
        : costs(cc), broker(clock, costs, latency, DeterministicRng{42}, fc) {
        book.set_trading_state(kSpy, TradingState::Trading);
    }

    oms::Order order(Side side, double qty, oms::OrderType type = oms::OrderType::Market,
                     std::optional<double> limit = std::nullopt,
                     std::optional<double> stop = std::nullopt,
                     oms::TimeInForce tif = oms::TimeInForce::Day) {
        LifecycleTimes t;
        t.decision_time = clock.now();
        Result<oms::Order> o = fail(make_error(ErrorCode::Unsupported, "unhandled"));
        switch (type) {
            case oms::OrderType::Market:
                o = oms::Order::market(oms::OrderId{++next}, kSpy, side, Qty{qty}, t, tif);
                break;
            case oms::OrderType::Limit:
                o = oms::Order::limit(oms::OrderId{++next}, kSpy, side, Qty{qty}, Price{*limit}, t,
                                      tif);
                break;
            case oms::OrderType::Stop:
                o = oms::Order::stop(oms::OrderId{++next}, kSpy, side, Qty{qty}, Price{*stop}, t,
                                     tif);
                break;
            case oms::OrderType::StopLimit:
                o = oms::Order::stop_limit(oms::OrderId{++next}, kSpy, side, Qty{qty}, Price{*stop},
                                           Price{*limit}, t, tif);
                break;
        }
        REQUIRE(o.has_value());
        return *o;
    }

    /// Apply a quote and match, after letting latency elapse.
    ///
    /// The clock is advanced TO the quote's own timestamp plus a beat, not by a
    /// fixed increment. A fixed increment leaves the clock behind quotes that
    /// jump a minute, and the book then correctly refuses them as
    /// future-dated -- which is the causality guarantee working, but it makes
    /// the harness rather than the venue the thing under test.
    std::vector<oms::Fill> tick(const market::Quote& q, Duration advance = seconds{1}) {
        REQUIRE(book.update(q).has_value());
        const Timestamp target = q.time().exchange_time + advance;
        if (target > clock.now()) {
            clock.advance_to(target);
        } else {
            clock.advance_by(advance);
        }
        const QuoteRouter router{book};
        const auto routed = router.route(kSpy, clock.now());
        auto fills = broker.on_quote_update(kSpy, routed, clock.now());
        REQUIRE(fills.has_value());
        return *fills;
    }
};

}  // namespace

TEST_CASE("a market buy executes at the ask and a sell at the bid", "[execution][quote][leakage]") {
    // THE NAMED PHASE 8 TEST (test_quote_aware_fill). A taker crosses the
    // spread; a fill better than the touch would be a discount no venue offers.
    Venue v;
    REQUIRE(v.broker.submit(v.order(Side::Buy, 100)).has_value());
    REQUIRE(v.broker.submit(v.order(Side::Sell, 100)).has_value());

    const auto fills = v.tick(quote_at("2024-07-02T15:00:00Z", 499.00, 501.00));
    REQUIRE(fills.size() == 2);
    REQUIRE(fills[0].price().get() == Catch::Approx(501.00));  // buy pays the ask
    REQUIRE(fills[1].price().get() == Catch::Approx(499.00));  // sell hits the bid
    REQUIRE(v.broker.stats().filled_from_quote == 2);
    REQUIRE(v.broker.stats().filled_from_bar == 0);
}

TEST_CASE("a non-marketable limit does not fill", "[execution][quote][leakage]") {
    // THE NAMED PHASE 8 TEST (test_no_fill). ADR-0003: a price being touched
    // says nothing about whether we were ever at the front of a queue we cannot
    // observe.
    Venue v;
    REQUIRE(v.broker.submit(v.order(Side::Buy, 100, oms::OrderType::Limit, 495.0)).has_value());

    REQUIRE(v.tick(quote_at("2024-07-02T15:00:00Z", 499.00, 501.00)).empty());
    REQUIRE(v.broker.stats().passive_no_fill > 0);

    // Once the market comes to it, it fills.
    const auto fills = v.tick(quote_at("2024-07-02T15:01:00Z", 493.0, 494.0));
    REQUIRE(fills.size() == 1);
    REQUIRE(fills[0].price().get() <= 495.0);
}

TEST_CASE("a stale quote produces no fill", "[execution][quote][leakage]") {
    // A quote older than the tier's budget describes a market that has moved
    // on. Filling against it would price a trade from the past.
    Venue v;
    REQUIRE(v.broker.submit(v.order(Side::Buy, 100)).has_value());
    REQUIRE(v.book.update(quote_at("2024-07-02T15:00:00Z", 499.0, 501.0)).has_value());

    // Advance well past the one-minute budget without a new quote.
    v.clock.advance_by(minutes{5});
    const QuoteRouter router{v.book};
    const auto routed = router.route(kSpy, v.clock.now());
    REQUIRE_FALSE(routed.executable);

    auto fills = v.broker.on_quote_update(kSpy, routed, v.clock.now());
    REQUIRE(fills.has_value());
    REQUIRE(fills->empty());
    REQUIRE(v.broker.stats().rejected_stale_quote == 1);
    // The order RESTS: staleness is temporary, and cancelling would silently
    // change the strategy.
    REQUIRE(v.broker.pending_count() == 1);
}

TEST_CASE("a halted instrument produces no fill", "[execution][quote][halt]") {
    Venue v;
    REQUIRE(v.broker.submit(v.order(Side::Buy, 100)).has_value());
    REQUIRE(v.book.update(quote_at("2024-07-02T15:00:00Z", 499.0, 501.0)).has_value());
    v.book.set_trading_state(kSpy, TradingState::Halted);

    v.clock.advance_by(seconds{1});
    const QuoteRouter router{v.book};
    auto fills = v.broker.on_quote_update(kSpy, router.route(kSpy, v.clock.now()), v.clock.now());
    REQUIRE(fills.has_value());
    REQUIRE(fills->empty());
    REQUIRE(v.broker.stats().rejected_halted == 1);
    REQUIRE(v.broker.pending_count() == 1);

    // Resumption fills the resting order.
    v.book.set_trading_state(kSpy, TradingState::Trading);
    REQUIRE(v.tick(quote_at("2024-07-02T15:00:30Z", 499.0, 501.0)).size() == 1);
}

TEST_CASE("a locked market still fills a taker", "[execution][quote][edge]") {
    // bid == ask is legal. The spread is zero, so a cost model must not divide
    // by it, but a marketable order still executes.
    Venue v;
    REQUIRE(v.broker.submit(v.order(Side::Buy, 100)).has_value());
    const auto fills = v.tick(quote_at("2024-07-02T15:00:00Z", 500.0, 500.0));
    REQUIRE(fills.size() == 1);
    REQUIRE(fills[0].price().get() == Catch::Approx(500.0));
    REQUIRE(is_finite(fills[0].price().get()));
}

TEST_CASE("a stop is dormant until it triggers", "[execution][quote][stop]") {
    // A stop that filled on arrival would turn a protective order into an
    // immediate market order -- the opposite of its purpose.
    Venue v;
    REQUIRE(v.broker.submit(v.order(Side::Sell, 100, oms::OrderType::Stop, std::nullopt, 495.0))
                .has_value());

    // Well above the stop: dormant.
    REQUIRE(v.tick(quote_at("2024-07-02T15:00:00Z", 499.0, 501.0)).empty());
    REQUIRE(v.broker.stats().stops_triggered == 0);
    REQUIRE(v.broker.pending_count() == 1);

    // The market falls through the stop.
    const auto fills = v.tick(quote_at("2024-07-02T15:01:00Z", 494.0, 494.5));
    REQUIRE(v.broker.stats().stops_triggered == 1);
    REQUIRE(fills.size() == 1);
    REQUIRE(fills[0].side() == Side::Sell);
}

TEST_CASE("a buy stop triggers on a rise", "[execution][quote][stop]") {
    Venue v;
    REQUIRE(v.broker.submit(v.order(Side::Buy, 100, oms::OrderType::Stop, std::nullopt, 505.0))
                .has_value());
    REQUIRE(v.tick(quote_at("2024-07-02T15:00:00Z", 499.0, 501.0)).empty());
    REQUIRE(v.tick(quote_at("2024-07-02T15:01:00Z", 505.5, 506.0)).size() == 1);
    REQUIRE(v.broker.stats().stops_triggered == 1);
}

TEST_CASE("a stop-limit still respects its limit after triggering", "[execution][quote][stop]") {
    // Triggering makes it live; it does not make it marketable at any price.
    Venue v;
    REQUIRE(v.broker.submit(v.order(Side::Sell, 100, oms::OrderType::StopLimit, 494.0, 495.0))
                .has_value());

    // Falls through the stop, but the bid is below the limit: triggered, no fill.
    REQUIRE(v.tick(quote_at("2024-07-02T15:01:00Z", 490.0, 490.5)).empty());
    REQUIRE(v.broker.stats().stops_triggered == 1);
    REQUIRE(v.broker.pending_count() == 1);

    // The bid recovers above the limit and it fills.
    REQUIRE(v.tick(quote_at("2024-07-02T15:02:00Z", 496.0, 496.5)).size() == 1);
}

TEST_CASE("IOC takes what it can and cancels the rest", "[execution][quote][tif]") {
    FillConfig fc;
    fc.respect_displayed_size = true;
    fc.max_participation_rate = 1.0;
    Venue v{fc};

    REQUIRE(v.broker
                .submit(v.order(Side::Buy, 1000, oms::OrderType::Market, std::nullopt, std::nullopt,
                                oms::TimeInForce::ImmediateOrCancel))
                .has_value());

    const auto fills = v.tick(quote_at("2024-07-02T15:00:00Z", 499.0, 501.0, 300.0));
    REQUIRE(fills.size() == 1);
    REQUIRE(fills[0].quantity().get() == Catch::Approx(300.0));
    // The remainder is cancelled, not left resting.
    REQUIRE(v.broker.pending_count() == 0);
}

TEST_CASE("FOK fills entirely or not at all", "[execution][quote][tif]") {
    // Distinct from IOC. An FOK that cannot be filled in FULL fills nothing;
    // conflating the two understates execution risk on exactly the orders too
    // large for the displayed book.
    FillConfig fc;
    fc.respect_displayed_size = true;
    fc.max_participation_rate = 1.0;
    Venue v{fc};

    REQUIRE(v.broker
                .submit(v.order(Side::Buy, 1000, oms::OrderType::Market, std::nullopt, std::nullopt,
                                oms::TimeInForce::FillOrKill))
                .has_value());

    // Only 300 displayed: the FOK is killed with NO partial fill.
    const auto killed = v.tick(quote_at("2024-07-02T15:00:00Z", 499.0, 501.0, 300.0));
    REQUIRE(killed.empty());
    REQUIRE(v.broker.stats().fok_killed == 1);
    REQUIRE(v.broker.pending_count() == 0);

    // With enough size it fills completely.
    Venue full{fc};
    REQUIRE(full.broker
                .submit(full.order(Side::Buy, 1000, oms::OrderType::Market, std::nullopt,
                                   std::nullopt, oms::TimeInForce::FillOrKill))
                .has_value());
    const auto filled = full.tick(quote_at("2024-07-02T15:00:00Z", 499.0, 501.0, 5000.0));
    REQUIRE(filled.size() == 1);
    REQUIRE(filled[0].quantity().get() == Catch::Approx(1000.0));
}

TEST_CASE("a day order rests across quote updates", "[execution][quote][tif]") {
    FillConfig fc;
    fc.respect_displayed_size = true;
    fc.max_participation_rate = 1.0;
    Venue v{fc};

    REQUIRE(v.broker.submit(v.order(Side::Buy, 1000)).has_value());
    const auto first = v.tick(quote_at("2024-07-02T15:00:00Z", 499.0, 501.0, 400.0));
    REQUIRE(first.size() == 1);
    REQUIRE(first[0].quantity().get() == Catch::Approx(400.0));
    // The remainder RESTS, unlike IOC.
    REQUIRE(v.broker.pending_count() == 1);

    const auto second = v.tick(quote_at("2024-07-02T15:01:00Z", 499.0, 501.0, 600.0));
    REQUIRE(second.size() == 1);
    REQUIRE(second[0].quantity().get() == Catch::Approx(600.0));
    REQUIRE(v.broker.pending_count() == 0);
}

TEST_CASE("displayed size bounds a fill", "[execution][quote][leakage]") {
    // THE NAMED PHASE 8 TEST (test_fill_constraint): filled quantity never
    // exceeds the liquidity the model can actually see.
    FillConfig fc;
    fc.respect_displayed_size = true;
    fc.max_participation_rate = 1.0;
    Venue v{fc};

    REQUIRE(v.broker.submit(v.order(Side::Buy, 10000)).has_value());
    const auto fills = v.tick(quote_at("2024-07-02T15:00:00Z", 499.0, 501.0, 250.0));
    REQUIRE(fills.size() == 1);
    REQUIRE(fills[0].quantity().get() <= 250.0);
    REQUIRE(v.broker.stats().displayed_size_capped > 0);
}

TEST_CASE("an order cannot fill on the quote that created it", "[execution][quote][leakage]") {
    // Latency, preserved from Phase 3. An order submitted at T arrives strictly
    // after T, so matching at T produces nothing.
    Venue v;
    const auto order = v.order(Side::Buy, 100);
    const auto arrival = v.broker.submit(order);
    REQUIRE(arrival.has_value());
    REQUIRE(*arrival > order.decision_time());

    REQUIRE(v.book.update(quote_at("2024-07-02T15:00:00Z", 499.0, 501.0)).has_value());
    const QuoteRouter router{v.book};
    auto same_instant =
        v.broker.on_quote_update(kSpy, router.route(kSpy, v.clock.now()), v.clock.now());
    REQUIRE(same_instant.has_value());
    REQUIRE(same_instant->empty());
}

TEST_CASE("quote-driven execution is deterministic", "[execution][quote][determinism]") {
    const auto run = [] {
        Venue v;
        (void)v.broker.submit(v.order(Side::Buy, 100));
        (void)v.broker.submit(v.order(Side::Sell, 50));
        auto fills = v.tick(quote_at("2024-07-02T15:00:00Z", 499.0, 501.0));
        std::vector<double> prices;
        for (const auto& f : fills) prices.push_back(f.price().get());
        return prices;
    };
    REQUIRE(run() == run());
}
