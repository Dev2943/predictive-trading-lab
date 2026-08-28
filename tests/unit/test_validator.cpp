#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <vector>

#include "ptl/market/validator.hpp"
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

MarketEvent bar_at(const char* left_edge, double px, double vol = 1000.0,
                   Duration tf = minutes{1}) {
    auto b = Bar::from_left_edge(kSpy, at(left_edge), tf, Price{px}, Price{px}, Price{px},
                                 Price{px}, Volume{vol});
    REQUIRE(b.has_value());
    return *b;
}

const Calendar& us() {
    static const Calendar cal = [] {
        auto r = Calendar::build(Calendar::us_equities_spec(), 2024, 2024);
        REQUIRE(r.has_value());
        return std::move(*r);
    }();
    return cal;
}

bool has(const ValidationReport& r, IssueCode c) {
    for (const auto& i : r.issues) {
        if (i.code == c) return true;
    }
    return false;
}

}  // namespace

TEST_CASE("a clean stream validates with no issues", "[market][validator]") {
    const std::vector<MarketEvent> events{bar_at("2024-07-02T14:52:00Z", 100.0),
                                          bar_at("2024-07-02T14:53:00Z", 100.1),
                                          bar_at("2024-07-02T14:54:00Z", 100.2)};
    const ValidationReport r = DataValidator{}.validate(events, &us());
    REQUIRE(r.ok());
    REQUIRE(r.issues.empty());
    REQUIRE(r.stats.bars == 3);
    REQUIRE(r.stats.sessions == 1);
}

TEST_CASE("a suspicious price jump is fatal", "[market][validator][leakage]") {
    // Liquid ETFs do not move 20% in a minute. When one appears it is an
    // unadjusted split, and admitting it would put a fictional double-digit
    // return into the series -- which a momentum model would happily learn.
    const std::vector<MarketEvent> events{bar_at("2024-07-02T14:52:00Z", 100.0),
                                          bar_at("2024-07-02T14:53:00Z", 50.0)};
    const ValidationReport r = DataValidator{}.validate(events, &us());
    REQUIRE_FALSE(r.ok());
    REQUIRE(has(r, IssueCode::SuspiciousPriceJump));
    bool mentions_corporate_action = false;
    for (const auto& i : r.issues) {
        if (i.detail.find("split or dividend") != std::string::npos) {
            mentions_corporate_action = true;
        }
    }
    REQUIRE(mentions_corporate_action);
}

TEST_CASE("a gap within a session is reported but an overnight gap is not", "[market][validator]") {
    // THE REASON THIS NEEDS A CALENDAR. Arithmetic alone cannot tell a missing
    // bar from the overnight boundary, and flagging every night would bury the
    // real gaps in noise.
    const std::vector<MarketEvent> gapped{bar_at("2024-07-02T14:52:00Z", 100.0),
                                          bar_at("2024-07-02T14:57:00Z", 100.1)};
    REQUIRE(has(DataValidator{}.validate(gapped, &us()), IssueCode::GapInSession));

    const std::vector<MarketEvent> overnight{bar_at("2024-07-02T19:58:00Z", 100.0),
                                             bar_at("2024-07-03T13:31:00Z", 100.1)};
    const ValidationReport r = DataValidator{}.validate(overnight, &us());
    REQUIRE_FALSE(has(r, IssueCode::GapInSession));
    REQUIRE(r.stats.sessions == 2);
}

TEST_CASE("a bar outside every session is fatal", "[market][validator][leakage]") {
    // Extended-hours data we did not ask for, or a timestamp-convention error.
    // Both put an untradeable price into the return series.
    const std::vector<MarketEvent> events{bar_at("2024-07-02T21:30:00Z", 100.0)};
    const ValidationReport r = DataValidator{}.validate(events, &us());
    REQUIRE_FALSE(r.ok());
    REQUIRE(has(r, IssueCode::OutsideSession));
}

TEST_CASE("a bar on a holiday is fatal", "[market][validator][calendar]") {
    // 2024-07-04 was Independence Day. A bar on it means the vendor served us
    // something we cannot explain.
    const std::vector<MarketEvent> events{bar_at("2024-07-04T14:52:00Z", 100.0)};
    REQUIRE_FALSE(DataValidator{}.validate(events, &us()).ok());
}

