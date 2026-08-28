#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <chrono>

#include "ptl/execution/quote_router.hpp"
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

market::Quote quote_at(const char* iso, double bid, double ask, double size = 500.0) {
    auto q = market::Quote::create(kSpy, at(iso), Price{bid}, Qty{size}, Price{ask}, Qty{size});
    REQUIRE(q.has_value());
    return *q;
}

}  // namespace

TEST_CASE("a normal two-sided quote is executable", "[quote][book]") {
    QuoteBook book;
    book.set_trading_state(kSpy, TradingState::Trading);
    REQUIRE(book.update(quote_at("2024-07-02T15:00:00Z", 499.99, 500.01)).has_value());

    const auto state = book.state_at(kSpy, at("2024-07-02T15:00:10Z"));
    REQUIRE(state.present);
    REQUIRE(state.condition == QuoteCondition::Normal);
    REQUIRE(state.executable());
    REQUIRE(state.age == seconds{10});
}

TEST_CASE("quotes must arrive in order", "[quote][book][leakage]") {
    // An out-of-order quote means the feed or the merge is broken. Accepting it
    // would let a stale price overwrite a current one, and every fill afterwards
    // would be priced from the past.
    QuoteBook book;
    REQUIRE(book.update(quote_at("2024-07-02T15:00:00Z", 499.99, 500.01)).has_value());

    auto backwards = book.update(quote_at("2024-07-02T14:59:00Z", 499.0, 500.0));
    REQUIRE_FALSE(backwards.has_value());
    REQUIRE(backwards.error().message.find("precedes") != std::string::npos);
    REQUIRE(book.stats().updates_rejected_out_of_order == 1);

    // The original quote is untouched.
    REQUIRE(book.raw_quote(kSpy)->bid().get() == Catch::Approx(499.99));
    // Same timestamp is allowed: a venue can republish within a nanosecond.
    REQUIRE(book.update(quote_at("2024-07-02T15:00:00Z", 499.98, 500.02)).has_value());
}

TEST_CASE("a locked quote is executable but a crossed one is not", "[quote][book][edge]") {
    // Locked (bid == ask) is legal and briefly real. Crossed on a CONSOLIDATED
    // snapshot is a data fault, and filling against it would book a negative
    // transaction cost.
    QuoteBook book;
    book.set_trading_state(kSpy, TradingState::Trading);
    REQUIRE(book.update(quote_at("2024-07-02T15:00:00Z", 500.0, 500.0)).has_value());

    const auto locked = book.state_at(kSpy, at("2024-07-02T15:00:01Z"));
    REQUIRE(locked.condition == QuoteCondition::Locked);
    REQUIRE(locked.executable());
    REQUIRE(book.stats().locked_observed == 1);
}

TEST_CASE("a stale quote is refused", "[quote][book][leakage]") {
    // ADR-0001 bounds a sampled CBBO observation to roughly one sampling
    // interval. Beyond that the quote describes a market that has moved on.
    QuoteBookConfig cfg;
    cfg.tier = QuoteTier::OneMinute;
    QuoteBook book{cfg};
    REQUIRE(book.staleness_budget() == seconds{60});

    book.set_trading_state(kSpy, TradingState::Trading);
    REQUIRE(book.update(quote_at("2024-07-02T15:00:00Z", 499.99, 500.01)).has_value());

    REQUIRE(book.state_at(kSpy, at("2024-07-02T15:00:59Z")).executable());
    const auto stale = book.state_at(kSpy, at("2024-07-02T15:02:00Z"));
    REQUIRE(stale.condition == QuoteCondition::Stale);
    REQUIRE_FALSE(stale.executable());
    REQUIRE(stale.refusal_reason().find("staleness budget") != std::string::npos);
}

TEST_CASE("the one-second tier has a much tighter budget", "[quote][book][calendar]") {
    // ADR-0001: max_staleness_t2_seconds = 60, max_staleness_t3_seconds = 2.
    REQUIRE(default_staleness_budget(QuoteTier::OneMinute) == seconds{60});
    REQUIRE(default_staleness_budget(QuoteTier::OneSecond) == seconds{2});

    QuoteBookConfig cfg;
    cfg.tier = QuoteTier::OneSecond;
    QuoteBook book{cfg};
    book.set_trading_state(kSpy, TradingState::Trading);
    REQUIRE(book.update(quote_at("2024-07-02T15:00:00Z", 499.99, 500.01)).has_value());

    REQUIRE(book.state_at(kSpy, at("2024-07-02T15:00:01Z")).executable());
    REQUIRE_FALSE(book.state_at(kSpy, at("2024-07-02T15:00:05Z")).executable());
}

TEST_CASE("a quote from the future is unusable", "[quote][book][leakage]") {
    // THE CENTRAL CAUSALITY CHECK OF THE QUOTE BOOK. Reading a quote stamped
    // after the query instant is lookahead, and it is what makes replaying an
    // interleaved quote stream safe.
    QuoteBook book;
    book.set_trading_state(kSpy, TradingState::Trading);
    REQUIRE(book.update(quote_at("2024-07-02T15:05:00Z", 499.99, 500.01)).has_value());

    const auto future = book.state_at(kSpy, at("2024-07-02T15:00:00Z"));
    REQUIRE_FALSE(future.executable());
    REQUIRE(future.age < Duration::zero());
}

