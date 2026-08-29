#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <string>

#include "ptl/market/alpaca.hpp"
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

/// A realistic Alpaca /v2/stocks/bars payload. `t` is the LEFT EDGE.
constexpr const char* kTwoBars = R"({
  "bars": {
    "SPY": [
      {"t":"2024-07-02T14:52:00Z","o":544.10,"h":544.35,"l":544.02,"c":544.28,"v":118234,"n":812,"vw":544.19},
      {"t":"2024-07-02T14:53:00Z","o":544.28,"h":544.40,"l":544.20,"c":544.31,"v":95120,"n":690,"vw":544.30}
    ]
  },
  "next_page_token": null
})";

auth::ApiCredential fake_credential() {
    const auth::StaticCredentials src{{{"K", "PKTESTKEYID"}, {"S", "topsecretvalue"}}};
    auto c = auth::resolve(src, "K", "S");
    REQUIRE(c.has_value());
    return std::move(*c);
}

}  // namespace

TEST_CASE("Alpaca bar timestamps are treated as left edges", "[market][alpaca][leakage]") {
    // THE SINGLE MOST IMPORTANT ASSERTION IN THIS ADAPTER (ADR-0001 bar policy).
    //
    // A bar stamped 14:52:00Z covers [14:52:00, 14:53:00) and is not knowable
    // until 14:53:00. If this ever regresses, every backtest gains one minute
    // of lookahead and still runs to completion.
    InstrumentTable instruments;
    std::string token;
    auto bars = parse_alpaca_bars(kTwoBars, minutes{1}, instruments, &token);
    REQUIRE(bars.has_value());
    REQUIRE(bars->size() == 2);

    const Bar& first = bars->front();
    REQUIRE(instruments.symbol(first.instrument()) == "SPY");
    REQUIRE(to_iso8601(first.open_time()) == "2024-07-02T14:52:00.000000000Z");
    REQUIRE(to_iso8601(first.close_time()) == "2024-07-02T14:53:00.000000000Z");
    // And the event exists at its close, not its open.
    REQUIRE(first.time().exchange_time == first.close_time());

    REQUIRE(first.open().get() == Catch::Approx(544.10));
    REQUIRE(first.high().get() == Catch::Approx(544.35));
    REQUIRE(first.low().get() == Catch::Approx(544.02));
    REQUIRE(first.close().get() == Catch::Approx(544.28));
    REQUIRE(first.volume().get() == Catch::Approx(118234.0));
    REQUIRE(token.empty());
}

TEST_CASE("parsed bars are chronological across symbols", "[market][alpaca][determinism]") {
    // Alpaca groups by symbol, so a multi-symbol payload is not globally
    // ordered even though each array is. ReplaySource requires chronology.
    constexpr const char* kTwoSymbols = R"({
      "bars": {
        "QQQ": [{"t":"2024-07-02T14:52:00Z","o":480,"h":480,"l":480,"c":480,"v":10}],
        "SPY": [{"t":"2024-07-02T14:51:00Z","o":544,"h":544,"l":544,"c":544,"v":10}]
      }
    })";
    InstrumentTable instruments;
    auto bars = parse_alpaca_bars(kTwoSymbols, minutes{1}, instruments, nullptr);
    REQUIRE(bars.has_value());
    REQUIRE(bars->size() == 2);
    REQUIRE((*bars)[0].close_time() < (*bars)[1].close_time());
    REQUIRE(instruments.symbol((*bars)[0].instrument()) == "SPY");
}

TEST_CASE("a null bars member means no data not a malformed response", "[market][alpaca]") {
    // A holiday legitimately returns nothing. Conflating that with a parse
    // failure would make ingest abort on a perfectly good empty window.
    InstrumentTable instruments;
    auto bars = parse_alpaca_bars(R"({"bars": null})", minutes{1}, instruments, nullptr);
    REQUIRE(bars.has_value());
    REQUIRE(bars->empty());
}

TEST_CASE("malformed Alpaca payloads are rejected with context", "[market][alpaca][validation]") {
    InstrumentTable instruments;
    const auto bad = [&](const char* json) {
        return parse_alpaca_bars(json, minutes{1}, instruments, nullptr);
    };

    REQUIRE_FALSE(bad("not json at all").has_value());
    REQUIRE_FALSE(bad(R"({"unexpected": 1})").has_value());           // no bars member
    REQUIRE_FALSE(bad(R"({"bars": [1,2,3]})").has_value());           // not a symbol map
    REQUIRE_FALSE(bad(R"({"bars":{"SPY":"nope"}})").has_value());     // not an array
    REQUIRE_FALSE(bad(R"({"bars":{"SPY":[{"o":1}]}})").has_value());  // no timestamp
    REQUIRE_FALSE(
        bad(R"({"bars":{"SPY":[{"t":"not-a-time","o":1,"h":1,"l":1,"c":1,"v":1}]}})").has_value());
    // Missing an OHLCV field must fail rather than defaulting to zero -- a zero
    // price would be rejected downstream but with a far less useful message.
    REQUIRE_FALSE(
        bad(R"({"bars":{"SPY":[{"t":"2024-07-02T14:52:00Z","o":1,"h":1,"l":1}]}})").has_value());

    // A row that parses but violates a bar invariant is rejected too, and the
    // error names the symbol and timestamp.
    auto crossed = bad(
        R"({"bars":{"SPY":[{"t":"2024-07-02T14:52:00Z","o":100,"h":99,"l":101,"c":100,"v":1}]}})");
    REQUIRE_FALSE(crossed.has_value());
    REQUIRE(crossed.error().context.find("SPY") != std::string::npos);
}

