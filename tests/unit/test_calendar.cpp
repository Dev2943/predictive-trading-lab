#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <string>

#include "ptl/market/calendar.hpp"
#include "support/ptl_catch.hpp"

using namespace ptl;
using namespace ptl::market;
using namespace std::chrono;

namespace {

Timestamp date(const char* iso) {
    Timestamp ts{};
    REQUIRE(parse_date(iso, ts));
    return ts;
}

const Calendar& us() {
    static const Calendar cal = [] {
        auto r = Calendar::build(Calendar::us_equities_spec(), 2016, 2026);
        REQUIRE(r.has_value());
        return std::move(*r);
    }();
    return cal;
}

/// Was the market open on this exchange date?
bool trading(const char* iso) {
    return us().is_trading_day(date(iso));
}

SessionKind kind_of(const char* iso) {
    const auto s = us().session_on(date(iso));
    REQUIRE(s.has_value());
    return s->kind;
}

}  // namespace

// ---------------------------------------------------------------------------
// Rule primitives
// ---------------------------------------------------------------------------

TEST_CASE("Easter is computed correctly across many years", "[market][calendar]") {
    // A wrong Easter silently deletes or invents a trading day every single
    // year, so it gets its own test rather than being trusted inside the
    // generator. Reference dates are the well-known Gregorian Easter sequence.
    struct Case {
        int year;
        const char* easter;
    };
    const Case cases[] = {
        {2016, "2016-03-27"}, {2017, "2017-04-16"}, {2018, "2018-04-01"}, {2019, "2019-04-21"},
        {2020, "2020-04-12"}, {2021, "2021-04-04"}, {2022, "2022-04-17"}, {2023, "2023-04-09"},
        {2024, "2024-03-31"}, {2025, "2025-04-20"}, {2026, "2026-04-05"},
    };
    for (const auto& c : cases) {
        INFO("year " << c.year);
        REQUIRE(easter_sunday(c.year) == date(c.easter));
    }
}

TEST_CASE("nth_weekday finds forward and backward occurrences", "[market][calendar]") {
    REQUIRE(nth_weekday(2024, 1, 1, 3) == date("2024-01-15"));   // MLK 2024
    REQUIRE(nth_weekday(2024, 2, 1, 3) == date("2024-02-19"));   // Presidents 2024
    REQUIRE(nth_weekday(2024, 5, 1, -1) == date("2024-05-27"));  // last Monday in May
    REQUIRE(nth_weekday(2024, 9, 1, 1) == date("2024-09-02"));   // Labor 2024
    REQUIRE(nth_weekday(2024, 11, 4, 4) == date("2024-11-28"));  // Thanksgiving 2024
    REQUIRE(nth_weekday(2026, 11, 4, 4) == date("2026-11-26"));
}

TEST_CASE("holidays observe the weekend-shift rule", "[market][calendar]") {
    REQUIRE(observed_holiday(date("2021-07-04")) == date("2021-07-05"));  // Sun -> Mon
    REQUIRE(observed_holiday(date("2020-07-04")) == date("2020-07-03"));  // Sat -> Fri
    REQUIRE(observed_holiday(date("2024-07-04")) == date("2024-07-04"));  // Thu, unchanged
}

// ---------------------------------------------------------------------------
// Holidays
// ---------------------------------------------------------------------------

TEST_CASE("weekends are closed", "[market][calendar]") {
    REQUIRE_FALSE(trading("2024-01-06"));  // Saturday
    REQUIRE_FALSE(trading("2024-01-07"));  // Sunday
    REQUIRE(trading("2024-01-05"));        // Friday
}

TEST_CASE("the standard holiday set is closed", "[market][calendar]") {
    const char* closed_2024[] = {
        "2024-01-01",  // New Year
        "2024-01-15",  // MLK
        "2024-02-19",  // Presidents
        "2024-03-29",  // Good Friday
        "2024-05-27",  // Memorial
        "2024-06-19",  // Juneteenth
        "2024-07-04",  // Independence
        "2024-09-02",  // Labor
        "2024-11-28",  // Thanksgiving
        "2024-12-25",  // Christmas
    };
    for (const auto* d : closed_2024) {
        INFO(d);
        REQUIRE_FALSE(trading(d));
    }
}

TEST_CASE("Juneteenth is a holiday only from 2022", "[market][calendar]") {
    // Became a federal holiday in 2021; NYSE first observed it in 2022. Getting
    // this wrong invents a missing session for every year before that.
    REQUIRE(trading("2021-06-18"));        // Friday, market open
    REQUIRE_FALSE(trading("2022-06-20"));  // observed Monday
    REQUIRE_FALSE(trading("2024-06-19"));
}

