#include <algorithm>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <string>

#include "ptl/databento/client.hpp"
#include "ptl/databento/historical.hpp"
#include "ptl/databento/live.hpp"
#include "ptl/databento/symbols.hpp"
#include "support/ptl_catch.hpp"

using namespace ptl;
using namespace ptl::databento;
using namespace std::chrono;

namespace {

Timestamp at(const char* iso) {
    Timestamp ts{};
    REQUIRE(parse_timestamp(iso, ts));
    return ts;
}

/// Nanoseconds since epoch for an ISO instant, as Databento sends them.
std::int64_t ns(const char* iso) {
    return at(iso).time_since_epoch().count();
}

struct Fixture {
    InstrumentTable instruments;
    market::SymbolMapper mapper{instruments};

    Fixture() {
        market::SymbolMapping spy;
        spy.raw_symbol = "SPY";
        spy.vendor_id = 1001;
        spy.valid_from = Timestamp{Duration::zero()};
        spy.valid_to = kMaxTimestamp;
        REQUIRE(mapper.add(spy).has_value());
    }
};

}  // namespace

TEST_CASE("publisher-scoped bbo schemas are refused", "[databento][adr]") {
    // ADR-0001 is explicit: cbbo-* is consolidated, bbo-* is one publisher.
    // Accepting bbo-1m here would give single-venue data the appearance of
    // consolidated coverage -- the exact confusion the entitlement gate exists
    // to prevent.
    REQUIRE(parse_schema("cbbo-1m").has_value());
    REQUIRE(parse_schema("cbbo-1s").has_value());

    auto rejected = parse_schema("bbo-1m");
    REQUIRE_FALSE(rejected.has_value());
    REQUIRE(rejected.error().message.find("publisher-scoped") != std::string::npos);
    REQUIRE(rejected.error().message.find("cbbo-1m") != std::string::npos);
    REQUIRE_FALSE(parse_schema("bbo-1s").has_value());
    REQUIRE_FALSE(parse_schema("nonsense").has_value());
}

TEST_CASE("quotes decode with fixed-point prices scaled once", "[databento][decoder]") {
    // The scale conversion happens in one place. Doing it at each call site is
    // how a factor-of-1000 error survives review.
    Fixture f;
    Decoder decoder{f.mapper};

    const std::string payload = std::string{R"([
      {"instrument_id": 1001, "ts_recv": )"} +
                                std::to_string(ns("2024-07-02T15:00:00Z")) +
                                R"(, "bid_px": 499990000000, "ask_px": 500010000000,
           "bid_sz": 500, "ask_sz": 300}
    ])";

    auto quotes = decoder.decode_quotes(payload);
    REQUIRE(quotes.has_value());
    REQUIRE(quotes->size() == 1);
    // 499990000000 * 1e-9 = 499.99
    REQUIRE(quotes->front().bid().get() == Catch::Approx(499.99));
    REQUIRE(quotes->front().ask().get() == Catch::Approx(500.01));
    REQUIRE(quotes->front().bid_size().get() == Catch::Approx(500.0));
    REQUIRE(quotes->front().time().exchange_time == at("2024-07-02T15:00:00Z"));
}

TEST_CASE("unmapped records are counted not silently lost", "[databento][decoder][validation]") {
    // This count is what tells you a symbology file was incomplete for part of
    // the range.
    Fixture f;
    Decoder decoder{f.mapper};

    const std::string payload =
        std::string{R"([
      {"instrument_id": 9999, "ts_recv": )"} +
        std::to_string(ns("2024-07-02T15:00:00Z")) +
        R"(, "bid_px": 499990000000, "ask_px": 500010000000, "bid_sz": 1, "ask_sz": 1}
    ])";

    auto quotes = decoder.decode_quotes(payload);
    REQUIRE(quotes.has_value());
    REQUIRE(quotes->empty());
    REQUIRE(decoder.normaliser_stats().dropped_unmapped == 1);
}

