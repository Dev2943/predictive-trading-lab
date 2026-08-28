#include <algorithm>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <iterator>
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

namespace schema_probe {
template <class T>
concept HasSessionBarCount = requires(T s) { s.tradable_bars_per_session(); };
}  // namespace schema_probe

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
    static_assert(!schema_probe::HasSessionBarCount<SessionSection>,
                  "session length must come from the calendar, not from config");
}

TEST_CASE("missing config file is reported not silently defaulted", "[config]") {
    auto r = load("/nonexistent/path/to/config.toml");
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code == ErrorCode::NotFound);
}

TEST_CASE("session times are parsed at load not carried as strings", "[config][validation]") {
    // Review finding M-2. Downstream must never re-parse these, because a
    // second parser is a second chance to disagree.
    const Config c = must_load(kMinimal);
    REQUIRE(c.session.open_offset == std::chrono::hours{9} + std::chrono::minutes{30});
    REQUIRE(c.session.close_offset == std::chrono::hours{16});
}

TEST_CASE("malformed session times are rejected at load", "[config][validation]") {
    REQUIRE_FALSE(
        load_from_string(kMinimal, Ov{"market_session.regular_open=9:30"}, "<t>").has_value());
    REQUIRE_FALSE(
        load_from_string(kMinimal, Ov{"market_session.regular_close=25:00:00"}, "<t>").has_value());

    // A close at or before the open is incoherent regardless of parseability.
    auto inverted = load_from_string(
        kMinimal,
        Ov{"market_session.regular_open=16:00:00", "market_session.regular_close=09:30:00"}, "<t>");
    REQUIRE_FALSE(inverted.has_value());
    REQUIRE(inverted.error().message.find("after regular_open") != std::string::npos);
}

TEST_CASE("the holdout boundary is parsed and validated at load", "[config][validation]") {
    const Config c = must_load(kMinimal);
    REQUIRE(is_set(c.holdout.boundary));
    REQUIRE(to_date_string(c.holdout.boundary) == "2024-09-01");

    // The single most consequential string in the file. A typo surviving to
    // Phase 5 would define a holdout that does not exist.
    auto bad = load_from_string(kMinimal, Ov{"holdout.boundary_date=2024-13-01"}, "<t>");
    REQUIRE_FALSE(bad.has_value());
    REQUIRE(bad.error().message.find("YYYY-MM-DD") != std::string::npos);

    REQUIRE_FALSE(
        load_from_string(kMinimal, Ov{"holdout.boundary_date=2024-09-01T00:00:00Z"}, "<t>")
            .has_value());
    REQUIRE_FALSE(
        load_from_string(kMinimal, Ov{"holdout.boundary_date=september"}, "<t>").has_value());

    // Empty is legal: the boundary is filled in at Phase 2 ingest, once the
    // data range is known.
    auto empty = load_from_string(kMinimal, Ov{"holdout.boundary_date="}, "<t>");
    REQUIRE(empty.has_value());
    REQUIRE_FALSE(is_set(empty->holdout.boundary));
}

TEST_CASE("the schema is composed from the section declarations", "[config][schema]") {
    // Review finding M-1. The loader owns no key list of its own; it composes
    // what each section declares beside the struct it populates.
    const auto& keys = all_known_keys();

    // 4 run + 4 data + 2 databento + 5 session + 4 holdout + 4 log
    //   + 3 tiers x 6 suffixes = 41
    REQUIRE(keys.size() == 41);

    const auto has = [&keys](std::string_view k) {
        return std::find(keys.begin(), keys.end(), k) != keys.end();
    };
    REQUIRE(has("run.seed"));
    REQUIRE(has("holdout.boundary_date"));
    REQUIRE(has("market_session.regular_open"));
    // Tier keys are composed, not listed three times.
    REQUIRE(has("data.t1.timestamp_semantics"));
    REQUIRE(has("data.t2.schema"));
    REQUIRE(has("data.t3.historical_end_delay_minutes"));
}

TEST_CASE("no key is declared twice", "[config][schema]") {
    // A duplicate would mean two sections believe they own the same setting,
    // and whichever parses last silently wins.
    auto keys = all_known_keys();
    std::sort(keys.begin(), keys.end());
    REQUIRE(std::adjacent_find(keys.begin(), keys.end()) == keys.end());
}

TEST_CASE("the declared schema and the keys the loader reads are identical", "[config][schema]") {
    // THE DRIFT GUARD, and the reason colocation is sufficient without a config
    // DSL.
    //
    // Colocating declarations beside their structs makes the two edits
    // adjacent; it does not make them one. Two failure modes remain, and both
    // are silent:
    //
    //   declared but never read -> the loader accepts the key and ignores it.
    //       Setting it does nothing, and no error is reported. This is exactly
    //       the failure the strict loader exists to prevent, reappearing one
    //       level up.
    //   read but never declared -> a config that sets it is rejected as
    //       "unrecognised", for a key the loader plainly supports.
    //
    // Comparing the two sets catches both. An earlier version of this test only
    // checked that declared keys were not rejected, which caught neither.
    std::vector<std::string> accessed;
    auto r = load_from_string(kMinimal, Ov{}, "<schema>", &accessed);
    REQUIRE(r.has_value());

    std::vector<std::string> declared;
    for (const auto& k : all_known_keys()) declared.emplace_back(k);

    std::sort(declared.begin(), declared.end());
    std::sort(accessed.begin(), accessed.end());
    accessed.erase(std::unique(accessed.begin(), accessed.end()), accessed.end());

    std::vector<std::string> declared_not_read;
    std::set_difference(declared.begin(), declared.end(), accessed.begin(), accessed.end(),
                        std::back_inserter(declared_not_read));
    std::vector<std::string> read_not_declared;
    std::set_difference(accessed.begin(), accessed.end(), declared.begin(), declared.end(),
                        std::back_inserter(read_not_declared));

    for (const auto& k : declared_not_read) {
        INFO("declared in the schema but never read by the loader: " << k);
        REQUIRE(declared_not_read.empty());
    }
    for (const auto& k : read_not_declared) {
        INFO("read by the loader but absent from the schema: " << k);
        REQUIRE(read_not_declared.empty());
    }
    REQUIRE(accessed.size() == declared.size());
}
