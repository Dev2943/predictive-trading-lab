#pragma once

/// \file symbols.hpp
/// Databento symbology: raw symbols to numeric instrument ids over date ranges.
///
/// Databento streams carry a numeric `instrument_id`, and the mapping from that
/// number to a ticker is dataset- and date-specific. It is delivered separately
/// as a symbology response. Loading it is not optional: without it, every
/// record in a stream is unattributable.

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "ptl/core/result.hpp"
#include "ptl/market/symbol_mapper.hpp"

namespace ptl::databento {

/// One interval of a Databento symbology response.
struct SymbologyInterval {
    std::string raw_symbol;
    std::uint32_t instrument_id = 0;
    /// Inclusive start date, YYYY-MM-DD.
    std::string start_date;
    /// EXCLUSIVE end date. Databento's `d1` is exclusive, and treating it as
    /// inclusive silently extends every mapping by a day -- which matters
    /// exactly when a ticker is remapped.
    std::string end_date;
};

/// Parse a Databento symbology JSON payload.
///
/// Shape: {"result": {"SPY": [{"d0": "2024-01-01", "d1": "2024-02-01",
///                             "s": "12345"}]}, ...}
[[nodiscard]] Result<std::vector<SymbologyInterval>> parse_symbology(std::string_view json);

/// Load parsed intervals into a SymbolMapper.
///
/// \returns the number of mappings added. Overlapping intervals for one symbol
///          are refused by the mapper, so a malformed symbology response fails
///          here rather than producing ambiguous lookups later.
[[nodiscard]] Result<std::size_t> load_symbology(market::SymbolMapper&,
                                                 std::span<const SymbologyInterval>);

}  // namespace ptl::databento