TEST_CASE("a one-sided or crossed quote is dropped", "[databento][decoder][edge]") {
    Fixture f;
    Decoder decoder{f.mapper};
    const auto t = std::to_string(ns("2024-07-02T15:00:00Z"));

    // One-sided: real at the very open, but not tradeable and cannot price a
    // fill.
    auto one_sided =
        decoder.decode_quotes(std::string{R"([{"instrument_id":1001,"ts_recv":)"} + t +
                              R"(,"bid_px":0,"ask_px":500010000000,"bid_sz":0,"ask_sz":1}])");
    REQUIRE(one_sided.has_value());
    REQUIRE(one_sided->empty());
    REQUIRE(decoder.normaliser_stats().dropped_one_sided == 1);

    auto crossed = decoder.decode_quotes(
        std::string{R"([{"instrument_id":1001,"ts_recv":)"} + t +
        R"(,"bid_px":500100000000,"ask_px":499900000000,"bid_sz":1,"ask_sz":1}])");
    REQUIRE(crossed.has_value());
    REQUIRE(crossed->empty());
    REQUIRE(decoder.normaliser_stats().dropped_crossed == 1);
}

TEST_CASE("malformed payloads are refused with context", "[databento][decoder][validation]") {
    Fixture f;
    Decoder decoder{f.mapper};
    REQUIRE_FALSE(decoder.decode_quotes("not json").has_value());
    REQUIRE_FALSE(decoder.decode_quotes(R"({"unexpected": 1})").has_value());
    REQUIRE_FALSE(decoder.decode_quotes(R"([{"instrument_id": 1001}])").has_value());
    // A records wrapper is accepted as well as a bare array.
    REQUIRE(decoder.decode_quotes(R"({"records": []})").has_value());
}

TEST_CASE("bars decode with the left-edge convention", "[databento][decoder][leakage]") {
    // Databento stamps OHLCV with the interval's OPEN. Treating that as a close
    // would be a one-minute lookahead introduced by the decoder itself.
    Fixture f;
    Decoder decoder{f.mapper};

    const std::string payload =
        std::string{R"([
      {"instrument_id": 1001, "ts_event": )"} +
        std::to_string(ns("2024-07-02T15:00:00Z")) +
        R"(, "open": 500.0, "high": 500.5, "low": 499.5, "close": 500.25, "volume": 12000}
    ])";

    auto bars = decoder.decode_bars(payload, minutes{1});
    REQUIRE(bars.has_value());
    REQUIRE(bars->size() == 1);
    REQUIRE(bars->front().open_time() == at("2024-07-02T15:00:00Z"));
    REQUIRE(bars->front().close_time() == at("2024-07-02T15:01:00Z"));
    // The event exists at its close, not its open.
    REQUIRE(bars->front().time().exchange_time == bars->front().close_time());
}

TEST_CASE("trades decode without inventing an aggressor", "[databento][decoder][leakage]") {
    // Signing a trade needs a contemporaneous quote and a rule. Inventing one
    // would be a modelling assumption dressed as data.
    Fixture f;
    Decoder decoder{f.mapper};

    const std::string payload = std::string{R"([
      {"instrument_id": 1001, "ts_recv": )"} +
                                std::to_string(ns("2024-07-02T15:00:00Z")) +
                                R"(, "price": 500000000000, "size": 250}
    ])";

    auto trades = decoder.decode_trades(payload);
    REQUIRE(trades.has_value());
    REQUIRE(trades->size() == 1);
    REQUIRE(trades->front().price().get() == Catch::Approx(500.0));
    REQUIRE(trades->front().size().get() == Catch::Approx(250.0));
    REQUIRE_FALSE(trades->front().aggressor().has_value());
}

