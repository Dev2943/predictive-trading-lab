#pragma once

/// \file broker.hpp
/// The live broker adapter and account reconciliation.
///
/// SAME SHAPE AS PaperBroker, DIFFERENT SOURCE OF TRUTH. Paper asks a simulator
/// what would have happened; live is TOLD what did. Both submit orders, poll
/// fills and screen against an account, so the session above them cannot tell
/// which it holds -- that is what makes replay, paper and live one pipeline.
///
/// FILLS STILL COME FROM ONE PLACE. `BrokerSimulator::ingest_external_fill`
/// constructs every live Fill after validating the venue's report against the
/// working order. This adapter never constructs a Fill, so the Phase 3
/// guarantee that every dollar traces to a single origin holds in live exactly
/// as it does in replay.

#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "ptl/core/clock.hpp"
#include "ptl/core/result.hpp"
#include "ptl/execution/broker.hpp"
#include "ptl/live/connection.hpp"
#include "ptl/live/translator.hpp"
#include "ptl/oms/fill.hpp"
#include "ptl/portfolio/portfolio.hpp"

namespace ptl::live {

/// The venue's view of the account.
///
/// Deliberately separate from portfolio::Portfolio: this is what the BROKER
/// says, and the portfolio is what WE computed. Keeping them apart is what
/// makes reconciliation possible; merging them would make a drift undetectable
/// by construction.
struct BrokerAccountSnapshot {
    Timestamp ts{kNoTimestamp};
    Notional cash{};
    Notional buying_power{};
    Notional equity{};
    Notional position_value{};
    Notional realized_pnl{};
    Notional unrealized_pnl{};
    /// Venue positions, by instrument.
    std::map<std::uint32_t, double> positions;
    std::size_t open_order_count = 0;

    [[nodiscard]] std::string to_json() const;
};

/// The result of comparing our book against the venue's.
struct ReconciliationReport {
    Timestamp ts{kNoTimestamp};
    bool cash_matches = false;
    bool positions_match = false;
    Notional cash_drift{};
    /// Instruments whose quantity differs, with our value minus theirs.
    std::map<std::uint32_t, double> position_drift;
    std::vector<std::string> notes;

    [[nodiscard]] bool clean() const noexcept {
        return cash_matches && positions_match && position_drift.empty();
    }
    [[nodiscard]] std::string describe() const;
};

struct LiveBrokerConfig {
    /// Absolute cash difference tolerated before reconciliation fails. Not
    /// zero: fees post asynchronously at most venues, and demanding exactness
    /// would fire on every ordinary session.
    double cash_tolerance = 0.01;
    /// Quantity difference tolerated per instrument. This IS zero -- a share
    /// count either matches or something is wrong, and there is no benign
    /// reason for a fractional discrepancy.
    double position_tolerance = 0.0;
    /// Refuse to trade while a reconciliation is outstanding.
    bool require_clean_reconciliation = true;
    /// Venue-side notional cap, independent of our risk limits.
    Notional max_order_notional{10'000'000.0};
};

struct LiveBrokerStats {
    std::size_t submitted = 0;
    std::size_t accepted = 0;
    std::size_t rejected = 0;
    std::size_t cancelled = 0;
    std::size_t replaced = 0;
    std::size_t fills_ingested = 0;
    std::size_t fills_refused = 0;
    std::size_t reconciliations = 0;
    std::size_t reconciliation_failures = 0;

    [[nodiscard]] std::string describe() const;
};

/// Adapts a venue connection to the shape the engine already understands.
class LiveBroker {
public:
    /// Every reference is borrowed and must outlive the broker. No singleton:
    /// two sessions must be able to hold two brokers without sharing a socket.
    LiveBroker(const IClock& clock, LiveConnection& connection, const IOrderTranslator& translator,
               execution::BrokerSimulator& simulator, const portfolio::Portfolio& portfolio,
               LiveBrokerConfig config = {});

    /// Submit an order to the venue.
    [[nodiscard]] Result<bool> submit(const oms::Order&);
    [[nodiscard]] Result<bool> cancel(oms::OrderId);
    [[nodiscard]] Result<bool> replace(const oms::Order& amended);

    /// Drain the connection, apply order updates, and construct any fills the
    /// venue reported. A PULL, so ordering is decided by one loop rather than
    /// by whichever callback fired first.
    [[nodiscard]] Result<std::vector<oms::Fill>> poll();

    /// Compare our portfolio against the venue's account.
    [[nodiscard]] ReconciliationReport reconcile(const BrokerAccountSnapshot&) const;
    /// Record a venue account snapshot, from an AccountUpdate message.
    void observe_account(BrokerAccountSnapshot);
    [[nodiscard]] const BrokerAccountSnapshot& last_account() const noexcept { return account_; }

    [[nodiscard]] const LiveBrokerStats& stats() const noexcept { return stats_; }
    [[nodiscard]] LiveOrderListener& listener() noexcept { return listener_; }
    [[nodiscard]] const LiveOrderListener& listener() const noexcept { return listener_; }
    [[nodiscard]] std::vector<oms::OrderId> working_orders() const;
    /// Venue order id for one of our orders, once assigned.
    [[nodiscard]] std::string venue_id_of(oms::OrderId) const;

private:
    const IClock* clock_;
    LiveConnection* connection_;
    const IOrderTranslator* translator_;
    execution::BrokerSimulator* simulator_;
    const portfolio::Portfolio* portfolio_;
    LiveBrokerConfig config_;

    LiveOrderListener listener_;
    LiveBrokerStats stats_;
    BrokerAccountSnapshot account_;
    std::map<std::uint64_t, std::string> venue_ids_;
    std::map<std::uint64_t, bool> working_;
};

}  // namespace ptl::live
