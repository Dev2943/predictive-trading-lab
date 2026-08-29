#pragma once

/// \file quote_router.hpp
/// Converts quote-book state into the MarketState the simulator consumes.
///
/// One place, one conversion. Before Phase 8 the broker synthesised a spread
/// from bar ranges whenever no quote existed; now a real quote populates
/// MarketState and the synthetic path is reserved for bar-only mode and
/// LABELLED as such. Keeping the conversion here means the broker never has to
/// ask where its prices came from.

#include <cstdint>
#include <optional>
#include <string_view>

#include "ptl/core/result.hpp"
#include "ptl/execution/models.hpp"
#include "ptl/execution/quote_book.hpp"
#include "ptl/market/bar.hpp"

namespace ptl::execution {

/// Where the prices in a MarketState came from. Recorded so a run can report
/// what fraction of its fills were priced from real quotes rather than
/// synthesised from bars.
enum class PriceSource : std::uint8_t {
    /// Real consolidated top-of-book.
    Quote,
    /// Synthesised from a bar: bid == ask == close, spread modelled.
    SyntheticFromBar,
    /// Nothing usable.
    None,
};

[[nodiscard]] std::string_view to_string(PriceSource) noexcept;

struct RoutedMarket {
    MarketState state;
    PriceSource source{PriceSource::None};
    QuoteState quote_state;
    /// True when an order may execute. Mirrors QuoteState::executable() for the
    /// quote path, and is always true for the synthetic path -- a bar carries
    /// no halt information, and that limitation is documented rather than
    /// silently assumed away.
    bool executable = false;
};

class QuoteRouter {
public:
    explicit QuoteRouter(const QuoteBook& book) noexcept : book_(&book) {}

    /// Build a MarketState from the current quote, if one is usable.
    [[nodiscard]] RoutedMarket route(InstrumentId, Timestamp now) const;

    /// Build one from a bar, for bar-only mode.
    ///
    /// The result is marked SyntheticFromBar so downstream reporting can
    /// separate fills priced from real quotes from those priced from a
    /// modelled spread.
    [[nodiscard]] static RoutedMarket route_from_bar(const market::Bar&,
                                                     double intraday_volatility);

    /// Prefer the quote; fall back to the bar. The ordering is the whole point:
    /// a real quote always beats a synthesised one, and the fallback exists so
    /// a T1-only dataset still runs.
    [[nodiscard]] RoutedMarket route_preferring_quote(const market::Bar&,
                                                      double intraday_volatility,
                                                      Timestamp now) const;

private:
    const QuoteBook* book_;
};

}  // namespace ptl::execution