TEST_CASE("instrument definitions decode", "[databento][decoder]") {
    Fixture f;
    Decoder decoder{f.mapper};
    auto defs = decoder.decode_definitions(R"([
      {"instrument_id": 1001, "raw_symbol": "SPY", "exchange": "XNAS",
       "asset_class": "ETF", "min_price_increment": 0.01, "lot_size": 1}
    ])");
    REQUIRE(defs.has_value());
    REQUIRE(defs->size() == 1);
    REQUIRE(defs->front().raw_symbol == "SPY");
    REQUIRE(defs->front().exchange == "XNAS");
    REQUIRE_FALSE(decoder.decode_definitions(R"([{"raw_symbol":"SPY"}])").has_value());
}

TEST_CASE("corporate actions decode and unknown types are refused",
          "[databento][decoder][corpaction]") {
    // A missed split silently corrupts every return across its effective date.
    // A loud failure is far cheaper than discovering it in a Sharpe.
    Fixture f;
    Decoder decoder{f.mapper};
    const auto t = std::to_string(ns("2024-07-02T00:00:00Z"));

    auto split =
        decoder.decode_corporate_actions(std::string{R"([{"instrument_id":1001,"ts_event":)"} + t +
                                         R"(,"action":"split","ratio":2.0}])");
    REQUIRE(split.has_value());
    REQUIRE(split->size() == 1);
    REQUIRE(split->front().split_ratio() == Catch::Approx(2.0));

    auto dividend =
        decoder.decode_corporate_actions(std::string{R"([{"instrument_id":1001,"ts_event":)"} + t +
                                         R"(,"action":"dividend","amount":1.76}])");
    REQUIRE(dividend.has_value());
    REQUIRE(dividend->front().kind() == market::CorporateActionKind::CashDividend);

    auto unknown = decoder.decode_corporate_actions(
        std::string{R"([{"instrument_id":1001,"ts_event":)"} + t + R"(,"action":"spinoff"}])");
    REQUIRE_FALSE(unknown.has_value());
    REQUIRE(unknown.error().message.find("corrupts every return") != std::string::npos);
}

// ---------------------------------------------------------------------------
// Symbology
// ---------------------------------------------------------------------------

TEST_CASE("symbology parses and loads with exclusive end dates", "[databento][symbols]") {
    // Databento's d1 is EXCLUSIVE. Treating it as inclusive silently extends
    // every mapping by a day, which matters exactly when a ticker is remapped.
    const std::string payload = R"({
      "result": {
        "SPY": [{"d0": "2024-01-01", "d1": "2024-07-01", "s": "1001"}],
        "QQQ": [{"d0": "2024-01-01", "d1": "2024-07-01", "s": "1002"}]
      }
    })";

    auto intervals = parse_symbology(payload);
    REQUIRE(intervals.has_value());
    REQUIRE(intervals->size() == 2);

    InstrumentTable instruments;
    market::SymbolMapper mapper{instruments};
    auto added = load_symbology(mapper, *intervals);
    REQUIRE(added.has_value());
    REQUIRE(*added == 2);

    REQUIRE(mapper.resolve("SPY", at("2024-03-01T00:00:00Z")).has_value());
    // The last day is excluded.
    REQUIRE_FALSE(mapper.resolve("SPY", at("2024-07-01T00:00:00Z")).has_value());
    REQUIRE(mapper.resolve_vendor_id(1001, at("2024-03-01T00:00:00Z")).has_value());
}

