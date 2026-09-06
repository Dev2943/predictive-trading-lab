#pragma once

/// \file session.hpp
/// Live session lifecycle and the live market data adapter.
///
/// THE PARITY CLAIM, EXTENDED TO LIVE. Replay, paper and live run the SAME
/// engine::Engine over the SAME engine::IStrategy. They differ in three objects
/// and nothing else:
///
///   - the IClock             (SimulatedClock vs wall clock)
///   - the IMarketDataSource  (ReplaySource vs LiveMarketDataAdapter)
///   - the broker             (BrokerSimulator vs PaperBroker vs LiveBroker)
///
/// Selection is DEPENDENCY INJECTION only. There is no mode flag, no `if
/// (live)` branch, and no code path that exists in one mode and not another.
/// The parity test asserts a live session driven by recorded venue messages
/// reproduces the equivalent backtest exactly.
///
/// WHAT LIVE ADDS over paper is failure handling: reconnection, heartbeat
/// supervision, and reconciliation against the venue's own account. None of it
/// touches trading logic.

#include <cstdint>
#include <deque>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "ptl/accounting/journal.hpp"
#include "ptl/core/clock.hpp"
#include "ptl/core/result.hpp"
#include "ptl/engine/engine.hpp"
#include "ptl/live/broker.hpp"
#include "ptl/market/source.hpp"
#include "ptl/storage/registry.hpp"

namespace ptl::live {

/// Feeds venue quotes and trades into the engine as market events.
///
/// Implements the EXISTING market::IMarketDataSource. It creates no new market
/// structure: a live quote becomes a market::Quote and a live trade becomes a
/// market::Trade, the same types replay produces, so everything downstream is
/// unable to tell where they came from.
class LiveMarketDataAdapter final : public market::IMarketDataSource {
public:
    LiveMarketDataAdapter(const IClock& clock, LiveConnection& connection) noexcept
        : clock_(&clock), connection_(&connection) {}

    /// Returns the next buffered event, or nullopt when the buffer is empty.
    ///
    /// Non-blocking by design: returning nullopt means "nothing right now", not
    /// "the stream has ended". A blocking read here would stall the engine loop
    /// and with it every other instrument.
    [[nodiscard]] std::optional<market::MarketEvent> next() override;

    /// Timestamp of the next buffered event, or kNoTimestamp when the buffer is
    /// empty. An empty live buffer is NOT the end of the stream, so the sentinel
    /// means "nothing yet" rather than "never again".
    [[nodiscard]] Timestamp peek_time() const noexcept override;

    [[nodiscard]] std::string_view description() const noexcept override { return "live"; }

    /// Drain the connection into the event buffer. Called by the session before
    /// stepping the engine, so message arrival and event dispatch stay on one
    /// thread in a defined order.
    [[nodiscard]] Result<std::size_t> pump();

    [[nodiscard]] std::size_t buffered() const noexcept { return buffer_.size(); }
    [[nodiscard]] std::size_t dropped_stale() const noexcept { return dropped_stale_; }

private:
    const IClock* clock_;
    LiveConnection* connection_;
    std::deque<market::MarketEvent> buffer_;
    Timestamp last_event_time_{kNoTimestamp};
    std::size_t dropped_stale_ = 0;
};

enum class LiveSessionPhase : std::uint8_t {
    Created,
    Connecting,
    Reconciling,
    Running,
    /// Connection lost; the engine is idle but state is intact and a reconnect
    /// is being attempted.
    Recovering,
    /// Market is closed. Not an error, and not a reason to disconnect.
    Halted,
    Stopping,
    Stopped,
    Failed,
};

[[nodiscard]] std::string_view to_string(LiveSessionPhase) noexcept;
[[nodiscard]] bool is_terminal(LiveSessionPhase) noexcept;

struct LiveSessionConfig {
    std::string session_id = "live";
    std::uint64_t config_fingerprint = 0;

    std::size_t persist_every_events = 50;
    /// Reconcile against the venue every N events. Frequent enough to catch a
    /// drift while it is still one order, rare enough not to dominate the loop.
    std::size_t reconcile_every_events = 500;
    /// Halt trading when reconciliation finds a drift. On by default: a book
    /// that disagrees with the venue is exactly when NOT to send more orders.
    bool halt_on_reconciliation_failure = true;
    /// Refuse to trade outside regular hours.
    bool regular_hours_only = true;
};

struct LiveSessionStats {
    std::size_t events_processed = 0;
    std::size_t orders_submitted = 0;
    std::size_t fills_received = 0;
    std::size_t reconnects = 0;
    std::size_t reconciliations = 0;
    std::size_t reconciliation_failures = 0;
    std::size_t persists = 0;
    std::size_t halts = 0;
    Timestamp started_at{kNoTimestamp};
    Timestamp stopped_at{kNoTimestamp};