TEST_CASE("a New Year holiday observed in December closes that December", "[market][calendar]") {
    // 2022-01-01 was a Saturday, so the holiday was observed Friday
    // 2021-12-31. A generator that only looks at holidays whose actual date
    // falls in the year leaves a phantom trading day on the last session.
    REQUIRE_FALSE(trading("2021-12-31"));
    REQUIRE(trading("2021-12-30"));
}

TEST_CASE("ad-hoc closures from the exception table are honoured", "[market][calendar]") {
    // Rules cannot predict a national day of mourning.
    REQUIRE_FALSE(trading("2018-12-05"));  // G.H.W. Bush
    REQUIRE(trading("2018-12-04"));
    REQUIRE_FALSE(trading("2025-01-09"));  // J. Carter
}

// ---------------------------------------------------------------------------
// Half days -- the case the deleted config helper got wrong
// ---------------------------------------------------------------------------

TEST_CASE("the Friday after Thanksgiving is a half day", "[market][calendar]") {
    REQUIRE(kind_of("2024-11-29") == SessionKind::HalfDay);
    REQUIRE(kind_of("2023-11-24") == SessionKind::HalfDay);
    REQUIRE(kind_of("2026-11-27") == SessionKind::HalfDay);
}

TEST_CASE("July 3 is a half day only when it and July 4 are both weekdays", "[market][calendar]") {
    REQUIRE(kind_of("2024-07-03") == SessionKind::HalfDay);  // Wed, Jul 4 Thu
    REQUIRE(kind_of("2023-07-03") == SessionKind::HalfDay);  // Mon, Jul 4 Tue

    // 2021-07-04 was a Sunday, observed Monday the 5th. July 3 was a Saturday,
    // so there is no session at all -- not a half day.
    REQUIRE(kind_of("2021-07-03") == SessionKind::Closed);
    REQUIRE_FALSE(trading("2021-07-05"));

    // 2020-07-04 was a Saturday, observed Friday July 3: a full closure, and
    // the early close is absorbed by the holiday.
    REQUIRE(kind_of("2020-07-03") == SessionKind::Closed);
}

TEST_CASE("Christmas Eve is a half day unless it is the observed holiday", "[market][calendar]") {
    REQUIRE(kind_of("2024-12-24") == SessionKind::HalfDay);  // Tue

    // 2021-12-25 was a Saturday, observed Friday the 24th: full closure, not a
    // half day.
    REQUIRE(kind_of("2021-12-24") == SessionKind::Closed);

    // 2022-12-24 was a Saturday: no session at all.
    REQUIRE(kind_of("2022-12-24") == SessionKind::Closed);
}

TEST_CASE("half days are 210 minutes and regular days 390", "[market][calendar][leakage]") {
    // THE REGRESSION FOR REVIEW FINDING H-4. A hardcoded 390 reaches across the
    // overnight gap on every half day and produces plausible numbers.
    const auto regular = us().session_on(date("2024-07-02"));
    const auto half = us().session_on(date("2024-07-03"));
    REQUIRE(regular.has_value());
    REQUIRE(half.has_value());

    REQUIRE(regular->length() == hours{6} + minutes{30});
    REQUIRE(half->length() == hours{3} + minutes{30});
    REQUIRE(regular->bar_count(minutes{1}) == 390);
    REQUIRE(half->bar_count(minutes{1}) == 210);
    REQUIRE(regular->bar_count(minutes{5}) == 78);
}

// ---------------------------------------------------------------------------
// UTC boundaries and DST
// ---------------------------------------------------------------------------

TEST_CASE("session boundaries are UTC instants that shift with DST",
          "[market][calendar][determinism]") {
    // 09:30 ET is 14:30Z in summer and 13:30Z... no: EDT is UTC-4, so 09:30 EDT
    // is 13:30Z; EST is UTC-5, so 09:30 EST is 14:30Z. A system that hardcodes
    // either is wrong for half the year.
    const auto summer = us().session_on(date("2024-07-02"));
    REQUIRE(to_iso8601(summer->open) == "2024-07-02T13:30:00.000000000Z");
    REQUIRE(to_iso8601(summer->close) == "2024-07-02T20:00:00.000000000Z");

    const auto winter = us().session_on(date("2024-01-03"));
    REQUIRE(to_iso8601(winter->open) == "2024-01-03T14:30:00.000000000Z");
    REQUIRE(to_iso8601(winter->close) == "2024-01-03T21:00:00.000000000Z");
}