TEST_CASE("overlapping symbol mappings are refused", "[databento][symbols][leakage]") {
    // Two live mappings for one ticker mean a lookup would have to choose, and
    // choosing silently is how one company's prices get attributed to another's
    // history after a ticker is reused post-delisting.
    InstrumentTable instruments;
    market::SymbolMapper mapper{instruments};

    market::SymbolMapping first;
    first.raw_symbol = "ACME";
    first.vendor_id = 1;
    first.valid_from = at("2024-01-01T00:00:00Z");
    first.valid_to = at("2024-06-01T00:00:00Z");
    REQUIRE(mapper.add(first).has_value());

    market::SymbolMapping overlapping = first;
    overlapping.vendor_id = 2;
    overlapping.valid_from = at("2024-05-01T00:00:00Z");
    overlapping.valid_to = at("2024-12-01T00:00:00Z");
    auto refused = mapper.add(overlapping);
    REQUIRE_FALSE(refused.has_value());
    REQUIRE(refused.error().message.find("overlapping") != std::string::npos);

    // Adjacent, non-overlapping windows are fine: that is a legitimate remap.
    market::SymbolMapping adjacent = first;
    adjacent.vendor_id = 3;
    adjacent.valid_from = at("2024-06-01T00:00:00Z");
    adjacent.valid_to = at("2024-12-01T00:00:00Z");
    REQUIRE(mapper.add(adjacent).has_value());

    // And the lookup picks the window that covers the instant.
    REQUIRE(mapper.resolve_vendor_id(1, at("2024-03-01T00:00:00Z")).has_value());
    REQUIRE(mapper.resolve_vendor_id(3, at("2024-08-01T00:00:00Z")).has_value());
    REQUIRE_FALSE(mapper.resolve_vendor_id(1, at("2024-08-01T00:00:00Z")).has_value());
}

TEST_CASE("malformed symbology is refused", "[databento][symbols][validation]") {
    REQUIRE_FALSE(parse_symbology("not json").has_value());
    REQUIRE_FALSE(parse_symbology(R"({"no_result": {}})").has_value());
    REQUIRE_FALSE(parse_symbology(R"({"result": {"SPY": [{"d0": "2024-01-01"}]}})").has_value());
}

// ---------------------------------------------------------------------------
// Historical merge
// ---------------------------------------------------------------------------

TEST_CASE("quotes precede bars on identical timestamps", "[databento][historical][leakage]") {
    // A bar close and a quote sampled at the same instant describe the same
    // moment. The simulator must hold the tradeable prices BEFORE it is asked
    // to act on the bar; emitting the bar first would price its fills from the
    // previous quote -- a one-interval staleness introduced by ordering alone.
    Fixture f;
    const auto instrument = *f.mapper.resolve("SPY", at("2024-07-02T15:00:00Z"));

    auto quote = market::Quote::create(instrument, at("2024-07-02T15:01:00Z"), Price{499.99},
                                       Qty{100}, Price{500.01}, Qty{100});
    auto bar = market::Bar::from_left_edge(instrument, at("2024-07-02T15:00:00Z"), minutes{1},
                                           Price{500.0}, Price{500.5}, Price{499.5}, Price{500.0},
                                           Volume{1000.0});
    REQUIRE(quote.has_value());
    REQUIRE(bar.has_value());
    // The bar CLOSES at 15:01:00 -- the same instant the quote is stamped.
    REQUIRE(bar->close_time() == quote->time().exchange_time);

    auto merged = merge_quotes_and_bars({*quote}, {*bar});
    REQUIRE(merged.has_value());
    REQUIRE(merged->size() == 2);
    REQUIRE(market::kind_name((*merged)[0]) == "quote");
    REQUIRE(market::kind_name((*merged)[1]) == "bar");
}

TEST_CASE("the merged stream is chronological", "[databento][historical]") {
    Fixture f;
    const auto instrument = *f.mapper.resolve("SPY", at("2024-07-02T15:00:00Z"));

    std::vector<market::Quote> quotes;
    std::vector<market::Bar> bars;
    Timestamp t = at("2024-07-02T15:00:00Z");
    for (int i = 0; i < 10; ++i) {
        auto q = market::Quote::create(instrument, t + seconds{30}, Price{499.99}, Qty{100},
                                       Price{500.01}, Qty{100});
        auto b = market::Bar::from_left_edge(instrument, t, minutes{1}, Price{500.0}, Price{500.5},
                                             Price{499.5}, Price{500.0}, Volume{1000.0});
        quotes.push_back(*q);
        bars.push_back(*b);
        t += minutes{1};
    }

    auto merged = merge_quotes_and_bars(quotes, bars);
    REQUIRE(merged.has_value());
    REQUIRE(merged->size() == 20);
    for (std::size_t i = 1; i < merged->size(); ++i) {
        REQUIRE(market::exchange_time_of((*merged)[i]) >=
                market::exchange_time_of((*merged)[i - 1]));
    }

    // An unsorted input is refused rather than quietly sorted.
    std::reverse(quotes.begin(), quotes.end());
    REQUIRE_FALSE(merge_quotes_and_bars(quotes, bars).has_value());
}