TEST_CASE("a duplicated bar close is fatal", "[market][validator]") {
    const std::vector<MarketEvent> events{bar_at("2024-07-02T14:52:00Z", 100.0),
                                          bar_at("2024-07-02T14:52:00Z", 100.0)};
    const ValidationReport r = DataValidator{}.validate(events, &us());
    REQUIRE_FALSE(r.ok());
    REQUIRE(has(r, IssueCode::DuplicateTimestamp));
}

TEST_CASE("a non-monotonic stream is fatal", "[market][validator][leakage]") {
    const std::vector<MarketEvent> events{bar_at("2024-07-02T14:54:00Z", 100.0),
                                          bar_at("2024-07-02T14:52:00Z", 100.0)};
    const ValidationReport r = DataValidator{}.validate(events, &us());
    REQUIRE_FALSE(r.ok());
    REQUIRE(has(r, IssueCode::NonMonotonicTimestamp));
}

TEST_CASE("a wrong timeframe is fatal", "[market][validator]") {
    // A five-minute bar in a one-minute stream would silently change what a
    // "1-period return" means.
    const std::vector<MarketEvent> events{
        bar_at("2024-07-02T14:52:00Z", 100.0, 1000.0, minutes{5})};
    const ValidationReport r = DataValidator{}.validate(events, &us());
    REQUIRE_FALSE(r.ok());
    REQUIRE(has(r, IssueCode::TimeframeMismatch));
}

TEST_CASE("a run of zero-volume bars warns without rejecting them", "[market][validator]") {
    // Zero-volume minutes are real for XLE and TLT. A hundred consecutive ones
    // mean the feed stopped, which is a different thing.
    std::vector<MarketEvent> events;
    Timestamp t = at("2024-07-02T14:00:00Z");
    for (int i = 0; i < 40; ++i) {
        auto b = Bar::from_left_edge(kSpy, t, minutes{1}, Price{100}, Price{100}, Price{100},
                                     Price{100}, Volume{0});
        events.emplace_back(*b);
        t += minutes{1};
    }
    const ValidationReport r = DataValidator{}.validate(events, &us());
    REQUIRE(r.ok());  // warnings only
    REQUIRE(has(r, IssueCode::ZeroVolume));
    REQUIRE(r.stats.zero_volume_bars == 40);
    REQUIRE(r.warning_count() > 0);
}

TEST_CASE("strict mode promotes warnings to fatal", "[market][validator]") {
    // Ingest wants a surprise to stop the pipeline rather than scroll past.
    const std::vector<MarketEvent> gapped{bar_at("2024-07-02T14:52:00Z", 100.0),
                                          bar_at("2024-07-02T14:57:00Z", 100.1)};
    ValidatorConfig strict;
    strict.strict = true;
    REQUIRE(DataValidator{}.validate(gapped, &us()).ok());
    REQUIRE_FALSE(DataValidator{strict}.validate(gapped, &us()).ok());
}

TEST_CASE("a missing calendar is recorded rather than silently assumed", "[market][validator]") {
    // "I could not check" and "I checked and it was fine" are different
    // answers. Conflating them is how an unvalidated dataset gets a clean bill.
    const std::vector<MarketEvent> events{bar_at("2024-07-02T14:52:00Z", 100.0)};
    const ValidationReport r = DataValidator{}.validate(events, nullptr);
    REQUIRE(has(r, IssueCode::UnknownSession));
    bool explains = false;
    for (const auto& i : r.issues) {
        if (i.detail.find("skipped") != std::string::npos) explains = true;
    }
    REQUIRE(explains);
}

TEST_CASE("the report summarises counts and range", "[market][validator]") {
    const std::vector<MarketEvent> events{bar_at("2024-07-02T14:52:00Z", 100.0),
                                          bar_at("2024-07-02T14:53:00Z", 100.1)};
    const ValidationReport r = DataValidator{}.validate(events, &us());
    const std::string s = r.summary();
    REQUIRE(s.find("2 bars") != std::string::npos);
    REQUIRE(s.find("0 fatal") != std::string::npos);
    REQUIRE(r.stats.first == at("2024-07-02T14:53:00Z"));
    REQUIRE(r.stats.last == at("2024-07-02T14:54:00Z"));
}