TEST_CASE("the DST transition weeks resolve correctly", "[market][calendar]") {
    // 2024 DST started Sunday March 10 and ended Sunday November 3.
    REQUIRE(to_iso8601(us().session_on(date("2024-03-08"))->open) ==
            "2024-03-08T14:30:00.000000000Z");  // Friday before: EST
    REQUIRE(to_iso8601(us().session_on(date("2024-03-11"))->open) ==
            "2024-03-11T13:30:00.000000000Z");  // Monday after: EDT
    REQUIRE(to_iso8601(us().session_on(date("2024-11-01"))->open) ==
            "2024-11-01T13:30:00.000000000Z");  // Friday before end: EDT
    REQUIRE(to_iso8601(us().session_on(date("2024-11-04"))->open) ==
            "2024-11-04T14:30:00.000000000Z");  // Monday after: EST
}

TEST_CASE("session containment is half-open", "[market][calendar][leakage]") {
    const auto s = us().session_on(date("2024-07-02"));
    REQUIRE(s->contains(s->open));
    REQUIRE(s->contains(s->close - nanoseconds{1}));
    // The closing instant belongs to the closing auction, which is out of scope.
    REQUIRE_FALSE(s->contains(s->close));
    REQUIRE_FALSE(s->contains(s->open - nanoseconds{1}));
}

TEST_CASE("session_containing resolves a UTC instant to the exchange day", "[market][calendar]") {
    // A UTC date is not an exchange date. 20:30Z on July 2 is after the close;
    // 13:30Z is the open.
    Timestamp ts{};
    REQUIRE(parse_timestamp("2024-07-02T15:00:00Z", ts));
    const auto s = us().session_containing(ts);
    REQUIRE(s.has_value());
    REQUIRE(to_date_string(s->date) == "2024-07-02");

    REQUIRE(parse_timestamp("2024-07-02T20:30:00Z", ts));
    REQUIRE_FALSE(us().session_containing(ts).has_value());
}

// ---------------------------------------------------------------------------
// Navigation and construction guards
// ---------------------------------------------------------------------------

TEST_CASE("next and previous session skip closed days", "[market][calendar]") {
    // Thursday Nov 28 2024 is Thanksgiving; the next open day is the Friday
    // half day, and the one after that is Monday.
    const auto after_wed = us().next_session(date("2024-11-27"));
    REQUIRE(to_date_string(after_wed->date) == "2024-11-29");
    REQUIRE(after_wed->kind == SessionKind::HalfDay);

    const auto after_fri = us().next_session(date("2024-11-29"));
    REQUIRE(to_date_string(after_fri->date) == "2024-12-02");

    const auto before_mon = us().previous_session(date("2024-12-02"));
    REQUIRE(to_date_string(before_mon->date) == "2024-11-29");
}

TEST_CASE("dates outside the generated range are distinguishable from closed days",
          "[market][calendar]") {
    // "The market was shut" and "I have no data for that date" are different
    // answers and must not be conflated: the first is a fact, the second is a
    // gap in our knowledge.
    REQUIRE_FALSE(us().session_on(date("1999-01-04")).has_value());
    const auto closed = us().session_on(date("2024-01-01"));
    REQUIRE(closed.has_value());
    REQUIRE_FALSE(closed->is_open());
}

TEST_CASE("calendar construction rejects incoherent specs", "[market][calendar]") {
    auto spec = Calendar::us_equities_spec();
    REQUIRE_FALSE(Calendar::build(spec, 2024, 2020).has_value());
    // The DST rule implemented is post-2007; applying it earlier would shift
    // boundaries by an hour for weeks at a time, silently.
    REQUIRE_FALSE(Calendar::build(spec, 2000, 2010).has_value());

    spec.regular_close = spec.regular_open;
    REQUIRE_FALSE(Calendar::build(spec, 2024, 2024).has_value());
}

TEST_CASE("every generated open session has coherent boundaries", "[market][calendar][property]") {
    for (const auto& s : us().sessions()) {
        INFO(to_date_string(s.date));
        REQUIRE(s.is_open());
        REQUIRE(s.open < s.close);
        REQUIRE(s.open > s.date);
        REQUIRE(s.length() > Duration::zero());
        REQUIRE((s.kind == SessionKind::Regular ? s.length() == hours{6} + minutes{30}
                                                : s.length() == hours{3} + minutes{30}));
    }
}

TEST_CASE("sessions are strictly chronological", "[market][calendar][determinism]") {
    const auto ss = us().sessions();
    REQUIRE(ss.size() > 2500);
    for (std::size_t i = 1; i < ss.size(); ++i) {
        REQUIRE(ss[i].date > ss[i - 1].date);
        REQUIRE(ss[i].open > ss[i - 1].close);
    }
}