TEST_CASE("numeric fields encoded as strings are accepted", "[market][alpaca][validation]") {
    // Being liberal here is safe: the value goes straight to a factory that
    // rejects anything nonsensical.
    InstrumentTable instruments;
    auto bars = parse_alpaca_bars(
        R"({"bars":{"SPY":[{"t":"2024-07-02T14:52:00Z","o":"100.5","h":"101","l":"100","c":"100.75","v":"500"}]}})",
        minutes{1}, instruments, nullptr);
    REQUIRE(bars.has_value());
    REQUIRE(bars->front().open().get() == Catch::Approx(100.5));
}

TEST_CASE("the provider declares consolidated coverage only for SIP",
          "[market][alpaca][entitlement][leakage]") {
    InstrumentTable instruments;
    RecordedTransport transport;

    AlpacaConfig sip_cfg;
    sip_cfg.feed = AlpacaFeed::Sip;
    AlpacaProvider sip{sip_cfg, fake_credential(), transport, instruments};
    REQUIRE(sip.capabilities().has(Capability::ConsolidatedFeed));
    REQUIRE(sip.identity().coverage == "consolidated_sip_historical");

    AlpacaConfig iex_cfg;
    iex_cfg.feed = AlpacaFeed::Iex;
    AlpacaProvider iex{iex_cfg, fake_credential(), transport, instruments};
    // The ABSENCE is the point: single-venue data looks exactly like
    // consolidated data to every downstream consumer.
    REQUIRE_FALSE(iex.capabilities().has(Capability::ConsolidatedFeed));
    REQUIRE(iex.identity().coverage == "single_venue_iex");
}

TEST_CASE("realtime capability follows the subscription flag", "[market][alpaca][entitlement]") {
    InstrumentTable instruments;
    RecordedTransport transport;

    AlpacaConfig basic;
    AlpacaProvider p{basic, fake_credential(), transport, instruments};
    REQUIRE_FALSE(p.capabilities().has(Capability::RealtimeBars));
    // Basic withholds the most recent fifteen minutes.
    REQUIRE(p.available_through(at("2024-07-02T20:00:00Z")) == at("2024-07-02T19:45:00Z"));

    AlpacaConfig plus;
    plus.realtime_entitled = true;
    AlpacaProvider paid{plus, fake_credential(), transport, instruments};
    REQUIRE(paid.capabilities().has(Capability::RealtimeBars));
    REQUIRE(paid.available_through(at("2024-07-02T20:00:00Z")) == kMaxTimestamp);
}

TEST_CASE("the entitlement probe issues exactly the request ADR-0001 documents",
          "[market][alpaca][entitlement]") {
    InstrumentTable instruments;
    RecordedTransport transport;
    AlpacaProvider p{AlpacaConfig{}, fake_credential(), transport, instruments};

    const HttpRequest probe = p.entitlement_probe_request();
    const std::string url = probe.full_url();
    REQUIRE(url.find("/v2/stocks/bars") != std::string::npos);
    REQUIRE(url.find("symbols=SPY") != std::string::npos);
    REQUIRE(url.find("timeframe=1Min") != std::string::npos);
    REQUIRE(url.find("feed=sip") != std::string::npos);
    REQUIRE(url.find("start=2024-01-02T14:30:00") != std::string::npos);
    // Credentials travel in headers, never in the URL, so a logged or cached
    // URL cannot leak them.
    REQUIRE(url.find("topsecretvalue") == std::string::npos);
    REQUIRE(probe.headers.at("APCA-API-SECRET-KEY") == "topsecretvalue");
}

