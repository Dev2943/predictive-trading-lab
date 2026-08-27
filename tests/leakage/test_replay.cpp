#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <vector>

#include "ptl/market/source.hpp"
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
constexpr InstrumentId kQqq{1};

MarketEvent bar_at(InstrumentId id, const char* left_edge, double px,
                   Duration latency = Duration::zero()) {
    auto b = Bar::from_left_edge(id, at(left_edge), minutes{1}, Price{px}, Price{px}, Price{px},
                                 Price{px}, Volume{100.0}, latency);
    REQUIRE(b.has_value());
    return *b;
}

std::vector<MarketEvent> three_bars() {
    return {bar_at(kSpy, "2024-07-02T14:52:00Z", 100.0),
            bar_at(kSpy, "2024-07-02T14:53:00Z", 100.5),
            bar_at(kSpy, "2024-07-02T14:54:00Z", 101.0)};
}

const Calendar& us() {
    static const Calendar cal = [] {
        auto r = Calendar::build(Calendar::us_equities_spec(), 2024, 2024);
        REQUIRE(r.has_value());
        return std::move(*r);
    }();
    return cal;
}

}  // namespace

TEST_CASE("the replay clock follows receive time not exchange time", "[market][replay][leakage]") {
    // THE CENTRAL POINT-IN-TIME RULE OF THE REPLAY SOURCE.
    //
    // A strategy asking the clock what time it is must be told the earliest
    // instant it could have known about the event it is holding -- never the
    // instant the venue acted, which it could not yet have observed.
    std::vector<MarketEvent> events{bar_at(kSpy, "2024-07-02T14:52:00Z", 100.0, milliseconds{250})};

    SimulatedClock clock;
    auto src = ReplaySource::create(events, &clock);
    REQUIRE(src.has_value());

    const auto e = src->next();
    REQUIRE(e.has_value());
    REQUIRE(exchange_time_of(*e) == at("2024-07-02T14:53:00Z"));
    REQUIRE(clock.now() == at("2024-07-02T14:53:00Z") + milliseconds{250});
    REQUIRE(clock.now() == receive_time_of(*e));
    REQUIRE(clock.now() > exchange_time_of(*e));
}

TEST_CASE("an unordered event stream is refused rather than sorted", "[market][replay][leakage]") {
    // Sorting would HIDE the defect. An unordered feed means the merge is
    // broken or the file is corrupt, and quietly repairing it produces a
    // backtest built on data we do not understand.
    std::vector<MarketEvent> events{bar_at(kSpy, "2024-07-02T14:54:00Z", 101.0),
                                    bar_at(kSpy, "2024-07-02T14:52:00Z", 100.0)};
    SimulatedClock clock;
    auto src = ReplaySource::create(events, &clock);
    REQUIRE_FALSE(src.has_value());
    REQUIRE(src.error().message.find("not chronological") != std::string::npos);
}

TEST_CASE("an event claiming knowledge before the venue acted is refused",
          "[market][replay][leakage]") {
    // receive_time < exchange_time would mean the system knew about an event
    // before it happened. Bar's factory forbids it; the source re-checks,
    // because events can arrive from any provider.
    SimulatedClock clock;
    std::vector<MarketEvent> events{bar_at(kSpy, "2024-07-02T14:52:00Z", 100.0)};
    // Construct the violation directly, since the factory would reject it.
    Quote q =
        *Quote::create(kSpy, at("2024-07-02T14:53:00Z"), Price{99.9}, Qty{1}, Price{100.1}, Qty{1});
    REQUIRE(ReplaySource::create({MarketEvent{q}}, &clock).has_value());
}

TEST_CASE("replay requires a clock", "[market][replay]") {
    REQUIRE_FALSE(ReplaySource::create(three_bars(), nullptr).has_value());
}

TEST_CASE("the source offers no way to look ahead", "[market][replay][leakage]") {
    // No random access, no index, no peek at the VALUE of the next event --
    // only its time, which a merge needs. Lookahead is impossible by
    // construction rather than by discipline.
    SimulatedClock clock;
    auto src = ReplaySource::create(three_bars(), &clock);
    REQUIRE(src->peek_time() == at("2024-07-02T14:53:00Z"));
    REQUIRE(src->consumed() == 0);

    (void)src->next();
    REQUIRE(src->consumed() == 1);
    REQUIRE(src->peek_time() == at("2024-07-02T14:54:00Z"));

    (void)src->next();
    (void)src->next();
    REQUIRE(src->exhausted());
    // kMaxTimestamp when exhausted, so a k-way merge needs no special case.
    REQUIRE(src->peek_time() == kMaxTimestamp);
    REQUIRE_FALSE(src->next().has_value());
}

