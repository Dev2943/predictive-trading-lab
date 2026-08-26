#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <string>

#include "ptl/market/provider.hpp"
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

/// A provider that declares whatever the test needs and serves nothing. The
/// gate must reach its verdict without any data being fetched -- that is the
/// definition of fail-fast.
class StubProvider final : public IMarketDataProvider {
public:
    StubProvider(ProviderIdentity id, CapabilitySet caps, Duration delay)
        : id_(std::move(id)), caps_(caps), delay_(delay) {}

    [[nodiscard]] const ProviderIdentity& identity() const noexcept override { return id_; }
    [[nodiscard]] CapabilitySet capabilities() const noexcept override { return caps_; }
    [[nodiscard]] Timestamp available_through(Timestamp now) const noexcept override {
        if (delay_ == Duration::zero()) return kMaxTimestamp;
        return now - delay_;
    }
    [[nodiscard]] Result<std::vector<Bar>> fetch_bars(const BarRequest&) override {
        fetched = true;
        return std::vector<Bar>{};
    }
    [[nodiscard]] Result<std::vector<Quote>> fetch_quotes(const QuoteRequest&) override {
        fetched = true;
        return std::vector<Quote>{};
    }

    mutable bool fetched = false;

private:
    ProviderIdentity id_;
    CapabilitySet caps_;
    Duration delay_;
};

EntitlementRequest research_request() {
    EntitlementRequest r;
    r.required = CapabilitySet{Capability::HistoricalBars, Capability::ConsolidatedFeed};
    r.range.begin = at("2024-01-02T00:00:00Z");
    r.range.end = at("2024-06-01T00:00:00Z");
    r.now = at("2026-01-01T00:00:00Z");
    r.purpose = "phase 2 ingest";
    return r;
}

}  // namespace

TEST_CASE("capability sets compose and report what is missing", "[market][entitlement]") {
    const CapabilitySet have{Capability::HistoricalBars, Capability::HistoricalQuotes};
    const CapabilitySet want{Capability::HistoricalBars, Capability::ConsolidatedFeed};

    REQUIRE(have.has(Capability::HistoricalBars));
    REQUIRE_FALSE(have.has(Capability::ConsolidatedFeed));
    REQUIRE_FALSE(have.contains(want));

    const CapabilitySet missing = have.missing_from(want);
    REQUIRE(missing.has(Capability::ConsolidatedFeed));
    REQUIRE_FALSE(missing.has(Capability::HistoricalBars));
    REQUIRE(missing.describe() == "consolidated_feed");

    REQUIRE(CapabilitySet{}.empty());
    REQUIRE(CapabilitySet{}.describe() == "(none)");
}

TEST_CASE("a fully entitled provider is granted", "[market][entitlement]") {
    StubProvider p({"alpaca", "sip", "ohlcv-1m", "consolidated_sip_historical", ""},
                   CapabilitySet{Capability::HistoricalBars, Capability::ConsolidatedFeed},
                   minutes{15});
    const auto report = EntitlementGate::evaluate(p, research_request());
    REQUIRE(report.granted);
    REQUIRE(report.missing.empty());
    REQUIRE(EntitlementGate::enforce(p, research_request()).has_value());
}

TEST_CASE("a single-venue provider is refused for consolidated work",
          "[market][entitlement][leakage]") {
    // THE FAILURE THIS GATE EXISTS FOR. IEX is a small share of consolidated
    // volume. A spread measured on it is not the NBBO spread, and no downstream
    // consumer can tell the difference -- the backtest runs to completion and
    // reports a plausible number built on unrepresentative data.
    StubProvider iex({"alpaca", "iex", "ohlcv-1m", "single_venue_iex", ""},
                     CapabilitySet{Capability::HistoricalBars}, minutes{15});

    const auto report = EntitlementGate::evaluate(iex, research_request());
    REQUIRE_FALSE(report.granted);
    REQUIRE(report.missing.has(Capability::ConsolidatedFeed));
    // The message must explain WHY it matters, not merely that a bit was unset.
    REQUIRE(report.describe().find("single venue") != std::string::npos);
    REQUIRE(report.describe().find("transaction-cost") != std::string::npos);

    auto enforced = EntitlementGate::enforce(iex, research_request());
    REQUIRE_FALSE(enforced.has_value());
    REQUIRE(enforced.error().code == ErrorCode::ValidationFailed);
    REQUIRE(enforced.error().context == "phase 2 ingest");
}

TEST_CASE("the gate reaches a verdict without fetching any data",
          "[market][entitlement][leakage]") {
    // Fail-fast means BEFORE the request, not after it fails confusingly.
    StubProvider iex({"alpaca", "iex", "ohlcv-1m", "single_venue_iex", ""},
                     CapabilitySet{Capability::HistoricalBars}, minutes{15});
    (void)EntitlementGate::enforce(iex, research_request());
    REQUIRE_FALSE(iex.fetched);
}

TEST_CASE("a range extending past the entitlement window is refused", "[market][entitlement]") {
    // Alpaca Basic withholds the most recent fifteen minutes. A provider with
    // the right capabilities can still be barred from the requested WINDOW.
    StubProvider p({"alpaca", "sip", "ohlcv-1m", "consolidated_sip_historical", ""},
                   CapabilitySet{Capability::HistoricalBars, Capability::ConsolidatedFeed},
                   minutes{15});

    EntitlementRequest r = research_request();
    r.now = at("2024-06-01T00:00:00Z");
    r.range.end = at("2024-06-01T00:00:00Z");  // right up to "now"
    const auto report = EntitlementGate::evaluate(p, r);
    REQUIRE_FALSE(report.granted);
    REQUIRE(report.describe().find("entitled through") != std::string::npos);

    // Backing off past the delay is granted.
    r.range.end = at("2024-05-31T23:40:00Z");
    REQUIRE(EntitlementGate::evaluate(p, r).granted);
}

TEST_CASE("realtime capability is separate from historical", "[market][entitlement]") {
    // Historical entitlement says nothing about realtime, and conflating them
    // is what makes a paper-trading session silently fall back to IEX.
    StubProvider basic({"alpaca", "sip", "ohlcv-1m", "consolidated_sip_historical", ""},
                       CapabilitySet{Capability::HistoricalBars, Capability::ConsolidatedFeed},
                       minutes{15});

    EntitlementRequest live;
    live.required = CapabilitySet{Capability::RealtimeBars, Capability::ConsolidatedFeed};
    live.now = at("2026-01-01T00:00:00Z");
    live.purpose = "phase 12 paper trading";

    const auto report = EntitlementGate::evaluate(basic, live);
    REQUIRE_FALSE(report.granted);
    REQUIRE(report.missing.has(Capability::RealtimeBars));
}

TEST_CASE("a provider with no delay is entitled through the end of time", "[market][entitlement]") {
    StubProvider paid({"alpaca", "sip", "ohlcv-1m", "consolidated_sip_historical", ""},
                      CapabilitySet{Capability::HistoricalBars, Capability::ConsolidatedFeed,
                                    Capability::RealtimeBars},
                      Duration::zero());
    REQUIRE(paid.available_through(at("2024-01-01T00:00:00Z")) == kMaxTimestamp);
    REQUIRE(EntitlementGate::evaluate(paid, research_request()).granted);
}
