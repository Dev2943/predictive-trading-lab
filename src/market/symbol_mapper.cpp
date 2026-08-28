#include "ptl/market/symbol_mapper.hpp"

#include <algorithm>

#include "ptl/market/quote_stream.hpp"

namespace ptl::market {
namespace {

[[nodiscard]] Error bad(std::string message, std::string context = {}) {
    return make_error(ErrorCode::ValidationFailed, std::move(message), std::move(context));
}

}  // namespace

Result<InstrumentId> SymbolMapper::add(const SymbolMapping& mapping) {
    if (mapping.raw_symbol.empty()) return fail(bad("symbol mapping has no raw symbol"));
    if (!is_set(mapping.valid_from)) return fail(bad("symbol mapping has no valid_from"));
    if (mapping.valid_to <= mapping.valid_from) {
        return fail(bad("symbol mapping validity window is empty", mapping.raw_symbol));
    }

    for (const auto& existing : mappings_) {
        if (existing.raw_symbol != mapping.raw_symbol) continue;
        // OVERLAPPING WINDOWS ARE REFUSED. Two live mappings for one ticker
        // mean a lookup would have to choose, and choosing silently is how one
        // company's prices get attributed to another's history after a ticker
        // is reused post-delisting.
        const bool overlaps =
            mapping.valid_from < existing.valid_to && existing.valid_from < mapping.valid_to;
        if (overlaps) {
            return fail(bad(
                "overlapping symbol mapping for " + mapping.raw_symbol,
                to_iso8601(mapping.valid_from) + " overlaps " + to_iso8601(existing.valid_from)));
        }
    }

    SymbolMapping stored = mapping;
    // Interned through the shared table, so ids are consistent with every other
    // module rather than local to the mapper.
    stored.instrument = instruments_->intern(mapping.raw_symbol);
    mappings_.push_back(stored);
    return stored.instrument;
}

Result<InstrumentId> SymbolMapper::add_static(std::string raw_symbol, std::uint32_t vendor_id) {
    SymbolMapping m;
    m.raw_symbol = std::move(raw_symbol);
    m.vendor_id = vendor_id;
    // Open-ended from the epoch: the common case for a liquid ETF that has
    // never been remapped.
    m.valid_from = Timestamp{Duration::zero()};
    m.valid_to = kMaxTimestamp;
    return add(m);
}

std::optional<InstrumentId> SymbolMapper::resolve(std::string_view raw_symbol,
                                                  Timestamp as_of) const {
    for (const auto& m : mappings_) {
        if (m.raw_symbol == raw_symbol && m.covers(as_of)) return m.instrument;
    }
    return std::nullopt;
}

std::optional<InstrumentId> SymbolMapper::resolve_vendor_id(std::uint32_t vendor_id,
                                                            Timestamp as_of) const {
    if (vendor_id == 0) return std::nullopt;
    for (const auto& m : mappings_) {
        if (m.vendor_id == vendor_id && m.covers(as_of)) return m.instrument;
    }
    return std::nullopt;
}

std::optional<std::string_view> SymbolMapper::raw_symbol_of(InstrumentId instrument,
                                                            Timestamp as_of) const {
    for (const auto& m : mappings_) {
        if (m.instrument == instrument && m.covers(as_of)) return m.raw_symbol;
    }
    return std::nullopt;
}

void SymbolMapper::reset() noexcept {
    mappings_.clear();
}

// ---------------------------------------------------------------------------
// QuoteNormaliser
// ---------------------------------------------------------------------------

std::optional<Quote> QuoteNormaliser::normalise(const RawQuote& raw) {
    const Timestamp exchange_time{Duration{raw.exchange_time_ns}};
    if (raw.exchange_time_ns <= 0) {
        ++stats_.dropped_invalid;
        return std::nullopt;
    }

    const auto instrument = mapper_->resolve_vendor_id(raw.vendor_instrument_id, exchange_time);
    if (!instrument.has_value()) {
        // Counted, never silently lost: this count is what tells you a
        // symbology file was incomplete for part of the range.
        ++stats_.dropped_unmapped;
        return std::nullopt;
    }

    if (cfg_.drop_one_sided && (raw.bid_price_fixed <= 0 || raw.ask_price_fixed <= 0)) {
        // A one-sided book is real at the very open, but it is not tradeable
        // and cannot price a fill.
        ++stats_.dropped_one_sided;
        return std::nullopt;
    }

    // The fixed-point conversion happens ONCE, here, with the scale stated.
    // Doing it at each call site is how a factor-of-1000 error survives review.
    const double bid = static_cast<double>(raw.bid_price_fixed) * cfg_.price_scale;
    const double ask = static_cast<double>(raw.ask_price_fixed) * cfg_.price_scale;

    if (ask < bid) {
        ++stats_.dropped_crossed;
        return std::nullopt;
    }

    auto quote = Quote::create(*instrument, exchange_time, Price{bid},
                               Qty{static_cast<double>(raw.bid_size)}, Price{ask},
                               Qty{static_cast<double>(raw.ask_size)}, cfg_.feed_latency);
    if (!quote) {
        ++stats_.dropped_invalid;
        return std::nullopt;
    }
    ++stats_.accepted;
    return *quote;
}

Result<std::vector<Quote>> QuoteNormaliser::normalise_batch(std::span<const RawQuote> raws) {
    std::vector<Quote> out;
    out.reserve(raws.size());
    for (const auto& raw : raws) {
        if (auto q = normalise(raw)) out.push_back(*q);
    }
    // Chronology is checked here rather than assumed: a decoder that emitted
    // records out of order would otherwise be discovered much later, inside the
    // quote book, with far less context.
    for (std::size_t i = 1; i < out.size(); ++i) {
        if (out[i].time().exchange_time < out[i - 1].time().exchange_time) {
            return fail(
                bad("normalised quotes are not chronological at index " + std::to_string(i)));
        }
    }
    return out;
}

}  // namespace ptl::market