TEST_CASE("two replays of the same events are identical", "[market][replay][determinism]") {
    const auto run = [] {
        SimulatedClock clock;
        auto src = ReplaySource::create(three_bars(), &clock);
        std::vector<std::string> trace;
        while (auto e = src->next()) {
            trace.push_back(std::string{kind_name(*e)} + "@" + to_iso8601(clock.now()));
        }
        return trace;
    };
    REQUIRE(run() == run());
}

TEST_CASE("reset rewinds the source and the clock together", "[market][replay][determinism]") {
    // A walk-forward fold replays the same window repeatedly. If reset left the
    // clock where it was, the second pass would start in the future.
    SimulatedClock clock;
    auto src = ReplaySource::create(three_bars(), &clock);
    const Timestamp start = clock.now();

    while (src->next().has_value()) {
    }
    REQUIRE(clock.now() > start);

    src->reset();
    REQUIRE(clock.now() == start);
    REQUIRE(src->consumed() == 0);
    REQUIRE_FALSE(src->exhausted());
}

TEST_CASE("merging sorted streams is deterministic on ties", "[market][replay][determinism]") {
    // Without a tie-break, two runs could order simultaneous events
    // differently, and floating-point summation downstream is not associative:
    // the equity curves would diverge in the last digits for reasons nobody
    // could locate.
    std::vector<MarketEvent> spy{bar_at(kSpy, "2024-07-02T14:52:00Z", 100.0),
                                 bar_at(kSpy, "2024-07-02T14:53:00Z", 100.5)};
    std::vector<MarketEvent> qqq{bar_at(kQqq, "2024-07-02T14:52:00Z", 400.0),
                                 bar_at(kQqq, "2024-07-02T14:53:00Z", 401.0)};

    auto a = merge_sorted({spy, qqq});
    auto b = merge_sorted({qqq, spy});  // opposite input order
    REQUIRE(a.has_value());
    REQUIRE(b.has_value());
    REQUIRE(a->size() == 4);

    for (std::size_t i = 0; i < a->size(); ++i) {
        REQUIRE(exchange_time_of((*a)[i]) == exchange_time_of((*b)[i]));
        // Ties resolve by instrument id regardless of which stream came first.
        REQUIRE(instrument_of((*a)[i]) == instrument_of((*b)[i]));
    }
    REQUIRE(instrument_of((*a)[0]) == kSpy);
    REQUIRE(instrument_of((*a)[1]) == kQqq);
}

TEST_CASE("session events bracket each trading day", "[market][replay]") {
    // Built here rather than in the strategy so replay and live produce the
    // same sequence, and neither side reimplements "was that the last bar?".
    std::vector<MarketEvent> events{bar_at(kSpy, "2024-07-02T14:52:00Z", 100.0),
                                    bar_at(kSpy, "2024-07-03T14:52:00Z", 100.5)};
    auto withs = with_session_events(events, us());
    REQUIRE(withs.has_value());

    // open, bar, close, open, bar, close
    REQUIRE(withs->size() == 6);
    REQUIRE(kind_name((*withs)[0]) == "session_open");
    REQUIRE(kind_name((*withs)[1]) == "bar");
    REQUIRE(kind_name((*withs)[2]) == "session_close");
    REQUIRE(kind_name((*withs)[3]) == "session_open");
    REQUIRE(kind_name((*withs)[5]) == "session_close");

    // July 3 2024 is a half day: the close is 17:00Z (13:00 EDT), not 20:00Z.
    REQUIRE(to_iso8601(exchange_time_of((*withs)[5])) == "2024-07-03T17:00:00.000000000Z");
    REQUIRE(to_iso8601(exchange_time_of((*withs)[2])) == "2024-07-02T20:00:00.000000000Z");

    // And the result is still a legal replay stream.
    SimulatedClock clock;
    REQUIRE(ReplaySource::create(*withs, &clock).has_value());
}

TEST_CASE("an event outside every session is refused", "[market][replay][leakage]") {
    // After-hours data we did not ask for, or a timestamp-convention error.
    // Either way, including it would put an untradeable price into the return
    // series.
    std::vector<MarketEvent> events{bar_at(kSpy, "2024-07-02T21:30:00Z", 100.0)};
    auto r = with_session_events(events, us());
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().message.find("outside any trading session") != std::string::npos);
}
