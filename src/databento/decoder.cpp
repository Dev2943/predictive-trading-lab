#include "ptl/databento/decoder.hpp"

#include <algorithm>
#include <nlohmann/json.hpp>

#include "ptl/databento/symbols.hpp"

namespace ptl::databento {
namespace {

using json = nlohmann::json;

[[nodiscard]] Error parse_error(std::string message, std::string context = {}) {
    return make_error(ErrorCode::ParseError, std::move(message), std::move(context));
}

/// Read a field that may be a number or a numeric string. Vendors are
/// inconsistent about this, and being liberal HERE is safe because every value
/// is handed straight to a validating factory.
[[nodiscard]] bool read_i64(const json& obj, const char* key, std::int64_t& out) {
    const auto it = obj.find(key);
    if (it == obj.end()) return false;
    if (it->is_number_integer() || it->is_number_unsigned()) {
        out = it->get<std::int64_t>();
        return true;
    }
    if (it->is_number_float()) {
        out = static_cast<std::int64_t>(it->get<double>());
        return true;
    }
    if (it->is_string()) {
        try {
            out = std::stoll(it->get<std::string>());
            return true;
        } catch (...) {
            return false;
        }
    }
    return false;
}

[[nodiscard]] bool read_double(const json& obj, const char* key, double& out) {
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

[[nodiscard]] Result<json> parse_records(std::string_view payload, const char* what) {
    json doc;
    try {
        doc = json::parse(payload);
    } catch (const json::exception& e) {
        return fail(parse_error(std::string{"malformed JSON in Databento "} + what, e.what()));
    }
    // Accept either a bare array or {"records": [...]}: Databento's HTTP and
    // file forms differ, and handling both here beats making every caller know.
    if (doc.is_array()) return doc;
    if (doc.is_object()) {
        if (const auto it = doc.find("records"); it != doc.end() && it->is_array()) {
            return *it;
        }
    }
    return fail(parse_error(std::string{"Databento "} + what +
                            " payload is neither an array nor a records object"));
}

}  // namespace

std::string_view to_string(Schema s) noexcept {
    switch (s) {
        case Schema::Cbbo1m:
            return "cbbo-1m";
        case Schema::Cbbo1s:
            return "cbbo-1s";
        case Schema::Ohlcv1m:
            return "ohlcv-1m";
        case Schema::Trades:
            return "trades";
        case Schema::Definition:
            return "definition";
        case Schema::CorporateActions:
            return "corporate-actions";
    }
    return "unknown";
}

Result<Schema> parse_schema(std::string_view name) {
    if (name == "cbbo-1m") return Schema::Cbbo1m;
    if (name == "cbbo-1s") return Schema::Cbbo1s;
    if (name == "ohlcv-1m") return Schema::Ohlcv1m;
    if (name == "trades") return Schema::Trades;
    if (name == "definition") return Schema::Definition;
    if (name == "corporate-actions") return Schema::CorporateActions;

    if (name == "bbo-1m" || name == "bbo-1s") {
        // ADR-0001 is explicit: cbbo-* is consolidated, bbo-* is scoped to a
        // single publisher. Accepting the latter here would give single-venue
        // data the appearance of consolidated coverage -- exactly the confusion
        // the entitlement gate exists to prevent.
        return fail(
            make_error(ErrorCode::Unsupported,
                       "schema '" + std::string{name} +
                           "' is publisher-scoped, not consolidated. ADR-0001 requires cbbo-1m or "
                           "cbbo-1s; a single-venue book is not a substitute for the consolidated "
                           "tape and must not be used for spread or transaction-cost modelling."));
    }
    return fail(
        make_error(ErrorCode::Unsupported, "unrecognised Databento schema: " + std::string{name}));
}

Result<std::vector<market::Quote>> Decoder::decode_quotes(std::string_view payload) {
    auto records = parse_records(payload, "quotes");
    if (!records) return fail(records.error());

    std::vector<market::RawQuote> raws;
    raws.reserve(records->size());

    for (const auto& rec : *records) {
        ++stats_.records_read;
        if (!rec.is_object()) {
            ++stats_.records_rejected;
            return fail(parse_error("Databento quote record is not an object"));
        }

        market::RawQuote raw;
        std::int64_t instrument_id = 0;
        std::int64_t ts = 0;
        if (!read_i64(rec, "instrument_id", instrument_id) || !read_i64(rec, "ts_recv", ts)) {
            // ts_recv is the venue-side receive stamp Databento normalises on.
            // A record without it cannot be placed in time at all.
            ++stats_.records_rejected;
            return fail(parse_error("Databento quote record lacks instrument_id or ts_recv"));
        }
        raw.vendor_instrument_id = static_cast<std::uint32_t>(instrument_id);
        raw.exchange_time_ns = ts;

        std::int64_t bid = 0;
        std::int64_t ask = 0;
        if (!read_i64(rec, "bid_px", bid) || !read_i64(rec, "ask_px", ask)) {
            ++stats_.records_rejected;
            return fail(parse_error("Databento quote record lacks bid_px or ask_px"));
        }
        raw.bid_price_fixed = bid;
        raw.ask_price_fixed = ask;

        std::int64_t bid_sz = 0;
        std::int64_t ask_sz = 0;
        (void)read_i64(rec, "bid_sz", bid_sz);
        (void)read_i64(rec, "ask_sz", ask_sz);
        raw.bid_size = static_cast<std::uint32_t>(std::max<std::int64_t>(0, bid_sz));
        raw.ask_size = static_cast<std::uint32_t>(std::max<std::int64_t>(0, ask_sz));

        raws.push_back(raw);
    }

    auto quotes = normaliser_.normalise_batch(raws);
    if (!quotes) return fail(quotes.error());
    stats_.records_decoded += quotes->size();
    return quotes;
}

Result<std::vector<market::Bar>> Decoder::decode_bars(std::string_view payload,
                                                      Duration timeframe) {
    auto records = parse_records(payload, "bars");
    if (!records) return fail(records.error());
    if (timeframe <= Duration::zero()) {
        return fail(parse_error("bar timeframe must be positive"));
    }

    std::vector<market::Bar> out;
    out.reserve(records->size());

    for (const auto& rec : *records) {
        ++stats_.records_read;
        std::int64_t instrument_id = 0;
        std::int64_t ts = 0;
        if (!read_i64(rec, "instrument_id", instrument_id) || !read_i64(rec, "ts_event", ts)) {
            ++stats_.records_rejected;
            return fail(parse_error("Databento bar record lacks instrument_id or ts_event"));
        }

        const Timestamp left_edge{Duration{ts}};
        const auto instrument =
            mapper_->resolve_vendor_id(static_cast<std::uint32_t>(instrument_id), left_edge);
        if (!instrument.has_value()) {
            ++stats_.records_rejected;
            continue;  // unmapped, counted, not fatal
        }

        double o = 0;
        double h = 0;
        double l = 0;
        double c = 0;
        double v = 0;
        if (!read_double(rec, "open", o) || !read_double(rec, "high", h) ||
            !read_double(rec, "low", l) || !read_double(rec, "close", c)) {
            ++stats_.records_rejected;
            return fail(parse_error("Databento bar record is missing an OHLC field"));
        }
        (void)read_double(rec, "volume", v);

        // Databento OHLCV records are stamped with the interval's OPEN, like
        // Alpaca. from_left_edge applies that convention exactly once, here;
        // treating the stamp as a close would be a one-minute lookahead
        // introduced by the decoder itself.
        auto bar = market::Bar::from_left_edge(*instrument, left_edge, timeframe, Price{o},
                                               Price{h}, Price{l}, Price{c}, Volume{v});
        if (!bar) {
            ++stats_.records_rejected;
            return fail(make_error(ErrorCode::ValidationFailed,
                                   "Databento bar rejected: " + bar.error().message));
        }
        out.push_back(*bar);
        ++stats_.records_decoded;
    }

    std::sort(out.begin(), out.end(), [](const market::Bar& a, const market::Bar& b) {
        if (a.close_time() != b.close_time()) return a.close_time() < b.close_time();
        return index_of(a.instrument()) < index_of(b.instrument());
    });
    return out;
}

Result<std::vector<market::Trade>> Decoder::decode_trades(std::string_view payload) {
    auto records = parse_records(payload, "trades");
    if (!records) return fail(records.error());

    std::vector<market::Trade> out;
    out.reserve(records->size());

    for (const auto& rec : *records) {
        ++stats_.records_read;
        std::int64_t instrument_id = 0;
        std::int64_t ts = 0;
        std::int64_t px = 0;
        std::int64_t size = 0;
        if (!read_i64(rec, "instrument_id", instrument_id) || !read_i64(rec, "ts_recv", ts) ||
            !read_i64(rec, "price", px) || !read_i64(rec, "size", size)) {
            ++stats_.records_rejected;
            return fail(parse_error("Databento trade record is missing a required field"));
        }

        const Timestamp exchange_time{Duration{ts}};
        const auto instrument =
            mapper_->resolve_vendor_id(static_cast<std::uint32_t>(instrument_id), exchange_time);
        if (!instrument.has_value()) {
            ++stats_.records_rejected;
            continue;
        }

        // Same fixed-point scale as quotes: nine implied decimals.
        const double price = static_cast<double>(px) * 1e-9;
        // Aggressor side is DELIBERATELY not inferred. Signing a trade needs a
        // contemporaneous quote and a rule, and inventing one here would be a
        // modelling assumption dressed as data.
        auto trade = market::Trade::create(*instrument, exchange_time, Price{price},
                                           Qty{static_cast<double>(size)});
        if (!trade) {
            ++stats_.records_rejected;
            continue;
        }
        out.push_back(*trade);
        ++stats_.records_decoded;
    }
    return out;
}

Result<std::vector<InstrumentDefinition>> Decoder::decode_definitions(std::string_view payload) {
    auto records = parse_records(payload, "definitions");
    if (!records) return fail(records.error());

    std::vector<InstrumentDefinition> out;
    for (const auto& rec : *records) {
        ++stats_.records_read;
        InstrumentDefinition def;
        std::int64_t id = 0;
        if (!read_i64(rec, "instrument_id", id)) {
            ++stats_.records_rejected;
            return fail(parse_error("instrument definition lacks instrument_id"));
        }
        def.instrument_id = static_cast<std::uint32_t>(id);

        if (const auto it = rec.find("raw_symbol"); it != rec.end() && it->is_string()) {
            def.raw_symbol = it->get<std::string>();
        } else {
            ++stats_.records_rejected;
            return fail(parse_error("instrument definition lacks raw_symbol"));
        }
        if (const auto it = rec.find("exchange"); it != rec.end() && it->is_string()) {
            def.exchange = it->get<std::string>();
        }
        if (const auto it = rec.find("asset_class"); it != rec.end() && it->is_string()) {
            def.asset_class = it->get<std::string>();
        }
        (void)read_double(rec, "min_price_increment", def.min_price_increment);
        std::int64_t lot = 0;
        if (read_i64(rec, "lot_size", lot) && lot > 0) {
            def.lot_size = static_cast<std::uint32_t>(lot);
        }
        out.push_back(std::move(def));
        ++stats_.records_decoded;
    }
    return out;
}

Result<std::vector<market::CorporateAction>> Decoder::decode_corporate_actions(
    std::string_view payload) {
    auto records = parse_records(payload, "corporate actions");
    if (!records) return fail(records.error());

    std::vector<market::CorporateAction> out;
    for (const auto& rec : *records) {
        ++stats_.records_read;
        std::int64_t instrument_id = 0;
        std::int64_t ts = 0;
        if (!read_i64(rec, "instrument_id", instrument_id) || !read_i64(rec, "ts_event", ts)) {
            ++stats_.records_rejected;
            return fail(parse_error("corporate action lacks instrument_id or ts_event"));
        }
        const Timestamp effective{Duration{ts}};
        const auto instrument =
            mapper_->resolve_vendor_id(static_cast<std::uint32_t>(instrument_id), effective);
        if (!instrument.has_value()) {
            ++stats_.records_rejected;
            continue;
        }

        std::string kind;
        if (const auto it = rec.find("action"); it != rec.end() && it->is_string()) {
            kind = it->get<std::string>();
        }

        Result<market::CorporateAction> action =
            fail(parse_error("unrecognised corporate action type: " + kind));
        if (kind == "split") {
            double ratio = 0.0;
            if (!read_double(rec, "ratio", ratio)) {
                ++stats_.records_rejected;
                return fail(parse_error("split record lacks a ratio"));
            }
            action = market::CorporateAction::split(*instrument, effective, ratio);
        } else if (kind == "dividend" || kind == "cash_dividend") {
            double amount = 0.0;
            if (!read_double(rec, "amount", amount)) {
                ++stats_.records_rejected;
                return fail(parse_error("dividend record lacks an amount"));
            }
            action =
                market::CorporateAction::cash_dividend(*instrument, effective, Notional{amount});
        } else {
            // An unknown action type is REFUSED rather than skipped. A missed
            // split silently corrupts every return across the event date, and
            // a loud failure is far cheaper than discovering it in a Sharpe.
            ++stats_.records_rejected;
            return fail(parse_error("unrecognised corporate action type '" + kind +
                                    "'; a missed split corrupts every return across "
                                    "its effective date"));
        }

        if (!action) {
            ++stats_.records_rejected;
            return fail(action.error());
        }
        out.push_back(*action);
        ++stats_.records_decoded;
    }
    return out;
}

void Decoder::reset() noexcept {
    stats_ = DecoderStats{};
    normaliser_.reset();
}

}  // namespace ptl::databento
