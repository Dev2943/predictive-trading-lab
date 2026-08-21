#include <catch2/catch_test_macros.hpp>

#include "support/ptl_catch.hpp"

#include "ptl/core/types.hpp"

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
    REQUIRE_FALSE(parse_timestamp("2024-13-02", ts));   // month 13
    REQUIRE_FALSE(parse_timestamp("2024-02-30", ts));   // not a real date
    REQUIRE_FALSE(parse_timestamp("2024-01-02X14:52:00", ts));
    REQUIRE_FALSE(parse_timestamp("2024-01-02T25:00:00", ts));
    // Leap seconds have no representation in sys_time. Rejecting is honest;
    // rolling silently to the next minute would corrupt event ordering.
    REQUIRE_FALSE(parse_timestamp("2024-01-02T23:59:60", ts));
}

TEST_CASE("timestamp round-trips through parse and format", "[core][time]") {
    for (const auto* text : {"2016-02-29T00:00:00.000000001Z",
                             "2024-07-04T20:00:00.999999999Z",
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