    [[nodiscard]] std::string describe() const;
};

/// Persisted live state, sufficient to resume without corrupting the book.
struct LiveSessionState {
    std::string session_id;
    std::uint64_t sequence = 0;
    Timestamp saved_at{kNoTimestamp};
    LiveSessionPhase phase{LiveSessionPhase::Running};
    std::uint64_t config_fingerprint = 0;

    std::size_t events_processed = 0;
    /// Highest venue sequence applied. Restored so redelivered messages are
    /// still recognised as duplicates after a restart.
    std::uint64_t last_venue_sequence = 0;
    Timestamp last_sync_point{kNoTimestamp};

    BrokerAccountSnapshot account;
    /// client order id -> our order id, for orders still working at the venue.
    std::vector<std::pair<std::string, std::uint64_t>> tracked_orders;

    /// Checksum over the raw bits. A corrupt state is refused, not resumed
    /// from; resuming a live book from damaged data is the worst outcome
    /// available.
    [[nodiscard]] std::uint64_t checksum() const;
    [[nodiscard]] std::string to_json() const;
    [[nodiscard]] static Result<LiveSessionState> from_json(std::string_view);
};

/// Orchestrates a live session.
///
/// Owns the operational envelope; BORROWS every trading component. It drives an
/// Engine rather than dispatching events itself, exactly as PaperSession does.
class LiveSession {
public:
    LiveSession(LiveSessionConfig config, IClock& clock, LiveConnection& connection,
                LiveMarketDataAdapter& market_data, LiveBroker& broker, engine::IStrategy& strategy,
                execution::BrokerSimulator& simulator, portfolio::Portfolio& portfolio,
                oms::OrderManager& oms, risk::RiskManager& risk, accounting::Journal& journal,
                storage::ArtifactStore& artifacts, const market::Calendar* calendar = nullptr);

    [[nodiscard]] Result<bool> start();
    [[nodiscard]] Result<std::size_t> step(std::size_t max_events = 1);
    [[nodiscard]] Result<LiveSessionStats> run(std::size_t max_iterations = 0);

    /// Halt trading without disconnecting. Used for a market close or a failed
    /// reconciliation; data keeps flowing so the book stays marked.
    void halt(std::string reason);
    [[nodiscard]] Result<bool> resume();

    void request_stop() noexcept { stop_requested_ = true; }
    [[nodiscard]] Result<bool> persist();
    [[nodiscard]] Result<bool> shutdown();

    /// Compare against the venue and act on the result.
    [[nodiscard]] Result<ReconciliationReport> reconcile();

    [[nodiscard]] LiveSessionPhase phase() const noexcept { return phase_; }
    [[nodiscard]] const LiveSessionStats& stats() const noexcept { return stats_; }
    [[nodiscard]] const std::string& halt_reason() const noexcept { return halt_reason_; }
    [[nodiscard]] bool trading_permitted() const noexcept;
    [[nodiscard]] std::uint64_t content_hash() const noexcept { return content_hash_; }
    [[nodiscard]] LiveSessionState capture() const;

private:
    [[nodiscard]] Result<bool> recover();

    LiveSessionConfig config_;
    IClock* clock_;
    LiveConnection* connection_;
    LiveMarketDataAdapter* market_data_;
    LiveBroker* broker_;
    engine::IStrategy* strategy_;
    execution::BrokerSimulator* simulator_;
    portfolio::Portfolio* portfolio_;
    oms::OrderManager* oms_;
    risk::RiskManager* risk_;
    accounting::Journal* journal_;
    storage::ArtifactStore* artifacts_;
    const market::Calendar* calendar_;

    std::optional<engine::Engine> engine_;
    LiveSessionPhase phase_{LiveSessionPhase::Created};
    LiveSessionStats stats_;
    std::string halt_reason_;
    bool stop_requested_ = false;
    std::uint64_t sequence_ = 0;
    std::uint64_t content_hash_ = 0xcbf29ce484222325ULL;
    Timestamp last_event_{kNoTimestamp};
    Timestamp last_sync_{kNoTimestamp};
};

}  // namespace ptl::live
