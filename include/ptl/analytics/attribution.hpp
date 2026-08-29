#pragma once

/// \file attribution.hpp
/// Performance attribution and trade analytics.
///
/// ATTRIBUTION MUST RECONCILE. A table of contributions that does not sum to
/// the reported P&L is not attribution -- it is a plausible-looking table. Every
/// breakdown here carries a residual, and the tests assert it is zero.
///
/// The same principle the Phase 3 gross-to-net bridge follows, applied one
/// dimension further: not just what the costs were, but which instruments,
/// sectors, strategies and algorithms incurred them.

#include <cstdint>
#include <map>
#include <span>
#include <string>
#include <vector>

#include "ptl/accounting/journal.hpp"
#include "ptl/core/result.hpp"
#include "ptl/core/types.hpp"
#include "ptl/oms/fill.hpp"

namespace ptl::analytics {

/// One line of an attribution table.
struct AttributionEntry {
    std::string key;
    Notional gross_pnl{};
    Notional costs{};
    Notional net_pnl{};
    Notional turnover{};
    std::size_t trades = 0;
    std::size_t fills = 0;
    /// Share of total net P&L. Signed, and can exceed 1 when other lines lost
    /// money -- which is informative rather than an error.
    double contribution_share = 0.0;
};

/// A complete breakdown along one dimension.
struct AttributionTable {
    std::string dimension;
    /// Ordered by key, so two runs produce identical tables.
    std::map<std::string, AttributionEntry, std::less<>> entries;
    Notional total_gross{};
    Notional total_costs{};
    Notional total_net{};

    /// Reported net minus the sum of the lines. Must be zero.
    [[nodiscard]] Notional residual(Notional reported_net) const noexcept;
    [[nodiscard]] bool reconciles(Notional reported_net, double tolerance = 1e-6) const noexcept;
    /// Lines sorted by net contribution, largest first.
    [[nodiscard]] std::vector<AttributionEntry> ranked() const;
    [[nodiscard]] std::string describe() const;
};

/// Metadata a fill needs before it can be attributed.
///
/// Supplied by the caller because the analytics layer does not know which
/// strategy or algorithm produced an order -- and inventing an attribution it
/// cannot observe would be worse than declining to attribute.
struct FillAttribution {
    InstrumentId instrument{kInvalidInstrument};
    std::int32_t sector = -1;
    std::string strategy;
    std::string algorithm;
};

class AttributionAnalyzer {
public:
    /// Register how a fill should be attributed. Fills without an entry are
    /// counted under "unattributed" rather than dropped -- a missing mapping
    /// must be visible, not silent.
    void map_instrument(InstrumentId, FillAttribution);

    /// Attribute round-trip trades by instrument.
    [[nodiscard]] Result<AttributionTable> by_instrument(std::span<const accounting::Trade>) const;

    [[nodiscard]] Result<AttributionTable> by_sector(std::span<const accounting::Trade>) const;

    /// By strategy and by execution algorithm. Both take fills rather than
    /// trades, because a round trip can span several algorithms and attributing
    /// the whole trade to one of them would be arbitrary.
    [[nodiscard]] Result<AttributionTable> by_strategy(std::span<const oms::Fill>) const;
    [[nodiscard]] Result<AttributionTable> by_algorithm(std::span<const oms::Fill>) const;

    /// Costs split into their components across all fills.
    [[nodiscard]] Result<AttributionTable> by_cost_component(std::span<const oms::Fill>) const;

    [[nodiscard]] std::size_t mapped_instruments() const noexcept { return map_.size(); }
    void reset() noexcept { map_.clear(); }

private:
    std::map<std::uint32_t, FillAttribution> map_;
};

// ---------------------------------------------------------------------------
// Trade analytics
// ---------------------------------------------------------------------------

/// Round-trip statistics.
///
/// Computed over TRADES, not fills: an expectancy over fills counts a partially
/// filled entry as several separate bets and flatters the win rate.
struct TradeStatistics {
    std::size_t trades = 0;
    std::size_t wins = 0;
    std::size_t losses = 0;
    std::size_t scratches = 0;

    double win_rate = 0.0;
    Notional average_win{};
    Notional average_loss{};
    Notional largest_win{};
    Notional largest_loss{};
    Notional expectancy{};
    double profit_factor = 0.0;
    double win_loss_ratio = 0.0;

    Duration average_holding_period{Duration::zero()};
    Duration median_holding_period{Duration::zero()};
    Duration longest_holding_period{Duration::zero()};
    Duration average_winner_duration{Duration::zero()};
    Duration average_loser_duration{Duration::zero()};

    Notional gross_profit{};
    Notional gross_loss{};
    Notional total_costs{};

    /// Longest runs of consecutive wins and losses. A strategy with a 55% hit
    /// rate and a twelve-loss streak is a different proposition from one with
    /// the same rate and a three-loss streak.
    std::size_t max_consecutive_wins = 0;
    std::size_t max_consecutive_losses = 0;

    [[nodiscard]] std::string describe() const;
};

class TradeAnalyzer {
public:
    [[nodiscard]] Result<TradeStatistics> analyze(std::span<const accounting::Trade>) const;
};

}  // namespace ptl::analytics
