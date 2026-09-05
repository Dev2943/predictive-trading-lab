#pragma once

/// \file session.hpp
/// Paper trading session lifecycle.
///
/// THE PARITY CLAIM, STATED PRECISELY. A paper session and a backtest run the
/// SAME engine::Engine over the SAME engine::IStrategy. They differ in exactly
/// two objects:
///
///   - the IClock            (wall clock vs SimulatedClock)
///   - the IMarketDataSource (paper feed vs ReplaySource)
///
/// Everything else -- features, signals, sizing, optimization, risk, OMS,
/// execution algorithms, fill simulation, portfolio, accounting, analytics --
/// is the identical code path. There is no `if (paper)` branch in this file or
/// below it, and the parity test asserts a paper session reproduces a backtest
/// exactly: same orders, same fills, same equity, same journal.
///
/// What a session ADDS is the operational envelope a backtest does not need:
/// start/stop/pause/resume, persistence, recovery after restart, daily reset.
/// None of it touches trading logic.

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "ptl/accounting/journal.hpp"
#include "ptl/core/clock.hpp"
#include "ptl/core/result.hpp"
#include "ptl/engine/engine.hpp"
#include "ptl/market/source.hpp"
#include "ptl/paper/broker.hpp"
#include "ptl/storage/registry.hpp"

namespace ptl::paper {

enum class SessionPhase : std::uint8_t {
    Created,
    Starting,
    Running,
    /// Market data keeps flowing so the book stays marked; only ORDERS stop.
    /// Halting data too would leave a hole in the equity curve and the
    /// positions mismarked on resume.
    Paused,
    Stopping,
    Stopped,
    Failed,
};

[[nodiscard]] std::string_view to_string(SessionPhase) noexcept;
[[nodiscard]] bool is_terminal(SessionPhase) noexcept;

struct SessionConfig {
    std::string session_id = "paper";
    /// Fingerprint of the configuration this session runs under. Resuming into
    /// a different one silently mixes two runs.
    std::uint64_t config_fingerprint = 0;

    /// Persist every N events: often enough that a crash loses little, rarely
    /// enough that the write does not dominate the loop.
    std::size_t persist_every_events = 100;

    /// Roll daily counters at a session boundary.
    bool daily_reset = true;
    /// Refuse to resume under a different configuration.
    bool require_matching_config = true;

    MarginConfig margin;
    PaperBrokerConfig broker;
};

/// Persisted session state, sufficient to resume without corruption.
struct SessionState {
    std::string session_id;
    std::uint64_t sequence = 0;
    Timestamp saved_at{kNoTimestamp};
    SessionPhase phase{SessionPhase::Running};
    std::uint64_t config_fingerprint = 0;

    std::size_t events_processed = 0;
    Timestamp last_event_time{kNoTimestamp};

    AccountSnapshot account;

    struct PersistedPosition {
        std::uint32_t instrument = 0;
        double quantity = 0.0;
        double average_cost = 0.0;
        double realized_pnl = 0.0;
    };
    std::vector<PersistedPosition> positions;

    struct PersistedOrder {
        std::uint64_t order_id = 0;
        std::uint32_t instrument = 0;
        std::uint8_t side = 0;
        double quantity = 0.0;
        double filled = 0.0;
    };
    std::vector<PersistedOrder> working_orders;

    std::size_t fills_received = 0;
    std::size_t orders_submitted = 0;

    /// Checksum over the raw bits of everything above. A truncated or corrupt
    /// file is detected on load rather than resumed from: resuming a book from
    /// damaged data is worse than refusing, because the run would look complete.
    ///
    /// This is why every numeric field must serialize ROUND-TRIP EXACT.
    [[nodiscard]] std::uint64_t checksum() const;
    [[nodiscard]] std::string to_json() const;
    [[nodiscard]] static Result<SessionState> from_json(std::string_view);
};

struct SessionStats {
    std::size_t events_processed = 0;
    std::size_t orders_submitted = 0;
    std::size_t orders_rejected = 0;
    std::size_t fills_received = 0;
    std::size_t persists = 0;
    std::size_t pauses = 0;
    std::size_t daily_resets = 0;
    Timestamp started_at{kNoTimestamp};
    Timestamp stopped_at{kNoTimestamp};

    [[nodiscard]] std::string describe() const;
};

/// Orchestrates a paper session.
///
/// Owns the operational envelope; BORROWS every trading component. It drives an
/// Engine rather than dispatching events itself.
class PaperSession {
public:
    /// Every reference is borrowed and must outlive the session.
    PaperSession(SessionConfig config, IClock& clock, market::IMarketDataSource& source,
                 engine::IStrategy& strategy, execution::BrokerSimulator& simulator,
                 PaperBroker& broker, portfolio::Portfolio& portfolio, oms::OrderManager& oms,
                 risk::RiskManager& risk, accounting::Journal& journal,
                 storage::ArtifactStore& artifacts, const market::Calendar* calendar = nullptr);

    [[nodiscard]] Result<bool> start();

    /// Process up to `max_events`. Exposed so a caller can drive the session on
    /// its own cadence -- a real deployment needs a timer, because a stalled
    /// feed is exactly when nothing arrives to drive the loop.
    [[nodiscard]] Result<std::size_t> step(std::size_t max_events = 1);
    [[nodiscard]] Result<SessionStats> run();

    /// Pause stops ORDERS, not data.
    void pause(std::string reason);
    [[nodiscard]] Result<bool> resume();

    /// Safe from a signal handler: sets a flag the loop notices next iteration.
    void request_stop() noexcept { stop_requested_ = true; }

    [[nodiscard]] Result<bool> persist();
    [[nodiscard]] Result<bool> daily_reset(Timestamp);
    [[nodiscard]] Result<bool> shutdown();

    [[nodiscard]] SessionPhase phase() const noexcept { return phase_; }
    [[nodiscard]] const SessionStats& stats() const noexcept { return stats_; }
    [[nodiscard]] const std::string& pause_reason() const noexcept { return pause_reason_; }
    [[nodiscard]] bool trading_permitted() const noexcept {
        return phase_ == SessionPhase::Running;
    }
    [[nodiscard]] AccountSnapshot account_snapshot() const;

    /// Hash over every event consumed, for parity comparison.
    [[nodiscard]] std::uint64_t content_hash() const noexcept { return content_hash_; }

    /// Capture state without writing it, for testing recovery without the
    /// filesystem.
    [[nodiscard]] SessionState capture() const;

private:
    [[nodiscard]] Result<bool> recover();

    SessionConfig config_;
    IClock* clock_;
    market::IMarketDataSource* source_;
    engine::IStrategy* strategy_;
    execution::BrokerSimulator* simulator_;
    PaperBroker* broker_;
    portfolio::Portfolio* portfolio_;
    oms::OrderManager* oms_;
    risk::RiskManager* risk_;
    accounting::Journal* journal_;
    storage::ArtifactStore* artifacts_;
    const market::Calendar* calendar_;

    PaperAccount account_;
    std::optional<engine::Engine> engine_;

    SessionPhase phase_{SessionPhase::Created};
    SessionStats stats_;
    std::string pause_reason_;
    bool stop_requested_ = false;
    std::uint64_t sequence_ = 0;
    std::uint64_t content_hash_ = 0xcbf29ce484222325ULL;
    Timestamp last_event_{kNoTimestamp};
    Timestamp current_session_date_{kNoTimestamp};
};

}  // namespace ptl::paper