TEST_CASE("fetch_bars paginates until the token is exhausted", "[market][alpaca]") {
    InstrumentTable instruments;
    RecordedTransport transport;
    AlpacaProvider p{AlpacaConfig{}, fake_credential(), transport, instruments};

    BarRequest req;
    req.instruments = {instruments.intern("SPY")};
    req.range.begin = at("2024-07-02T13:30:00Z");
    req.range.end = at("2024-07-02T20:00:00Z");

    // Discover the exact URLs the adapter builds, then record answers for them.
    HttpRequest page1;
    page1.url = "https://data.alpaca.markets/v2/stocks/bars";
    page1.query = {{"symbols", "SPY"},
                   {"timeframe", "1Min"},
                   {"feed", "sip"},
                   {"start", to_iso8601(req.range.begin)},
                   {"end", to_iso8601(req.range.end)},
                   {"limit", "10000"}};
    HttpRequest page2 = page1;
    page2.query["page_token"] = "TOKEN2";

    transport.record(
        page1.full_url(),
        {200,
         R"({"bars":{"SPY":[{"t":"2024-07-02T14:52:00Z","o":544,"h":544,"l":544,"c":544,"v":10}]},"next_page_token":"TOKEN2"})"});
    transport.record(
        page2.full_url(),
        {200,
         R"({"bars":{"SPY":[{"t":"2024-07-02T14:53:00Z","o":545,"h":545,"l":545,"c":545,"v":10}]},"next_page_token":null})"});

    auto bars = p.fetch_bars(req);
    REQUIRE(bars.has_value());
    REQUIRE(bars->size() == 2);
    REQUIRE(transport.requested().size() == 2);
    REQUIRE((*bars)[0].close_time() < (*bars)[1].close_time());
}

TEST_CASE("an HTTP error is surfaced rather than returning no data",
          "[market][alpaca][validation]") {
    // An empty result and a rejected request must never look the same: the
    // first is a fact about the market, the second is a fact about us.
    InstrumentTable instruments;
    RecordedTransport transport;
    AlpacaProvider p{AlpacaConfig{}, fake_credential(), transport, instruments};

    BarRequest req;
    req.instruments = {instruments.intern("SPY")};
    req.range.begin = at("2024-07-02T13:30:00Z");
    req.range.end = at("2024-07-02T20:00:00Z");

    HttpRequest expected;
    expected.url = "https://data.alpaca.markets/v2/stocks/bars";
    expected.query = {{"symbols", "SPY"},
                      {"timeframe", "1Min"},
                      {"feed", "sip"},
                      {"start", to_iso8601(req.range.begin)},
                      {"end", to_iso8601(req.range.end)},
                      {"limit", "10000"}};
    transport.record(
        expected.full_url(),
        {403, R"({"message":"subscription does not permit querying recent SIP data"})"});

    auto bars = p.fetch_bars(req);
    REQUIRE_FALSE(bars.has_value());
    REQUIRE(bars.error().message.find("403") != std::string::npos);
    REQUIRE(bars.error().context.find("subscription") != std::string::npos);
}

TEST_CASE("invalid requests are refused before any network call", "[market][alpaca][validation]") {
    InstrumentTable instruments;
    RecordedTransport transport;
    AlpacaProvider p{AlpacaConfig{}, fake_credential(), transport, instruments};

    BarRequest empty;
    REQUIRE_FALSE(p.fetch_bars(empty).has_value());

    BarRequest no_syms;
    no_syms.range.begin = at("2024-07-02T13:30:00Z");
    no_syms.range.end = at("2024-07-02T20:00:00Z");
    REQUIRE_FALSE(p.fetch_bars(no_syms).has_value());

    // An unwired timeframe must fail loudly rather than guessing the vendor
    // spelling and silently fetching the wrong resolution.
    BarRequest wrong_tf = no_syms;
    wrong_tf.instruments = {instruments.intern("SPY")};
    wrong_tf.timeframe = minutes{5};
    REQUIRE_FALSE(p.fetch_bars(wrong_tf).has_value());

    REQUIRE(transport.requested().empty());
}

TEST_CASE("Alpaca quote ingestion is refused rather than silently empty", "[market][alpaca]") {
    // ADR-0001 assigns the quote tiers to Databento cbbo-*. Returning an empty
    // vector would look like "no quotes in this window".
    InstrumentTable instruments;
    RecordedTransport transport;
    AlpacaProvider p{AlpacaConfig{}, fake_credential(), transport, instruments};
    auto q = p.fetch_quotes({});
    REQUIRE_FALSE(q.has_value());
    REQUIRE(q.error().code == ErrorCode::Unsupported);
    REQUIRE(q.error().message.find("Databento") != std::string::npos);
}

TEST_CASE("query strings are built deterministically", "[market][alpaca][determinism]") {
    HttpRequest a;
    a.url = "https://x/y";
    a.query = {{"b", "2"}, {"a", "1"}, {"c", "3"}};
    HttpRequest b;
    b.url = "https://x/y";
    b.query = {{"c", "3"}, {"a", "1"}, {"b", "2"}};
    // Insertion order must not change the URL, or a fixture key and a cache key
    // would depend on how the map happened to be filled.
    REQUIRE(a.full_url() == b.full_url());
    REQUIRE(a.full_url() == "https://x/y?a=1&b=2&c=3");
}