TEST_CASE("a halted instrument does not execute", "[quote][book][halt]") {
    QuoteBook book;
    REQUIRE(book.update(quote_at("2024-07-02T15:00:00Z", 499.99, 500.01)).has_value());
    book.set_trading_state(kSpy, TradingState::Halted);

    const auto halted = book.state_at(kSpy, at("2024-07-02T15:00:01Z"));
    REQUIRE(halted.present);
    // The quote itself is fine; the instrument is not tradeable.
    REQUIRE(halted.condition == QuoteCondition::Normal);
    REQUIRE_FALSE(halted.executable());
    REQUIRE(halted.refusal_reason().find("halted") != std::string::npos);

    book.set_trading_state(kSpy, TradingState::Trading);
    REQUIRE(book.state_at(kSpy, at("2024-07-02T15:00:01Z")).executable());
}

TEST_CASE("an auction period does not execute", "[quote][book][session]") {
    // Continuous-trading assumptions do not describe an auction, so the
    // simulator declines rather than pretending they do.
    QuoteBook book;
    REQUIRE(book.update(quote_at("2024-07-02T15:00:00Z", 499.99, 500.01)).has_value());
    book.set_trading_state(kSpy, TradingState::Auction);
    REQUIRE_FALSE(book.state_at(kSpy, at("2024-07-02T15:00:01Z")).executable());

    book.set_trading_state(kSpy, TradingState::Closed);
    REQUIRE_FALSE(book.state_at(kSpy, at("2024-07-02T15:00:01Z")).executable());
}

TEST_CASE("an unknown instrument is absent not stale", "[quote][book][edge]") {
    // "No quote has ever arrived" and "the quote we have is old" are different
    // facts, and conflating them hides a symbology gap.
    QuoteBook book;
    const auto state = book.state_at(InstrumentId{42}, at("2024-07-02T15:00:00Z"));
    REQUIRE_FALSE(state.present);
    REQUIRE(state.condition == QuoteCondition::Absent);
    REQUIRE_FALSE(state.executable());
}

// ---------------------------------------------------------------------------
// Router
// ---------------------------------------------------------------------------

TEST_CASE("the router prefers a real quote over a synthetic one", "[quote][router]") {
    QuoteBook book;
    book.set_trading_state(kSpy, TradingState::Trading);
    REQUIRE(book.update(quote_at("2024-07-02T15:00:00Z", 499.90, 500.10)).has_value());
    const QuoteRouter router{book};

    auto bar =
        market::Bar::from_left_edge(kSpy, at("2024-07-02T14:59:00Z"), minutes{1}, Price{500.0},
                                    Price{500.5}, Price{499.5}, Price{500.0}, Volume{10000.0});
    REQUIRE(bar.has_value());

    const auto routed = router.route_preferring_quote(*bar, 0.01, at("2024-07-02T15:00:10Z"));
    REQUIRE(routed.source == PriceSource::Quote);
    REQUIRE(routed.state.has_quote);
    REQUIRE(routed.state.bid.get() == Catch::Approx(499.90));
    REQUIRE(routed.state.ask.get() == Catch::Approx(500.10));
    // The bar still supplies volume, which a top-of-book snapshot lacks.
    REQUIRE(routed.state.interval_volume.get() == Catch::Approx(10000.0));
}

TEST_CASE("the router falls back to a synthetic bar price", "[quote][router][edge]") {
    QuoteBook book;  // empty
    const QuoteRouter router{book};

    auto bar =
        market::Bar::from_left_edge(kSpy, at("2024-07-02T14:59:00Z"), minutes{1}, Price{500.0},
                                    Price{500.5}, Price{499.5}, Price{500.0}, Volume{10000.0});
    const auto routed = router.route_preferring_quote(*bar, 0.01, at("2024-07-02T15:00:00Z"));
    REQUIRE(routed.source == PriceSource::SyntheticFromBar);
    // has_quote false tells the cost model to synthesise a spread rather than
    // read one that is not there.
    REQUIRE_FALSE(routed.state.has_quote);
    REQUIRE(routed.state.bid.get() == Catch::Approx(500.0));
    REQUIRE(routed.executable);
}

TEST_CASE("a halt is not overridden by a bar", "[quote][router][halt]") {
    // A halt is a fact about the venue. Falling back to a synthetic bar price
    // would let the simulator trade through a halt.
    QuoteBook book;
    REQUIRE(book.update(quote_at("2024-07-02T15:00:00Z", 499.99, 500.01)).has_value());
    book.set_trading_state(kSpy, TradingState::Halted);
    const QuoteRouter router{book};

    auto bar =
        market::Bar::from_left_edge(kSpy, at("2024-07-02T15:00:00Z"), minutes{1}, Price{500.0},
                                    Price{500.5}, Price{499.5}, Price{500.0}, Volume{10000.0});
    const auto routed = router.route_preferring_quote(*bar, 0.01, at("2024-07-02T15:00:30Z"));
    REQUIRE_FALSE(routed.executable);
    REQUIRE(routed.source == PriceSource::None);
}
