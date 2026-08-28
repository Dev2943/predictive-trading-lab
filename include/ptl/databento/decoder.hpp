#pragma once

/// \file decoder.hpp
/// Databento record decoding.
///
/// Decodes the JSON representation of Databento records. The binary DBN format
/// is deliberately NOT implemented here: it is a versioned wire format whose
/// layout must match the vendor's exactly, and a hand-rolled struct-cast that
/// is subtly wrong produces plausible numbers rather than an error. When binary
/// decoding is needed, the vendor's own library is the right tool, and this
/// decoder's interface is what it would slot behind.
///
/// Every decoder returns fully validated domain objects. A record that cannot
/// be decoded is an error at the point of parsing, where the record index is
/// still known.

#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

#include "ptl/core/result.hpp"
#include "ptl/market/bar.hpp"
#include "ptl/market/corporate_action.hpp"
#include "ptl/market/quote_stream.hpp"
#include "ptl/market/trade.hpp"

namespace ptl::databento {

/// Databento schemas this decoder understands.
enum class Schema : std::uint8_t {
    /// Consolidated BBO, one-minute sampling. ADR-0001 T2.
    Cbbo1m,
    /// Consolidated BBO, one-second sampling. ADR-0001 T3.
    Cbbo1s,
    /// One-minute OHLCV bars.
    Ohlcv1m,
    /// Individual trades.
    Trades,
    /// Instrument definitions.
    Definition,
    /// Corporate actions.
    CorporateActions,
};

[[nodiscard]] std::string_view to_string(Schema) noexcept;

/// Parse a schema name, rejecting the publisher-scoped `bbo-*` variants.
///
/// ADR-0001 is explicit that `cbbo-*` (consolidated) is required and `bbo-*`
/// (single publisher) is not a substitute. Accepting `bbo-1m` here would give
/// single-venue data the appearance of consolidated coverage, which is the
/// exact confusion the entitlement gate exists to prevent.
[[nodiscard]] Result<Schema> parse_schema(std::string_view);

/// An instrument definition record.
struct InstrumentDefinition {
    std::uint32_t instrument_id = 0;
    std::string raw_symbol;
    std::string exchange;
    std::string asset_class;
    double min_price_increment = 0.01;
    std::uint32_t lot_size = 1;
};

struct DecoderStats {
    std::size_t records_read = 0;
    std::size_t records_decoded = 0;
    std::size_t records_rejected = 0;
};

/// Decodes Databento JSON payloads into domain objects.
class Decoder {
public:
    Decoder(const market::SymbolMapper& mapper, market::QuoteNormaliserConfig cfg = {})
        : normaliser_(mapper, cfg), mapper_(&mapper) {}

    /// Decode a cbbo-1m or cbbo-1s payload into validated Quotes.
    [[nodiscard]] Result<std::vector<market::Quote>> decode_quotes(std::string_view json);

    /// Decode an ohlcv-1m payload.
    ///
    /// Databento stamps OHLCV records with the interval's OPEN, like Alpaca, so
    /// bars are built through Bar::from_left_edge. Treating that stamp as a
    /// close would be a one-minute lookahead introduced by the decoder.
    [[nodiscard]] Result<std::vector<market::Bar>> decode_bars(std::string_view json,
                                                               Duration timeframe);

    [[nodiscard]] Result<std::vector<market::Trade>> decode_trades(std::string_view json);

    [[nodiscard]] Result<std::vector<InstrumentDefinition>> decode_definitions(
        std::string_view json);

    [[nodiscard]] Result<std::vector<market::CorporateAction>> decode_corporate_actions(
        std::string_view json);

    [[nodiscard]] const DecoderStats& stats() const noexcept { return stats_; }
    [[nodiscard]] const market::QuoteNormaliserStats& normaliser_stats() const noexcept {
        return normaliser_.stats();
    }
    void reset() noexcept;

private:
    market::QuoteNormaliser normaliser_;
    const market::SymbolMapper* mapper_;
    DecoderStats stats_;
};

}  // namespace ptl::databento
