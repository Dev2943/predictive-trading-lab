#include <catch2/catch_test_macros.hpp>
#include <limits>

#include "ptl/core/types.hpp"
#include "support/ptl_catch.hpp"

using namespace ptl;

TEST_CASE("timestamp parsing accepts the documented forms", "[core][time]") {
    Timestamp ts{};

    REQUIRE(parse_timestamp("2024-01-02", ts));
    REQUIRE(to_iso8601(ts) == "2024-01-02T00:00:00.000000000Z");

    REQUIRE(parse_timestamp("2024-01-02T14:52:00Z", ts));
    REQUIRE(to_iso8601(ts) == "2024-01-02T14:52:00.000000000Z");

    REQUIRE(parse_timestamp("2024-01-02 14:52:00", ts));  // space instead of T
    REQUIRE(to_date_string(ts) == "2024-01-02");

    REQUIRE(parse_timestamp("2024-01-02T14:52:00.123456789Z", ts));
    REQUIRE(to_iso8601(ts) == "2024-01-02T14:52:00.123456789Z");

    // Fractional digits shorter than nanosecond precision must scale, not
    // truncate: ".5" is half a second, not 5 nanoseconds. Getting this wrong
    // would silently reorder events inside a millisecond.
    REQUIRE(parse_timestamp("2024-01-02T14:52:00.5Z", ts));
    REQUIRE(to_iso8601(ts) == "2024-01-02T14:52:00.500000000Z");
}

TEST_CASE("timestamp parsing rejects malformed input", "[core][time]") {
    Timestamp ts{};
    REQUIRE_FALSE(parse_timestamp("", ts));
    REQUIRE_FALSE(parse_timestamp("2024-01", ts));
    REQUIRE_FALSE(parse_timestamp("2024/01/02", ts));
    REQUIRE_FALSE(parse_timestamp("2024-13-02", ts));  // month 13
    REQUIRE_FALSE(parse_timestamp("2024-02-30", ts));  // not a real date
    REQUIRE_FALSE(parse_timestamp("2024-01-02X14:52:00", ts));
    REQUIRE_FALSE(parse_timestamp("2024-01-02T25:00:00", ts));
    // Leap seconds have no representation in sys_time. Rejecting is honest;
    // rolling silently to the next minute would corrupt event ordering.
    REQUIRE_FALSE(parse_timestamp("2024-01-02T23:59:60", ts));
}

TEST_CASE("timestamp round-trips through parse and format", "[core][time]") {
    for (const auto* text : {"2016-02-29T00:00:00.000000001Z", "2024-07-04T20:00:00.999999999Z",
                             "1999-12-31T23:59:59.000000000Z"}) {
        Timestamp ts{};
        REQUIRE(parse_timestamp(text, ts));
        REQUIRE(to_iso8601(ts) == text);
    }
}

TEST_CASE("utc_date_floor truncates to midnight", "[core][time]") {
    Timestamp ts{};
    REQUIRE(parse_timestamp("2024-01-02T14:52:33.5Z", ts));
    REQUIRE(to_iso8601(utc_date_floor(ts)) == "2024-01-02T00:00:00.000000000Z");
}

TEST_CASE("participation returns zero for a zero-volume interval", "[core][types][edge]") {
    // Review finding H-6. Zero-volume minutes are real for XLE and TLT near the
    // open and in quiet afternoons. Returning inf would flow into a
    // participation cap, then a fill quantity, then a P&L number -- and one
    // non-finite value makes an entire Sharpe ratio NaN, hundreds of lines away
    // from its cause.
    STATIC_REQUIRE(participation(Qty{100.0}, Volume{0.0}) == 0.0);
    REQUIRE(is_finite(participation(Qty{100.0}, Volume{0.0})));

    // Semantics: 0 means "no participation measurable", NOT "unlimited".
    // A cap check must treat this as no liquidity available.
    STATIC_REQUIRE(participation(Qty{500.0}, Volume{10000.0}) == 0.05);
}

TEST_CASE("to_bps against a zero reference does not produce infinity", "[core][types][edge]") {
    STATIC_REQUIRE(to_bps(Price{100.0}, Price{0.0}).get() == 0.0);
    REQUIRE(is_finite(to_bps(Price{100.0}, Price{0.0}).get()));
}

TEST_CASE("is_finite rejects both infinities and NaN", "[core][types][edge]") {
    REQUIRE(is_finite(0.0));
    REQUIRE(is_finite(-1e300));
    REQUIRE_FALSE(is_finite(std::numeric_limits<double>::infinity()));
    REQUIRE_FALSE(is_finite(-std::numeric_limits<double>::infinity()));
    REQUIRE_FALSE(is_finite(std::numeric_limits<double>::quiet_NaN()));
}
