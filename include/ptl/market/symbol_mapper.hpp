#pragma once

/// \file symbol_mapper.hpp
/// Vendor symbol to InstrumentId, with time-ranged validity.
///
/// A symbol is not a stable identity. Tickers are reused after delisting, and a
/// vendor's instrument id can be remapped across dataset versions. Mapping
/// without a date range is how a backtest ends up attributing one company's
/// prices to another's history -- rare, silent, and devastating when it
/// happens.
///
/// Every mapping therefore carries a validity window, and a lookup requires the
/// instant it applies to.

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "ptl/core/instrument_table.hpp"
#include "ptl/core/result.hpp"
#include "ptl/core/types.hpp"

namespace ptl::market {

/// One vendor symbol valid over a half-open time range.
struct SymbolMapping {
    std::string raw_symbol;
    InstrumentId instrument{kInvalidInstrument};
    /// Vendor-native numeric id, when the feed uses one (Databento does).
    std::uint32_t vendor_id = 0;
    Timestamp valid_from{kNoTimestamp};
    /// Half-open: valid while ts < valid_to. kMaxTimestamp means open-ended.
    Timestamp valid_to{kMaxTimestamp};

    [[nodiscard]] bool covers(Timestamp ts) const noexcept {
        return ts >= valid_from && ts < valid_to;
    }
};

class SymbolMapper {
public:
    /// \param instruments borrowed; symbols are interned through it so ids stay
    ///        consistent with the rest of the system.
    explicit SymbolMapper(InstrumentTable& instruments) noexcept : instruments_(&instruments) {}

    /// Register a mapping. Overlapping windows for the same raw symbol are
    /// REFUSED: two live mappings mean a lookup would have to pick one, and
    /// picking silently is how the wrong instrument gets traded.
    [[nodiscard]] Result<InstrumentId> add(const SymbolMapping&);

    /// Convenience for the common open-ended case.
    [[nodiscard]] Result<InstrumentId> add_static(std::string raw_symbol,
                                                  std::uint32_t vendor_id = 0);

    [[nodiscard]] std::optional<InstrumentId> resolve(std::string_view raw_symbol,
                                                      Timestamp as_of) const;

    /// Databento streams carry a numeric instrument id; the raw symbol arrives
    /// separately in the symbology mapping.
    [[nodiscard]] std::optional<InstrumentId> resolve_vendor_id(std::uint32_t vendor_id,
                                                                Timestamp as_of) const;

    [[nodiscard]] std::optional<std::string_view> raw_symbol_of(InstrumentId,
                                                                Timestamp as_of) const;

    [[nodiscard]] std::size_t size() const noexcept { return mappings_.size(); }
    [[nodiscard]] const std::vector<SymbolMapping>& mappings() const noexcept { return mappings_; }

    void reset() noexcept;

private:
    InstrumentTable* instruments_;
    std::vector<SymbolMapping> mappings_;
};

}  // namespace ptl::market
