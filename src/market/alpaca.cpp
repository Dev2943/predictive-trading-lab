#include "ptl/market/alpaca.hpp"

#include <algorithm>
#include <nlohmann/json.hpp>
#include <utility>

namespace ptl::market {
namespace {

using json = nlohmann::json;

[[nodiscard]] Error parse_error(std::string message, std::string context = {}) {
    return make_error(ErrorCode::ParseError, std::move(message), std::move(context));
}

/// Read a numeric field that a vendor may encode as either a number or a
/// string. Being liberal HERE is safe because the value is immediately handed
/// to a factory that rejects anything nonsensical; being liberal downstream
/// would not be.
[[nodiscard]] bool read_number(const json& obj, const char* key, double& out) {
    const auto it = obj.find(key);
    if (it == obj.end()) return false;
    if (it->is_number()) {
        out = it->get<double>();
        return true;
    }
    if (it->is_string()) {
        try {
            out = std::stod(it->get<std::string>());
            return true;
        } catch (...) {
            return false;
        }
    }
    return false;
}

}  // namespace

std::string_view to_string(AlpacaFeed f) noexcept {
    return f == AlpacaFeed::Sip ? "sip" : "iex";
}

Result<std::vector<Bar>> parse_alpaca_bars(std::string_view payload, Duration timeframe,
                                           InstrumentTable& instruments,
                                           std::string* next_page_token) {
    json doc;
    try {
        doc = json::parse(payload);
    } catch (const json::exception& e) {
        return fail(parse_error("malformed JSON in Alpaca bars payload", e.what()));
    }
    if (!doc.is_object()) return fail(parse_error("Alpaca bars payload is not an object"));

    if (next_page_token != nullptr) {
        next_page_token->clear();
        if (const auto it = doc.find("next_page_token"); it != doc.end() && it->is_string()) {
            *next_page_token = it->get<std::string>();
        }
    }

    const auto bars_it = doc.find("bars");
    if (bars_it == doc.end()) {
        return fail(parse_error("Alpaca bars payload has no 'bars' member"));
    }
    // An explicit null means "no data in this window", which is a legitimate
    // answer for a holiday and must not be confused with a malformed response.
    if (bars_it->is_null()) return std::vector<Bar>{};
    if (!bars_it->is_object()) {
        return fail(parse_error("Alpaca 'bars' member is not a symbol map"));
    }

    std::vector<Bar> out;
    for (const auto& [symbol, rows] : bars_it->items()) {
        if (!rows.is_array()) {
            return fail(parse_error("Alpaca bars for a symbol is not an array", symbol));
        }
        const InstrumentId id = instruments.intern(symbol);

        for (const auto& row : rows) {
            if (!row.is_object()) {
                return fail(parse_error("Alpaca bar row is not an object", symbol));
            }
            const auto t_it = row.find("t");
            if (t_it == row.end() || !t_it->is_string()) {
                return fail(parse_error("Alpaca bar row has no 't' timestamp", symbol));
            }
            Timestamp left_edge{};
            const std::string t_str = t_it->get<std::string>();
            if (!parse_timestamp(t_str, left_edge)) {
                return fail(parse_error("unparseable Alpaca bar timestamp", symbol + " " + t_str));
            }

            double o = 0;
            double h = 0;
            double l = 0;
            double c = 0;
            double v = 0;
            if (!read_number(row, "o", o) || !read_number(row, "h", h) ||
                !read_number(row, "l", l) || !read_number(row, "c", c) ||
                !read_number(row, "v", v)) {
                return fail(
                    parse_error("Alpaca bar row is missing an OHLCV field", symbol + " " + t_str));
            }

            // THE CRITICAL LINE OF THIS ADAPTER.
            //
            // Alpaca's 't' is the LEFT EDGE of the interval. Constructing via
            // from_left_edge applies that convention exactly once, here. Every
            // consumer downstream then receives a Bar whose close_time means
            // what it says, and no consumer has to remember the vendor's
            // convention. Treating 't' as a close time would be a one-minute
            // lookahead introduced by the data layer itself.
            auto bar = Bar::from_left_edge(id, left_edge, timeframe, Price{o}, Price{h}, Price{l},
                                           Price{c}, Volume{v});
            if (!bar) {
                return fail(make_error(ErrorCode::ValidationFailed,
                                       "Alpaca bar rejected: " + bar.error().message,
                                       symbol + " " + t_str));
            }
            out.push_back(*bar);
        }
    }

    // Alpaca groups by symbol, so a multi-symbol response is not globally
    // ordered even though each symbol's array is. Sorting here means the
    // provider always returns a chronological stream, which is what
    // ReplaySource requires.
    std::sort(out.begin(), out.end(), [](const Bar& a, const Bar& b) {
        if (a.close_time() != b.close_time()) return a.close_time() < b.close_time();
        return index_of(a.instrument()) < index_of(b.instrument());
    });
    return out;
}

AlpacaProvider::AlpacaProvider(AlpacaConfig cfg, auth::ApiCredential credential,
                               IHttpTransport& transport, InstrumentTable& instruments)
    : cfg_(std::move(cfg)),
      credential_(std::move(credential)),
      transport_(&transport),
      instruments_(&instruments) {
    id_.name = "alpaca";
    id_.feed = to_string(cfg_.feed);
    id_.schema = "ohlcv-1m";
    id_.coverage =
        cfg_.feed == AlpacaFeed::Sip ? "consolidated_sip_historical" : "single_venue_iex";
}

CapabilitySet AlpacaProvider::capabilities() const noexcept {
    CapabilitySet caps{Capability::HistoricalBars, Capability::HistoricalTrades,
                       Capability::HistoricalQuotes, Capability::CorporateActions};
    if (cfg_.feed == AlpacaFeed::Sip) {
        // Consolidated coverage is claimed ONLY for SIP. Its absence on IEX is
        // the whole reason the capability exists: single-venue data looks
        // exactly like consolidated data to every downstream consumer.
        caps.add(Capability::ConsolidatedFeed);
    }
    if (cfg_.realtime_entitled) {
        caps.add(Capability::RealtimeBars)
            .add(Capability::RealtimeTrades)
            .add(Capability::RealtimeQuotes);
    }
    return caps;
}

Timestamp AlpacaProvider::available_through(Timestamp now) const noexcept {
    if (cfg_.realtime_entitled) return kMaxTimestamp;
    if (!is_set(now)) return kMaxTimestamp;
    return now - cfg_.historical_end_delay;
}

HttpRequest AlpacaProvider::make_request(std::string_view path) const {
    HttpRequest r;
    r.url = cfg_.base_url + std::string{path};
    r.headers["APCA-API-KEY-ID"] = credential_.key_id;
    // The only place the secret leaves the Secret wrapper. reveal() reads as a
    // deliberate act, and it appears exactly once in the codebase.
    r.headers["APCA-API-SECRET-KEY"] = credential_.secret.reveal();
    return r;
}

HttpRequest AlpacaProvider::entitlement_probe_request() const {
    // Precisely the call ADR-0001 specifies, so the gate tests the documented
    // claim rather than an approximation of it.
    HttpRequest r = make_request("/v2/stocks/bars");
    r.query["symbols"] = "SPY";
    r.query["timeframe"] = "1Min";
    r.query["feed"] = std::string{to_string(cfg_.feed)};
    r.query["start"] = "2024-01-02T14:30:00Z";
    r.query["end"] = "2024-01-02T15:00:00Z";
    return r;
}

Result<std::vector<Bar>> AlpacaProvider::fetch_bars(const BarRequest& request) {
    if (!request.range.valid()) {
        return fail(make_error(ErrorCode::InvalidArgument, "bar request range is invalid"));
    }
    if (request.instruments.empty()) {
        return fail(make_error(ErrorCode::InvalidArgument, "bar request names no instruments"));
    }
    if (request.timeframe != std::chrono::minutes{1}) {
        return fail(make_error(ErrorCode::Unsupported,
                               "only 1Min bars are wired up; add the timeframe mapping "
                               "deliberately rather than guessing the vendor spelling"));
    }

    std::string symbols;
    for (std::size_t i = 0; i < request.instruments.size(); ++i) {
        if (i != 0) symbols += ',';
        symbols += std::string{instruments_->symbol(request.instruments[i])};
    }

    std::vector<Bar> all;
    std::string page_token;
    // Bounded rather than while(true): a vendor bug that returned the same
    // token forever would otherwise hang ingest silently.
    constexpr int kMaxPages = 10000;
    for (int page = 0; page < kMaxPages; ++page) {
        HttpRequest req = make_request("/v2/stocks/bars");
        req.query["symbols"] = symbols;
        req.query["timeframe"] = "1Min";
        req.query["feed"] = std::string{to_string(cfg_.feed)};
        req.query["start"] = to_iso8601(request.range.begin);
        req.query["end"] = to_iso8601(request.range.end);
        req.query["limit"] = std::to_string(cfg_.page_limit);
        if (!page_token.empty()) req.query["page_token"] = page_token;

        auto response = transport_->get(req);
        if (!response) return fail(response.error());
        if (!response->ok()) {
            return fail(make_error(ErrorCode::IoError,
                                   "Alpaca returned HTTP " + std::to_string(response->status),
                                   response->body));
        }

        std::string next;
        auto bars = parse_alpaca_bars(response->body, request.timeframe, *instruments_, &next);
        if (!bars) return fail(bars.error());
        all.insert(all.end(), bars->begin(), bars->end());

        if (next.empty()) break;
        page_token = next;
    }

    std::sort(all.begin(), all.end(), [](const Bar& a, const Bar& b) {
        if (a.close_time() != b.close_time()) return a.close_time() < b.close_time();
        return index_of(a.instrument()) < index_of(b.instrument());
    });
    return all;
}

Result<std::vector<Quote>> AlpacaProvider::fetch_quotes(const QuoteRequest&) {
    // Deliberately not implemented. ADR-0001 assigns the quote tiers to
    // Databento cbbo-1m/cbbo-1s, because Alpaca serves tick quotes and
    // subsampling them client-side means downloading order-of-10^9 rows to
    // discard almost all of them. Returning an error is honest; returning an
    // empty vector would look like "no quotes in this window".
    return fail(make_error(ErrorCode::Unsupported,
                           "Alpaca quote ingestion is out of scope: ADR-0001 assigns the "
                           "quote tiers to Databento cbbo-1m/cbbo-1s"));
}

}  // namespace ptl::market