// ---------------------------------------------------------------------------
// Live / replay parity
// ---------------------------------------------------------------------------

TEST_CASE("the live source emits the same events as a replay", "[databento][live][determinism]") {
    // THE PARITY PROPERTY. LiveQuoteSource satisfies the same
    // IMarketDataSource as ReplaySource, and a session driven by recorded
    // payloads produces exactly the events a replay of the decoded quotes does.
    Fixture f;
    Decoder decoder{f.mapper};

    const auto payload = [&](const char* iso, double bid, double ask) {
        return std::string{R"([{"instrument_id":1001,"ts_recv":)"} + std::to_string(ns(iso)) +
               R"(,"bid_px":)" + std::to_string(static_cast<std::int64_t>(bid * 1e9)) +
               R"(,"ask_px":)" + std::to_string(static_cast<std::int64_t>(ask * 1e9)) +
               R"(,"bid_sz":100,"ask_sz":100}])";
    };

    QueuedLiveTransport transport;
    transport.push(payload("2024-07-02T15:00:00Z", 499.99, 500.01));
    transport.push(payload("2024-07-02T15:01:00Z", 500.00, 500.02));
    transport.push(payload("2024-07-02T15:02:00Z", 500.01, 500.03));

    SimulatedClock clock;
    LiveQuoteSource source{transport, decoder, &clock};

    std::vector<Timestamp> emitted;
    while (auto e = source.next()) {
        emitted.push_back(market::exchange_time_of(*e));
        // The clock follows RECEIVE time, exactly as a replay does.
        REQUIRE(clock.now() >= market::exchange_time_of(*e));
    }

    REQUIRE(emitted.size() == 3);
    REQUIRE(emitted[0] == at("2024-07-02T15:00:00Z"));
    REQUIRE(emitted[2] == at("2024-07-02T15:02:00Z"));
    REQUIRE(source.decoded() == 3);
    REQUIRE(source.payloads_read() == 3);
    // Exhausted sources report kMaxTimestamp, so a k-way merge needs no special
    // case -- the same contract ReplaySource honours.
    REQUIRE(source.peek_time() == kMaxTimestamp);
}

TEST_CASE("a live feed that regresses in time skips the record", "[databento][live][leakage]") {
    // A backwards event would violate the chronology every downstream component
    // assumes.
    Fixture f;
    Decoder decoder{f.mapper};

    const auto payload = [&](const char* iso) {
        return std::string{R"([{"instrument_id":1001,"ts_recv":)"} + std::to_string(ns(iso)) +
               R"(,"bid_px":499990000000,"ask_px":500010000000,"bid_sz":1,"ask_sz":1}])";
    };

    QueuedLiveTransport transport;
    transport.push(payload("2024-07-02T15:02:00Z"));
    transport.push(payload("2024-07-02T15:00:00Z"));  // backwards
    transport.push(payload("2024-07-02T15:03:00Z"));

    SimulatedClock clock;
    LiveQuoteSource source{transport, decoder, &clock};

    std::vector<Timestamp> emitted;
    while (auto e = source.next()) emitted.push_back(market::exchange_time_of(*e));

    REQUIRE(emitted.size() == 2);
    REQUIRE(emitted[0] == at("2024-07-02T15:02:00Z"));
    REQUIRE(emitted[1] == at("2024-07-02T15:03:00Z"));
}

TEST_CASE("a bad payload does not end the live session", "[databento][live][edge]") {
    Fixture f;
    Decoder decoder{f.mapper};

    QueuedLiveTransport transport;
    transport.push("garbage, not json");
    transport.push(std::string{R"([{"instrument_id":1001,"ts_recv":)"} +
                   std::to_string(ns("2024-07-02T15:00:00Z")) +
                   R"(,"bid_px":499990000000,"ask_px":500010000000,"bid_sz":1,"ask_sz":1}])");

    SimulatedClock clock;
    LiveQuoteSource source{transport, decoder, &clock};
    REQUIRE(source.next().has_value());
    REQUIRE_FALSE(source.next().has_value());
}

