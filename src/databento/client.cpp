#include "ptl/databento/client.hpp"

#include <algorithm>
#include <nlohmann/json.hpp>
#include <sstream>

#include "ptl/databento/historical.hpp"
#include "ptl/databento/live.hpp"
#include "ptl/databento/symbols.hpp"

namespace ptl::databento {
namespace {

using json = nlohmann::json;

[[nodiscard]] Error bad(std::string message, std::string context = {}) {
    return make_error(ErrorCode::ValidationFailed, std::move(message), std::move(context));
}

}  // namespace

// ---------------------------------------------------------------------------
// Symbology
// ---------------------------------------------------------------------------

Result<std::vector<SymbologyInterval>> parse_symbology(std::string_view payload) {
    json doc;
    try {
        doc = json::parse(payload);
    } catch (const json::exception& e) {
        return fail(make_error(ErrorCode::ParseError, "malformed symbology JSON", e.what()));
    }

    const auto result = doc.find("result");
    if (result == doc.end() || !result->is_object()) {
        return fail(make_error(ErrorCode::ParseError, "symbology payload has no 'result' object"));
    }

    std::vector<SymbologyInterval> out;
    for (const auto& [symbol, intervals] : result->items()) {
        if (!intervals.is_array()) {
            return fail(make_error(ErrorCode::ParseError,
                                   "symbology intervals for a symbol are not an array", symbol));
        }
        for (const auto& iv : intervals) {
            SymbologyInterval entry;
            entry.raw_symbol = symbol;
            if (const auto it = iv.find("d0"); it != iv.end() && it->is_string()) {
                entry.start_date = it->get<std::string>();
            }
            if (const auto it = iv.find("d1"); it != iv.end() && it->is_string()) {
                entry.end_date = it->get<std::string>();
            }
            if (const auto it = iv.find("s"); it != iv.end()) {
                if (it->is_string()) {
                    try {
                        entry.instrument_id =
                            static_cast<std::uint32_t>(std::stoul(it->get<std::string>()));
                    } catch (...) {
                        return fail(make_error(ErrorCode::ParseError,
                                               "unparseable symbology instrument id", symbol));
                    }
                } else if (it->is_number_unsigned()) {
                    entry.instrument_id = it->get<std::uint32_t>();
                }
            }
            if (entry.instrument_id == 0 || entry.start_date.empty() || entry.end_date.empty()) {
                return fail(
                    make_error(ErrorCode::ParseError, "incomplete symbology interval", symbol));
            }
            out.push_back(std::move(entry));
        }
    }
    return out;
}

Result<std::size_t> load_symbology(market::SymbolMapper& mapper,
                                   std::span<const SymbologyInterval> intervals) {
    std::size_t added = 0;
    for (const auto& iv : intervals) {
        market::SymbolMapping m;
        m.raw_symbol = iv.raw_symbol;
        m.vendor_id = iv.instrument_id;

        Timestamp from{};
        Timestamp to{};
        if (!parse_date(iv.start_date, from)) {
            return fail(make_error(ErrorCode::ParseError, "unparseable symbology start date",
                                   iv.start_date));
        }
        if (!parse_date(iv.end_date, to)) {
            return fail(
                make_error(ErrorCode::ParseError, "unparseable symbology end date", iv.end_date));
        }
        m.valid_from = from;
        // Databento's d1 is EXCLUSIVE. Treating it as inclusive would silently
        // extend every mapping by a day, which matters precisely when a ticker
        // is remapped -- the one case the date range exists for.
        m.valid_to = to;

        auto id = mapper.add(m);
        if (!id) return fail(id.error());
        ++added;
    }
    return added;
}

// ---------------------------------------------------------------------------
// Provider
// ---------------------------------------------------------------------------

DatabentoProvider::DatabentoProvider(DatabentoConfig cfg, auth::ApiCredential credential,
                                     market::IHttpTransport& transport,
                                     InstrumentTable& instruments,
                                     const market::SymbolMapper& mapper)
    : cfg_(std::move(cfg)),
      credential_(std::move(credential)),
      transport_(&transport),
      instruments_(&instruments),
      mapper_(&mapper) {
    id_.name = "databento";
    id_.feed = "consolidated";
    id_.schema = std::string{to_string(cfg_.schema)};
    id_.dataset = cfg_.dataset;
    // cbbo-* IS consolidated coverage; that is the entire reason ADR-0001
    // selects it over bbo-*.
    id_.coverage = "consolidated_cbbo";
}

market::CapabilitySet DatabentoProvider::capabilities() const noexcept {
    market::CapabilitySet caps{
        market::Capability::HistoricalQuotes, market::Capability::HistoricalTrades,
        market::Capability::HistoricalBars, market::Capability::ConsolidatedFeed};
    if (cfg_.schema == Schema::Cbbo1m || cfg_.schema == Schema::Cbbo1s) {
        // Sampled, not continuous. Declaring this lets a consumer that needs a
        // continuous book refuse at startup instead of discovering the gaps in
        // its fill statistics.
        caps.add(market::Capability::SampledQuotes);
    }
    if (cfg_.schema == Schema::CorporateActions) {
        caps.add(market::Capability::CorporateActions);
    }
    if (cfg_.realtime_entitled) {
        caps.add(market::Capability::RealtimeQuotes).add(market::Capability::RealtimeTrades);
    }
    return caps;
}

Timestamp DatabentoProvider::available_through(Timestamp now) const noexcept {
    if (cfg_.realtime_entitled) return kMaxTimestamp;
    return now;  // historical data is available up to the present
}

market::HttpRequest DatabentoProvider::make_request(std::string_view path) const {
    market::HttpRequest r;
    r.url = cfg_.base_url + std::string{path};
    // Databento authenticates with HTTP basic using the key as the username.
    // The secret leaves the wrapper exactly once, here.
    r.headers["Authorization"] = "Bearer " + credential_.secret.reveal();
    return r;
}

market::HttpRequest DatabentoProvider::schema_probe_request() const {
    // The ADR-0001 Addendum A5 probe: confirm the schema exists for this
    // dataset before any download is attempted.
    market::HttpRequest r = make_request("/v0/metadata.list_schemas");
    r.query["dataset"] = cfg_.dataset;
    return r;
}

Result<CostEstimate> DatabentoProvider::estimate_cost(const market::QuoteRequest& request) const {
    if (!request.range.valid()) {
        return fail(bad("cost estimate needs a valid time range"));
    }

    std::string symbols;
    for (std::size_t i = 0; i < request.instruments.size(); ++i) {
        if (i != 0) symbols += ',';
        symbols += std::string{instruments_->symbol(request.instruments[i])};
    }

    market::HttpRequest r = make_request("/v0/metadata.get_cost");
    r.query["dataset"] = cfg_.dataset;
    r.query["schema"] = std::string{to_string(cfg_.schema)};
    r.query["symbols"] = symbols;
    r.query["start"] = to_iso8601(request.range.begin);
    r.query["end"] = to_iso8601(request.range.end);

    auto response = transport_->get(r);
    if (!response) return fail(response.error());
    if (!response->ok()) {
        return fail(
            make_error(ErrorCode::IoError,
                       "Databento cost estimate returned HTTP " + std::to_string(response->status),
                       response->body));
    }

    json doc;
    try {
        doc = json::parse(response->body);
    } catch (const json::exception& e) {
        return fail(make_error(ErrorCode::ParseError, "malformed cost estimate", e.what()));
    }

    CostEstimate estimate;
    estimate.estimated = true;
    if (doc.is_number()) {
        // The endpoint may return a bare number.
        estimate.usd = doc.get<double>();
        return estimate;
    }
    if (!doc.is_object()) return fail(bad("cost estimate is neither a number nor an object"));

    if (const auto it = doc.find("cost"); it != doc.end() && it->is_number()) {
        estimate.usd = it->get<double>();
    } else if (const auto usd = doc.find("total_cost"); usd != doc.end() && usd->is_number()) {
        estimate.usd = usd->get<double>();
    } else {
        // No cost field means the guard cannot be evaluated. Refusing is the
        // only safe response: proceeding would spend money against an estimate
        // that does not exist.
        return fail(
            bad("cost estimate response carries no cost field; the spend guard "
                "cannot be evaluated and the download is refused"));
    }
    if (const auto it = doc.find("size"); it != doc.end() && it->is_number_unsigned()) {
        estimate.bytes = it->get<std::uint64_t>();
    }
    if (const auto it = doc.find("record_count"); it != doc.end() && it->is_number_unsigned()) {
        estimate.record_count = it->get<std::uint64_t>();
    }
    return estimate;
}

Result<CostEstimate> DatabentoProvider::enforce_spend_guard(const CostEstimate& estimate) const {
    if (!estimate.estimated) {
        return fail(
            bad("spend guard requires an estimate from the vendor; no per-GB "
                "assumption is hardcoded, because a stale assumption would make "
                "the guard meaningless exactly when prices changed"));
    }
    if (!is_finite(estimate.usd) || estimate.usd < 0.0) {
        return fail(bad("cost estimate is not a usable number"));
    }
    if (estimate.usd > cfg_.max_spend_usd) {
        if (!cfg_.allow_spend_override) {
            // A REFUSAL, not a warning. A warning in a build log is a warning
            // nobody reads, and the consequence here is a real invoice.
            std::ostringstream ss;
            ss.precision(2);
            ss << std::fixed << "Databento cost estimate of $" << estimate.usd
               << " exceeds the configured maximum of $" << cfg_.max_spend_usd
               << ". Raise data.databento.max_spend_usd deliberately, or narrow the "
                  "request. (ADR-0001 spend guard.)";
            return fail(make_error(ErrorCode::ValidationFailed, ss.str()));
        }
    }
    return estimate;
}

Result<std::vector<market::Quote>> DatabentoProvider::fetch_quotes(
    const market::QuoteRequest& request) {
    if (!request.range.valid()) return fail(bad("quote request range is invalid"));
    if (request.instruments.empty()) {
        return fail(bad("quote request names no instruments"));
    }
    if (cfg_.schema != Schema::Cbbo1m && cfg_.schema != Schema::Cbbo1s) {
        return fail(make_error(ErrorCode::Unsupported, "provider is configured for schema '" +
                                                           std::string{to_string(cfg_.schema)} +
                                                           "', which does not serve quotes"));
    }

    // THE SPEND GUARD RUNS FIRST, BEFORE ANY DOWNLOAD. Estimating after
    // fetching would be an audit, not a guard.
    auto estimate = estimate_cost(request);
    if (!estimate) return fail(estimate.error());
    auto approved = enforce_spend_guard(*estimate);
    if (!approved) return fail(approved.error());

    std::string symbols;
    for (std::size_t i = 0; i < request.instruments.size(); ++i) {
        if (i != 0) symbols += ',';
        symbols += std::string{instruments_->symbol(request.instruments[i])};
    }

    market::HttpRequest r = make_request("/v0/timeseries.get_range");
    r.query["dataset"] = cfg_.dataset;
    r.query["schema"] = std::string{to_string(cfg_.schema)};
    r.query["symbols"] = symbols;
    r.query["start"] = to_iso8601(request.range.begin);
    r.query["end"] = to_iso8601(request.range.end);
    r.query["encoding"] = "json";

    auto response = transport_->get(r);
    if (!response) return fail(response.error());
    if (!response->ok()) {
        return fail(make_error(ErrorCode::IoError,
                               "Databento returned HTTP " + std::to_string(response->status),
                               response->body));
    }

    Decoder decoder{*mapper_};
    return decoder.decode_quotes(response->body);
}

Result<std::vector<market::Bar>> DatabentoProvider::fetch_bars(const market::BarRequest& request) {
    if (!request.range.valid()) return fail(bad("bar request range is invalid"));
    if (cfg_.schema != Schema::Ohlcv1m) {
        return fail(make_error(ErrorCode::Unsupported, "provider is configured for schema '" +
                                                           std::string{to_string(cfg_.schema)} +
                                                           "', which does not serve bars"));
    }

    std::string symbols;
    for (std::size_t i = 0; i < request.instruments.size(); ++i) {
        if (i != 0) symbols += ',';
        symbols += std::string{instruments_->symbol(request.instruments[i])};
    }

    market::HttpRequest r = make_request("/v0/timeseries.get_range");
    r.query["dataset"] = cfg_.dataset;
    r.query["schema"] = "ohlcv-1m";
    r.query["symbols"] = symbols;
    r.query["start"] = to_iso8601(request.range.begin);
    r.query["end"] = to_iso8601(request.range.end);
    r.query["encoding"] = "json";

    auto response = transport_->get(r);
    if (!response) return fail(response.error());
    if (!response->ok()) {
        return fail(make_error(ErrorCode::IoError,
                               "Databento returned HTTP " + std::to_string(response->status),
                               response->body));
    }

    Decoder decoder{*mapper_};
    return decoder.decode_bars(response->body, request.timeframe);
}

// ---------------------------------------------------------------------------
// Historical merge
// ---------------------------------------------------------------------------

Result<std::vector<market::MarketEvent>> merge_quotes_and_bars(std::vector<market::Quote> quotes,
                                                               std::vector<market::Bar> bars) {
    for (std::size_t i = 1; i < quotes.size(); ++i) {
        if (quotes[i].time().exchange_time < quotes[i - 1].time().exchange_time) {
            return fail(bad("quote series is not chronological at index " + std::to_string(i)));
        }
    }
    for (std::size_t i = 1; i < bars.size(); ++i) {
        if (bars[i].close_time() < bars[i - 1].close_time()) {
            return fail(bad("bar series is not chronological at index " + std::to_string(i)));
        }
    }

    std::vector<market::MarketEvent> out;
    out.reserve(quotes.size() + bars.size());

    std::size_t qi = 0;
    std::size_t bi = 0;
    while (qi < quotes.size() || bi < bars.size()) {
        const bool have_q = qi < quotes.size();
        const bool have_b = bi < bars.size();

        if (!have_b) {
            out.emplace_back(quotes[qi++]);
            continue;
        }
        if (!have_q) {
            out.emplace_back(bars[bi++]);
            continue;
        }

        const Timestamp qt = quotes[qi].time().exchange_time;
        const Timestamp bt = bars[bi].close_time();

        // QUOTE FIRST ON TIES. A bar close and a quote sampled at the same
        // instant describe the same moment, and the simulator must hold the
        // tradeable prices before it is asked to act on the bar. Emitting the
        // bar first would price its fills from the PREVIOUS quote -- a
        // one-interval staleness introduced by ordering alone.
        if (qt <= bt) {
            out.emplace_back(quotes[qi++]);
        } else {
            out.emplace_back(bars[bi++]);
        }
    }
    return out;
}

Result<std::vector<market::MarketEvent>> build_replay_events(std::vector<market::Quote> quotes,
                                                             std::vector<market::Bar> bars,
                                                             const market::Calendar& calendar) {
    auto merged = merge_quotes_and_bars(std::move(quotes), std::move(bars));
    if (!merged) return fail(merged.error());
    return market::with_session_events(std::move(*merged), calendar);
}

// ---------------------------------------------------------------------------
// Live
// ---------------------------------------------------------------------------

std::optional<std::string> QueuedLiveTransport::next_payload() {
    if (queue_.empty()) return std::nullopt;
    std::string out = std::move(queue_.front());
    queue_.pop_front();
    return out;
}

LiveQuoteSource::LiveQuoteSource(ILiveTransport& transport, Decoder& decoder,
                                 SimulatedClock* replay_clock)
    : transport_(&transport), decoder_(&decoder), replay_clock_(replay_clock) {}

bool LiveQuoteSource::refill() {
    while (pending_.empty()) {
        auto payload = transport_->next_payload();
        if (!payload.has_value()) return false;
        ++payloads_;

        auto quotes = decoder_->decode_quotes(*payload);
        if (!quotes) continue;  // a bad payload is skipped, not fatal to the session
        for (const auto& q : *quotes) {
            pending_.emplace_back(q);
            ++decoded_;
        }
    }
    return true;
}

std::optional<market::MarketEvent> LiveQuoteSource::next() {
    if (!refill()) return std::nullopt;

    market::MarketEvent event = pending_.front();
    pending_.pop_front();

    const Timestamp exchange_time = market::exchange_time_of(event);
    if (is_set(last_emitted_) && exchange_time < last_emitted_) {
        // A live feed that regresses in time is broken. Skipping the record is
        // safer than emitting it: a backwards event would violate the
        // chronology every downstream component assumes.
        return next();
    }
    last_emitted_ = exchange_time;

    // Advance to RECEIVE time, exactly as ReplaySource does -- the earliest
    // instant a live system could have known about this event. With no replay
    // clock this is a genuinely live session and the wall clock advances
    // itself; the events emitted are identical either way.
    if (replay_clock_ != nullptr) {
        replay_clock_->advance_to(market::receive_time_of(event));
    }
    return event;
}

Timestamp LiveQuoteSource::peek_time() const noexcept {
    if (pending_.empty()) return kMaxTimestamp;
    return market::exchange_time_of(pending_.front());
}

}  // namespace ptl::databento
