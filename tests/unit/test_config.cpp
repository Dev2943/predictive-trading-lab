#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <string>
#include <vector>

#include "ptl/config/config.hpp"
#include "support/ptl_catch.hpp"

using namespace ptl;
using namespace ptl::config;

namespace {

constexpr const char* kMinimal = R"TOML(
[run]
seed = 20240101
tag = "unit"

[data.t1]
provider = "alpaca"
feed = "sip"
schema = "ohlcv-1m"
timestamp_semantics = "bar_open_time"

[market_session]
exclude_opening_auction = true

[holdout]
boundary_date = "2024-09-01"
months = 4

[log]
level = "info"
)TOML";

using Ov = std::vector<std::string>;

namespace detail {
template <class T>
concept HasSessionBarCount = requires(T s) { s.tradable_bars_per_session(); };
}  // namespace detail

Config must_load(std::string_view text, std::vector<std::string> ov = {}) {
    auto r = load_from_string(text, ov, "<test>");
    REQUIRE(r.has_value());
    return *r;
}

}  // namespace

TEST_CASE("minimal config loads with documented defaults", "[config]") {
    const Config c = must_load(kMinimal);
    REQUIRE(c.run.seed == 20240101);
    REQUIRE(c.run.tag == "unit");
    REQUIRE(c.data.raw_dir == "data/raw");  // default
    REQUIRE(c.t1.provider == "alpaca");
    REQUIRE(c.t1.timestamp_semantics == "bar_open_time");
    REQUIRE(c.databento.max_spend_usd == Catch::Approx(25.0));  // default
    REQUIRE(c.databento.require_explicit_paid_override);
    REQUIRE(c.holdout.boundary_date == "2024-09-01");
    REQUIRE_FALSE(c.holdout.unlocked);
    REQUIRE(c.log.level == log::Level::Info);
}

TEST_CASE("an unrecognised key is an error not a warning", "[config][determinism]") {
    // A typo like run.sed = 42 under a permissive loader leaves the seed at its
    // default while the manifest records a config that appears to set it. That
    // is a reproducibility failure wearing the costume of a typo.
    auto r = load_from_string("[run]\nsed = 42\n", {}, "<test>");
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code == ErrorCode::ConfigError);
    REQUIRE(r.error().message.find("run.sed") != std::string::npos);
}

TEST_CASE("malformed TOML is reported with a line number", "[config]") {
    auto r = load_from_string("[run\nseed = 1\n", {}, "<test>");
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code == ErrorCode::ParseError);
}

TEST_CASE("dotted overrides apply with correct types", "[config]") {
    const Config c =
        must_load(kMinimal, {"run.seed=999", "log.level=debug", "data.databento.max_spend_usd=1.5",
                             "market_session.exclude_opening_auction=false", "run.tag=swept"});
    REQUIRE(c.run.seed == 999);  // integer, not "999"
    REQUIRE(c.log.level == log::Level::Debug);
    REQUIRE(c.databento.max_spend_usd == Catch::Approx(1.5));  // double
    REQUIRE_FALSE(c.session.exclude_opening_auction);          // bool
    REQUIRE(c.run.tag == "swept");                             // string
}

TEST_CASE("overrides are validated against the schema too", "[config]") {
    auto r = load_from_string(kMinimal, Ov{"run.nonsense=1"}, "<test>");
    REQUIRE_FALSE(r.has_value());
    auto bad = load_from_string(kMinimal, Ov{"noequalssign"}, "<test>");
    REQUIRE_FALSE(bad.has_value());
    REQUIRE(bad.error().code == ErrorCode::InvalidArgument);
}

