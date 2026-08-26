#pragma once

/// \file journal.hpp
/// The transaction journal and the gross-to-net reconciliation.
///
/// Append-only, chronological, and the single source of truth for what
/// happened. Phase 12 proves paper-trading parity by diffing a live journal
/// against a replayed one, so every entry must be a pure function of the
/// simulation -- no wall-clock timestamps, no hash-ordered iteration.
///
/// The reconciliation identity it enforces:
///
///     net = gross - spread - impact/slippage - fees/commissions
///           - financing - other
///
/// Attribution that does not sum to the reported net P&L is not attribution,
/// it is a plausible-looking table.

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "ptl/core/result.hpp"
#include "ptl/core/types.hpp"
#include "ptl/oms/fill.hpp"
#include "ptl/oms/order_manager.hpp"
#include "ptl/portfolio/portfolio.hpp"

namespace ptl::accounting {

enum class EntryKind : std::uint8_t {
    OrderSubmitted,
    OrderRejected,
    OrderCancelled,
    FillReceived,
    Dividend,
    Split,
    Mark,
    RiskRejection,
};

[[nodiscard]] std::string_view to_string(EntryKind) noexcept;

struct JournalEntry {
    Timestamp ts{kNoTimestamp};
    EntryKind kind{EntryKind::Mark};
    oms::OrderId order_id{oms::kNoOrder};
    InstrumentId instrument{kInvalidInstrument};
    Side side{Side::Buy};
    Qty quantity{};
    Price price{};
    Notional cash_delta{};
    Notional cost{};
    std::string detail;

    [[nodiscard]] std::string to_csv() const;
};

/// A round trip, matched from fills. Trade-level statistics need this: an
/// expectancy computed over FILLS rather than trades counts a partially filled
/// entry as several separate bets.
struct Trade {
    InstrumentId instrument{kInvalidInstrument};
    Side direction{Side::Buy};
    Timestamp opened{kNoTimestamp};
    Timestamp closed{kNoTimestamp};
    Qty quantity{};
    Price entry_price{};
    Price exit_price{};
    Notional gross_pnl{};
    Notional costs{};

    [[nodiscard]] Notional net_pnl() const noexcept { return gross_pnl - costs; }
    [[nodiscard]] bool is_win() const noexcept { return net_pnl().get() > 0.0; }
    [[nodiscard]] Duration holding_period() const noexcept { return closed - opened; }
};

/// The gross-to-net bridge. Every component is signed as a COST, so
/// net = gross - sum(components).
struct Reconciliation {
    Notional gross_pnl{};
    Notional spread_cost{};
    Notional impact_and_slippage{};
    Notional commissions_and_fees{};
    Notional financing{};
    Notional other{};
    Notional net_pnl{};
    Notional reported_net{};

    [[nodiscard]] Notional residual() const noexcept { return net_pnl - reported_net; }
    [[nodiscard]] bool balances(double tolerance = 1e-6) const noexcept;
    [[nodiscard]] std::string describe() const;
};

class Journal {
public:
    /// \throws nothing; a backwards timestamp is an error value, not a crash.
    [[nodiscard]] Result<bool> append(const JournalEntry& entry);

    void record_fill(const oms::Fill& fill);
    void record_risk_rejection(Timestamp ts, InstrumentId instrument, std::string reason);

    [[nodiscard]] std::span<const JournalEntry> entries() const noexcept { return entries_; }
    [[nodiscard]] std::span<const Trade> trades() const noexcept { return trades_; }
    [[nodiscard]] std::size_t size() const noexcept { return entries_.size(); }

    /// Match fills into round trips. Weighted-average matching, consistent with
    /// the position accounting, so trade P&L sums to realised P&L.
    void match_trades();

    /// Build the bridge and check that it balances against the portfolio.
    [[nodiscard]] Reconciliation reconcile(const portfolio::Portfolio& pf) const;

    /// Stable CSV, for the run artifacts and for a parity diff.
    [[nodiscard]] std::string to_csv() const;

    void reset() noexcept;

private:
    std::vector<JournalEntry> entries_;
    std::vector<oms::Fill> fills_;
    std::vector<Trade> trades_;
};

}  // namespace ptl::accounting
