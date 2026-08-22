#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <string>
#include <type_traits>

#include "ptl/core/time.hpp"
#include "ptl/core/types.hpp"
#include "support/ptl_catch.hpp"

using namespace ptl;

// ---------------------------------------------------------------------------
// Platform assumption guard
// ---------------------------------------------------------------------------
//
// ptl::Timestamp is time_point<system_clock, nanoseconds> EXPLICITLY. It is not
// system_clock::time_point, and that distinction is load-bearing:
//
//     libstdc++ : system_clock::duration == nanoseconds
//     libc++    : system_clock::duration == microseconds
//
// Had we written system_clock::time_point, the engine would silently carry
// nanosecond resolution on Linux and microsecond resolution on macOS. Every
// sub-microsecond latency model, every event ordering inside a millisecond, and
// therefore the byte-identical determinism guarantee, would differ by platform.
static_assert(std::is_same_v<Timestamp::duration, std::chrono::nanoseconds>,
              "ptl::Timestamp must be nanosecond-resolution on every platform");
static_assert(std::is_same_v<Timestamp::clock, std::chrono::system_clock>);
static_assert(Duration::period::den == 1000000000, "ptl::Duration must be nanoseconds");

// ---------------------------------------------------------------------------
// StringMaker selection
// ---------------------------------------------------------------------------

TEST_CASE("our Timestamp StringMaker wins over the Catch2 partial one",
          "[core][pit][portability]") {
    // THE REGRESSION TEST FOR THE macOS BUILD FAILURE.
    //
    // Catch2 provides a partial specialisation for
    // time_point<system_clock, Duration> whose convert() calls to_time_t().
    // On libc++ that fails to compile for a nanosecond time_point, because
    // system_clock::duration is microseconds there and chrono will not narrow
    // implicitly.
    //
    // Our explicit specialisation is more specialised, so it is selected and
    // Catch2's is never instantiated -- on any standard library. Asserting on
    // the *format* of the rendered string is what proves selection: Catch2's
    // version emits a second-resolution date, ours emits ISO-8601 with all nine
    // fractional digits. If this assertion ever fails, macOS builds break.
    Timestamp ts{};
    REQUIRE(parse_timestamp("2024-01-02T14:52:00.123456789Z", ts));

    const std::string rendered = Catch::Detail::stringify(ts);
    REQUIRE(rendered == "2024-01-02T14:52:00.123456789Z");
    REQUIRE(rendered == to_iso8601(ts));
    // Nanoseconds survive. Catch2's default would have discarded them.
    REQUIRE(rendered.find("123456789") != std::string::npos);
}

TEST_CASE("sentinel timestamps render readably", "[core][pit][portability]") {
    REQUIRE(Catch::Detail::stringify(kNoTimestamp) == "<unset>");
    REQUIRE(Catch::Detail::stringify(kMaxTimestamp) == "<max>");
}

TEST_CASE("strong typedefs render with their value", "[core][types][portability]") {
    // A failure message should say which value was wrong, not print an opaque
    // object address or fall back to "{?}".
    REQUIRE(Catch::Detail::stringify(Price{512.25}).find("512.25") != std::string::npos);
    REQUIRE(Catch::Detail::stringify(Qty{100.0}).find("100") != std::string::npos);
}

TEST_CASE("enums render by name rather than by ordinal", "[core][pit][portability]") {
    REQUIRE(Catch::Detail::stringify(Stage::ArrivalTime) == "arrival_time");
    REQUIRE(Catch::Detail::stringify(Stage::DecisionTime) == "decision_time");
    REQUIRE(Catch::Detail::stringify(Side::Buy) == "BUY");
    REQUIRE(Catch::Detail::stringify(ChainRule::StrictlyAfter) == "StrictlyAfter");
    REQUIRE(Catch::Detail::stringify(InstrumentId{7}) == "InstrumentId(7)");
}

TEST_CASE("a failing timestamp comparison would report both instants", "[core][pit][portability]") {
    // Exercises the exact path that broke: comparing two nanosecond
    // time_points inside a Catch2 assertion, which is what forces StringMaker
    // to be instantiated in the first place.
    Timestamp a{};
    Timestamp b{};
    REQUIRE(parse_timestamp("2024-01-02T14:52:00.000000000Z", a));
    REQUIRE(parse_timestamp("2024-01-02T14:52:00.000000001Z", b));
    REQUIRE(a != b);
    REQUIRE(a < b);
    REQUIRE(Catch::Detail::stringify(a) != Catch::Detail::stringify(b));
}