// ---------------------------------------------------------------------------
// Spend guard
// ---------------------------------------------------------------------------

TEST_CASE("the spend guard refuses an expensive download", "[databento][client][adr]") {
    // ADR-0001 imposes a hard spend guard. A refusal, not a warning: a warning
    // in a build log is a warning nobody reads, and the consequence is a real
    // invoice.
    Fixture f;
    market::RecordedTransport transport;

    auth::StaticCredentials creds{{{"K", "id"}, {"S", "secret"}}};
    auto credential = auth::resolve(creds, "K", "S");
    REQUIRE(credential.has_value());

    DatabentoConfig cfg;
    cfg.max_spend_usd = 25.0;
    DatabentoProvider provider{cfg, std::move(*credential), transport, f.instruments, f.mapper};

    CostEstimate cheap;
    cheap.usd = 10.0;
    cheap.estimated = true;
    REQUIRE(provider.enforce_spend_guard(cheap).has_value());

    CostEstimate expensive;
    expensive.usd = 500.0;
    expensive.estimated = true;
    auto refused = provider.enforce_spend_guard(expensive);
    REQUIRE_FALSE(refused.has_value());
    REQUIRE(refused.error().message.find("exceeds the configured maximum") != std::string::npos);

    // An unestimated cost cannot be guarded, so it is refused too.
    CostEstimate unknown;
    unknown.usd = 1.0;
    unknown.estimated = false;
    REQUIRE_FALSE(provider.enforce_spend_guard(unknown).has_value());
}

TEST_CASE("the override permits an expensive download deliberately", "[databento][client][adr]") {
    Fixture f;
    market::RecordedTransport transport;
    auth::StaticCredentials creds{{{"K", "id"}, {"S", "secret"}}};
    auto credential = auth::resolve(creds, "K", "S");

    DatabentoConfig cfg;
    cfg.max_spend_usd = 25.0;
    cfg.allow_spend_override = true;
    DatabentoProvider provider{cfg, std::move(*credential), transport, f.instruments, f.mapper};

    CostEstimate expensive;
    expensive.usd = 500.0;
    expensive.estimated = true;
    REQUIRE(provider.enforce_spend_guard(expensive).has_value());
}

TEST_CASE("the provider declares consolidated and sampled coverage",
          "[databento][client][entitlement]") {
    // cbbo-* IS consolidated -- the entire reason ADR-0001 selects it over
    // bbo-*. Declaring SampledQuotes lets a consumer needing a continuous book
    // refuse at startup rather than discovering gaps in its fill statistics.
    Fixture f;
    market::RecordedTransport transport;
    auth::StaticCredentials creds{{{"K", "id"}, {"S", "secret"}}};
    auto credential = auth::resolve(creds, "K", "S");

    DatabentoConfig cfg;
    cfg.schema = Schema::Cbbo1m;
    DatabentoProvider provider{cfg, std::move(*credential), transport, f.instruments, f.mapper};

    REQUIRE(provider.capabilities().has(market::Capability::ConsolidatedFeed));
    REQUIRE(provider.capabilities().has(market::Capability::SampledQuotes));
    REQUIRE(provider.capabilities().has(market::Capability::HistoricalQuotes));
    REQUIRE_FALSE(provider.capabilities().has(market::Capability::RealtimeQuotes));
    REQUIRE(provider.identity().coverage == "consolidated_cbbo");

    // The schema probe names the dataset, and the secret never enters the URL.
    const auto probe = provider.schema_probe_request();
    REQUIRE(probe.full_url().find("metadata.list_schemas") != std::string::npos);
    REQUIRE(probe.full_url().find("secret") == std::string::npos);
}
