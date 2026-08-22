#include <catch2/catch_test_macros.hpp>
#include <stdexcept>

#include "ptl/core/clock.hpp"
#include "support/ptl_catch.hpp"

using namespace ptl;
using namespace std::chrono;

TEST_CASE("simulated clock advances forward", "[core][clock]") {
    Timestamp t0{};
    REQUIRE(parse_timestamp("2024-01-02T14:52:00Z", t0));

    SimulatedClock clock{t0};
    REQUIRE(clock.now() == t0);

    clock.advance_to(t0 + seconds{60});
    REQUIRE(clock.now() == t0 + seconds{60});

    clock.advance_by(milliseconds{500});
    REQUIRE(clock.now() == t0 + seconds{60} + milliseconds{500});
}

TEST_CASE("advancing to the same instant is allowed", "[core][clock]") {
    // Multiple events can share a timestamp -- two venues printing in the same
    // nanosecond is ordinary. Equal is fine; backwards is not.
    Timestamp t0{};
    REQUIRE(parse_timestamp("2024-01-02T14:52:00Z", t0));
    SimulatedClock clock{t0};
    REQUIRE_NOTHROW(clock.advance_to(t0));
    REQUIRE(clock.now() == t0);
}

TEST_CASE("simulated clock refuses to move backwards", "[core][clock][leakage]") {
    // An out-of-order feed means the merge is broken or the data is unsorted.
    // Either way every point-in-time guarantee downstream is void, so this
    // throws rather than clamping. A backtest that silently processes events
    // out of sequence is worse than one that stops.
    Timestamp t0{};
    REQUIRE(parse_timestamp("2024-01-02T14:52:00Z", t0));

    SimulatedClock clock{t0 + seconds{60}};
    REQUIRE_THROWS_AS(clock.advance_to(t0), std::logic_error);
    REQUIRE_THROWS_AS(clock.advance_to(clock.now() - nanoseconds{1}), std::logic_error);

    // State is unchanged after the throw.
    REQUIRE(clock.now() == t0 + seconds{60});
}

TEST_CASE("wall clock is monotonic enough to be usable", "[core][clock]") {
    const WallClock clock;
    const Timestamp a = clock.now();
    const Timestamp b = clock.now();
    REQUIRE(b >= a);
    REQUIRE(a > Timestamp{});  // not the epoch
}