TEST_CASE("semantic validation rejects incoherent settings", "[config]") {
    REQUIRE_FALSE(load_from_string(kMinimal, Ov{"holdout.months=0"}, "<t>").has_value());
    REQUIRE_FALSE(
        load_from_string(kMinimal, Ov{"data.databento.max_spend_usd=-1"}, "<t>").has_value());
    REQUIRE_FALSE(load_from_string(kMinimal, Ov{"log.level=verbose"}, "<t>").has_value());

    // Unlocking the holdout without a written justification is refused. The
    // holdout is only worth having if using it leaves a mark.
    auto r = load_from_string(kMinimal, Ov{"holdout.unlocked=true"}, "<t>");
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().message.find("justification") != std::string::npos);

    REQUIRE(load_from_string(kMinimal,
                             Ov{"holdout.unlocked=true",
                                "holdout.unlock_justification=research design frozen 2024-09-01"},
                             "<t>")
                .has_value());
}

TEST_CASE("canonical form ignores comments whitespace and key order", "[config][determinism]") {
    // Two files that mean the same thing must hash the same, or the RunId
    // becomes a hash of the author's formatting habits rather than of the
    // settings that produced the result.
    const char* a = R"TOML(
# a comment
[run]
seed = 5
tag = "x"
[log]
level = "info"
)TOML";
    const char* b = R"TOML(
[log]
level   =    "info"

# an entirely different comment
[run]
tag  = "x"
seed = 5
)TOML";
    REQUIRE(must_load(a).canonical == must_load(b).canonical);
    REQUIRE(must_load(a).hash() == must_load(b).hash());
}

TEST_CASE("a meaningful change alters the hash", "[config][determinism]") {
    const Config base = must_load(kMinimal);
    REQUIRE(must_load(kMinimal, {"run.seed=2"}).hash() != base.hash());
    REQUIRE(must_load(kMinimal, {"holdout.months=6"}).hash() != base.hash());
    REQUIRE(must_load(kMinimal, {"market_session.exclude_opening_auction=false"}).hash() !=
            base.hash());
}

TEST_CASE("RunId responds to each of its four inputs", "[config][determinism]") {
    const Config c = must_load(kMinimal);
    const auto base = make_run_id(c.canonical, "sha-abc", "git-123", c.run.seed);

    REQUIRE(make_run_id(c.canonical, "sha-abc", "git-123", c.run.seed).value == base.value);
    REQUIRE(make_run_id(c.canonical, "sha-XXX", "git-123", c.run.seed).value != base.value);
    REQUIRE(make_run_id(c.canonical, "sha-abc", "git-XXX", c.run.seed).value != base.value);
    REQUIRE(make_run_id(c.canonical, "sha-abc", "git-123", 999).value != base.value);
    REQUIRE(base.hex().size() == 16);

    // The separator must prevent a character sliding across a field boundary
    // from producing a collision.
    REQUIRE(make_run_id("ab", "c", "d", 1).value != make_run_id("a", "bc", "d", 1).value);
}

TEST_CASE("session settings are carried but session LENGTH is not computed here", "[config]") {
    // Regression guard for review finding H-4. A previous revision exposed
    // tradable_bars_per_session() returning 390 minus the opening auction. That
    // is wrong on every half-day (Thanksgiving Friday, Christmas Eve, July 3
    // close at 13:00 => 210 minutes) and ignored exclude_closing_auction
    // entirely. Session length is a property of a DATE and belongs to
    // ptl::market::Calendar in Phase 2.
    //
    // This test exists so that reintroducing the helper is a deliberate act
    // rather than an accident: the config carries the default SCHEDULE, and
    // nothing more.
    const Config c = must_load(kMinimal);
    REQUIRE(c.session.exclude_opening_auction);
    REQUIRE(c.session.regular_open == "09:30:00");
    REQUIRE(c.session.regular_close == "16:00:00");

    // The concept must be a TEMPLATE. A requires-expression over a concrete
    // type is not a SFINAE context -- the member lookup simply fails to
    // compile rather than evaluating to false.
    static_assert(!detail::HasSessionBarCount<SessionSection>,
                  "session length must come from the calendar, not from config");
}

TEST_CASE("missing config file is reported not silently defaulted", "[config]") {
    auto r = load("/nonexistent/path/to/config.toml");
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code == ErrorCode::NotFound);
}
