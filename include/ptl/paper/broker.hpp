#pragma once

/// \file broker.hpp
/// The paper broker adapter.
///
/// WHAT THIS IS NOT: a fill engine. `execution::BrokerSimulator` already
/// matches orders against quotes, applies the ADR-0003 conservative fill model,
/// handles IOC/FOK and stop triggering, and produces every Fill in the system.
/// Reimplementing any of that would create a second execution model, and the
/// two would diverge silently.
///
/// What the adapter ADDS is the shape a brokerage has and a simulator does not:
/// asynchronous acknowledgement, venue-side rejection independent of our own
/// risk gate, and a queryable order lifecycle. A backtest gets fills
/// synchronously; a paper account gets an ack first and a fill later, and
/// strategy code must not be able to tell the difference.
///
/// Fill's constructor stays private to BrokerSimulator. The adapter ROUTES
/// fills and never creates one, so the Phase 3 guarantee that every dollar
/// traces to a single origin survives Phase 14 intact.

#include <chrono>
#include <cstdint>
#include <deque>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "ptl/core/clock.hpp"
#include "ptl/core/result.hpp"
#include "ptl/execution/broker.hpp"
#include "ptl/oms/fill.hpp"
#include "ptl/oms/order.hpp"
#include "ptl/paper/account.hpp"

namespace ptl::paper {

enum class AckStatus : std::uint8_t { Accepted, Rejected, Pending };

[[nodiscard]] std::string_view to_string(AckStatus) noexcept;

struct OrderAck {
    oms::OrderId order_id{oms::kNoOrder};
    AckStatus status{AckStatus::Pending};
    /// When the venue would have replied. Later than submission: no real venue
    /// acknowledges instantaneously, and a simulator that does makes every
    /// latency measurement optimistic.
    Timestamp acknowledged_at{kNoTimestamp};
    std::string reject_reason;

    [[nodiscard]] bool accepted() const noexcept { return status == AckStatus::Accepted; }
};

/// Why the VENUE refused. Distinct from a risk rejection: our risk gate and the
/// broker's limits are different policies, and a session that cannot tell them
/// apart cannot tell whether to fix its limits or call the broker.
enum class RejectReason : std::uint8_t {
    None,
    NotConnected,
    ExceedsVenueLimit,
    InsufficientBuyingPower,
    MarginCall,
    ShortNotPermitted,
    AccountSuspended,
    SimulatorRejected,
};

[[nodiscard]] std::string_view to_string(RejectReason) noexcept;

struct PaperBrokerConfig {
    Duration ack_latency{std::chrono::milliseconds{5}};
    /// Venue-side notional cap, independent of our risk limits.
    Notional max_order_notional{10'000'000.0};
    /// Deterministically refuse every Nth order, to exercise the rejection
    /// path. A session that has never seen a venue rejection has not tested the
    /// path it will meet first in production. Zero disables it.
    std::size_t reject_every_n = 0;
    bool enforce_buying_power = true;
};

struct BrokerStats {
    std::size_t submitted = 0;
    std::size_t accepted = 0;
    std::size_t rejected = 0;
    std::size_t cancelled = 0;
    std::size_t fills_routed = 0;
    std::map<std::uint8_t, std::size_t> rejections_by_reason;

    [[nodiscard]] std::string describe() const;
};

/// Adapts the Phase 3 simulator to a brokerage-shaped interface.
class PaperBroker {
public:
    /// Every reference is borrowed and must outlive the broker.
    PaperBroker(const IClock& clock, execution::BrokerSimulator& simulator,
                const PaperAccount& account, PaperBrokerConfig config = {}) noexcept;

    [[nodiscard]] Result<bool> connect();
    void disconnect() noexcept;
    [[nodiscard]] bool connected() const noexcept { return connected_; }

    /// Submit an order. Returns an ACK, not a fill.
    [[nodiscard]] Result<OrderAck> submit(const oms::Order&);
    [[nodiscard]] Result<bool> cancel(oms::OrderId);

    /// Hand the simulator's fills to the adapter queue, so they reach the
    /// strategy through the same poll a real venue would require.
    void route(std::vector<oms::Fill>);

    /// Drain fills. A PULL, not a callback: a callback would run on whatever
    /// thread the venue chose, and determinism rests on one thread draining
    /// events in order.
    [[nodiscard]] std::vector<oms::Fill> poll_fills();

    [[nodiscard]] std::vector<oms::OrderId> working_orders() const;
    [[nodiscard]] const BrokerStats& stats() const noexcept { return stats_; }
    [[nodiscard]] const PaperBrokerConfig& config() const noexcept { return config_; }

private:
    [[nodiscard]] RejectReason screen(const oms::Order&) const;

    const IClock* clock_;
    execution::BrokerSimulator* simulator_;
    const PaperAccount* account_;
    PaperBrokerConfig config_;
    bool connected_ = false;
    BrokerStats stats_;
    std::deque<oms::Fill> pending_;
    std::map<std::uint64_t, bool> working_;
};

}  // namespace ptl::paper
