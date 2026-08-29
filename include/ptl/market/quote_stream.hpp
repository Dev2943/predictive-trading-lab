#pragma once

/// \file quote_stream.hpp
/// Normalising a vendor quote feed into market::Quote.
///
/// The normaliser is where a vendor's conventions stop being visible. Prices
/// arrive as scaled integers, timestamps as nanoseconds since epoch, sizes as
/// raw counts; downstream code sees only validated Quote objects whose
/// timestamps mean what they say.

#include <chrono>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "ptl/core/result.hpp"
#include "ptl/market/quote.hpp"
#include "ptl/market/symbol_mapper.hpp"

namespace ptl::market {

/// A vendor quote record, before normalisation.
///
/// Prices are FIXED-POINT integers, as Databento sends them. Converting to
/// double happens once, here, with the scale stated explicitly -- doing it at
/// each call site is how a factor-of-1000 error survives review.
struct RawQuote {
    std::uint32_t vendor_instrument_id = 0;
    /// Nanoseconds since the Unix epoch, UTC.
    std::int64_t exchange_time_ns = 0;
    std::int64_t bid_price_fixed = 0;
    std::int64_t ask_price_fixed = 0;
    std::uint32_t bid_size = 0;
    std::uint32_t ask_size = 0;
};

struct QuoteNormaliserConfig {
    /// Databento fixed-point prices carry nine implied decimal places.
    double price_scale = 1e-9;
    /// Modelled feed latency added to exchange time to give receive time.
    Duration feed_latency{std::chrono::microseconds{500}};
    /// Drop records whose bid or ask is zero. A one-sided book is real at the
    /// very open, but it is not tradeable and cannot price a fill.
    bool drop_one_sided = true;
};

struct QuoteNormaliserStats {
    std::size_t accepted = 0;
    std::size_t dropped_unmapped = 0;
    std::size_t dropped_one_sided = 0;
    std::size_t dropped_invalid = 0;
    std::size_t dropped_crossed = 0;
};

/// Converts RawQuote records into validated Quotes.
class QuoteNormaliser {
public:
    QuoteNormaliser(const SymbolMapper& mapper, QuoteNormaliserConfig cfg = {}) noexcept
        : mapper_(&mapper), cfg_(cfg) {}

    /// \returns nullopt when the record is unmapped or unusable, with the
    ///          reason counted. A dropped record is never silently lost -- the
    ///          counts are what tell you a symbology file was incomplete.
    [[nodiscard]] std::optional<Quote> normalise(const RawQuote&);

    /// Normalise a batch, preserving order and skipping unusable records.
    [[nodiscard]] Result<std::vector<Quote>> normalise_batch(std::span<const RawQuote>);

    [[nodiscard]] const QuoteNormaliserStats& stats() const noexcept { return stats_; }
    void reset() noexcept { stats_ = QuoteNormaliserStats{}; }

private:
    const SymbolMapper* mapper_;
    QuoteNormaliserConfig cfg_;
    QuoteNormaliserStats stats_;
};

}  // namespace ptl::market
