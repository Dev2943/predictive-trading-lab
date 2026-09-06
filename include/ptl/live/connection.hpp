#pragma once

/// \file connection.hpp
/// Connection lifecycle, heartbeat and reconnection policy.
///
/// THE ONLY PLACE A NETWORK EXISTS is behind `ILiveTransport`. Everything above
/// it -- the broker adapter, the order translator, the session -- is a pure
/// function of the messages it receives, which is what makes live trading
/// testable without a socket and deterministic when replayed.
///
/// NO THREADS. A real broker SDK pushes callbacks from its own thread; this
/// module refuses that shape. The transport queues messages and the session
/// DRAINS them on its own loop, so ordering is decided by one thread reading a
/// queue rather than by whichever callback happened to fire first. Phase 0's
/// no-threads invariant survives Phase 15 intact, and a live session replays
/// exactly.
///
/// NO SINGLETON. Connections are objects the caller owns. A global one would
/// make two sessions share a socket and a reconnect in one tear down the other.

#include <chrono>
#include <cstdint>
#include <deque>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "ptl/core/clock.hpp"
#include "ptl/core/result.hpp"
#include "ptl/core/types.hpp"

namespace ptl::live {

enum class ConnectionState : std::uint8_t {
    Disconnected,
    Connecting,
    Connected,
    /// Connected but not yet synchronised with the venue. Orders MUST NOT be
    /// sent in this state: the local book and the broker's may disagree, and
    /// trading on a stale book is how a reconnect doubles a position.
    Synchronizing,
    Ready,
    Degraded,  ///< heartbeat late; still connected, but trust is reduced
    Reconnecting,
    Failed,
};

[[nodiscard]] std::string_view to_string(ConnectionState) noexcept;
/// Whether orders may be sent in this state.
[[nodiscard]] bool permits_trading(ConnectionState) noexcept;

/// A message from the venue, already parsed off the wire.
///
/// One flat type rather than a variant hierarchy: the set is small, closed and
/// defined by the broker's protocol, and a variant would push a visitor into
/// every consumer for no gain.
enum class MessageKind : std::uint8_t {
    Heartbeat,
    OrderAccepted,
    OrderRejected,
    OrderCancelled,
    OrderReplaced,
    PartialFill,
    Fill,
    AccountUpdate,
    Quote,
    Trade,
    /// The venue said something this build does not understand. Kept rather
    /// than dropped: an unknown message is evidence of a protocol change, and
    /// silently discarding it hides that until something breaks.
    Unknown,
};

[[nodiscard]] std::string_view to_string(MessageKind) noexcept;

struct BrokerMessage {
    MessageKind kind{MessageKind::Unknown};
    /// Venue sequence number, for duplicate and ordering detection. Zero means
    /// the venue does not sequence this stream.
    std::uint64_t sequence = 0;
    /// The venue's clock. Distinct from our receipt time: they disagree, and
    /// which one is used changes every latency measurement.
    Timestamp venue_time{kNoTimestamp};
    Timestamp received_time{kNoTimestamp};

    /// Venue identifiers, opaque above the adapter.
    std::string venue_order_id;
    std::string venue_fill_id;
    std::string client_order_id;

    InstrumentId instrument{kInvalidInstrument};
    Side side{Side::Buy};
    Qty quantity{};
    Price price{};
    Notional commission{};
    Notional exchange_fee{};

    /// Quote fields, when kind is Quote.
    Price bid{};
    Price ask{};
    Qty bid_size{};
    Qty ask_size{};

    std::string text;  ///< reject reason or free-form venue text

    [[nodiscard]] std::string describe() const;
};

/// The network boundary. The ONLY interface in the live module that a real
/// implementation would back with a socket.
///
/// Implementations must be non-blocking: `poll` returns what has arrived and
/// never waits, because a blocking read inside the event loop would stall every
/// other instrument behind one slow stream.
class ILiveTransport {
public:
    ILiveTransport() = default;
    virtual ~ILiveTransport() = default;
    ILiveTransport(const ILiveTransport&) = delete;
    ILiveTransport& operator=(const ILiveTransport&) = delete;

protected:
    ILiveTransport(ILiveTransport&&) = default;
    ILiveTransport& operator=(ILiveTransport&&) = default;

public:
    [[nodiscard]] virtual std::string_view venue() const noexcept = 0;
    [[nodiscard]] virtual Result<bool> open() = 0;
    virtual void close() noexcept = 0;
    [[nodiscard]] virtual bool is_open() const noexcept = 0;

    /// Send an already-encoded request. The transport does not know what an
    /// order is; encoding belongs to the translator.
    [[nodiscard]] virtual Result<bool> send(std::string_view payload) = 0;

    /// Take everything that has arrived. Non-blocking.
    [[nodiscard]] virtual std::vector<BrokerMessage> poll() = 0;
};

struct HeartbeatConfig {
    /// Expected interval between venue heartbeats.
    Duration interval{std::chrono::seconds{10}};
    /// Late by more than this and the connection is Degraded.
    Duration degraded_after{std::chrono::seconds{20}};
    /// Late by more than this and it is presumed dead.
    Duration dead_after{std::chrono::seconds{45}};
};

/// Tracks heartbeat liveness.
///
/// Three states rather than two: a venue that is merely LATE is not the same as
/// one that is gone, and tearing down a healthy session over one slow heartbeat
/// is itself an outage.
class HeartbeatMonitor {
public:
    explicit HeartbeatMonitor(HeartbeatConfig config = {}) noexcept : config_(config) {}

    void record_heartbeat(Timestamp) noexcept;
    /// Any inbound message proves liveness, not just an explicit heartbeat.
    void record_activity(Timestamp) noexcept;

    [[nodiscard]] bool healthy(Timestamp now) const noexcept;
    [[nodiscard]] bool degraded(Timestamp now) const noexcept;
    [[nodiscard]] bool dead(Timestamp now) const noexcept;
    [[nodiscard]] Duration since_last(Timestamp now) const noexcept;

    [[nodiscard]] Timestamp last_heartbeat() const noexcept { return last_heartbeat_; }
    [[nodiscard]] std::size_t beats() const noexcept { return beats_; }
    [[nodiscard]] std::size_t misses() const noexcept { return misses_; }
    void note_miss() noexcept { ++misses_; }
    void reset(Timestamp now) noexcept;

private:
    HeartbeatConfig config_;
    Timestamp last_heartbeat_{kNoTimestamp};
    Timestamp last_activity_{kNoTimestamp};
    std::size_t beats_ = 0;
    std::size_t misses_ = 0;
};

struct SupervisorConfig {
    /// Delay before the first reconnect attempt.
    Duration initial_backoff{std::chrono::seconds{1}};
    /// Ceiling on the backoff. Unbounded doubling means a session that drops
    /// overnight is still asleep when the market opens.
    Duration max_backoff{std::chrono::seconds{60}};
    double backoff_multiplier = 2.0;
    /// Give up after this many consecutive failures. Zero means never.
    std::size_t max_attempts = 10;
};

/// Decides WHEN to reconnect. Deliberately does not perform the reconnect: the
/// policy is a pure function of time and failure count, which makes it testable
/// without a network and identical under replay.
class ConnectionSupervisor {
public:
    explicit ConnectionSupervisor(SupervisorConfig config = {}) noexcept
        : config_(config), backoff_(config.initial_backoff) {}

    /// Whether a reconnect should be attempted now.
    [[nodiscard]] bool should_attempt(Timestamp now) const noexcept;
    /// Record an attempt starting now; advances the backoff.
    void record_attempt(Timestamp now) noexcept;
    /// Reset after a successful connection.
    void record_success(Timestamp now) noexcept;
    void record_failure(Timestamp now) noexcept;

    [[nodiscard]] bool exhausted() const noexcept;
    [[nodiscard]] Duration current_backoff() const noexcept { return backoff_; }
    [[nodiscard]] std::size_t attempts() const noexcept { return attempts_; }
    [[nodiscard]] std::size_t successes() const noexcept { return successes_; }

private:
    SupervisorConfig config_;
    Duration backoff_;
    Timestamp next_attempt_{kNoTimestamp};
    std::size_t attempts_ = 0;
    std::size_t successes_ = 0;
};

struct ConnectionStats {
    std::size_t connects = 0;
    std::size_t disconnects = 0;
    std::size_t reconnects = 0;
    std::size_t messages_received = 0;
    std::size_t duplicates_dropped = 0;
    std::size_t out_of_order_dropped = 0;
    std::size_t unknown_messages = 0;

    [[nodiscard]] std::string describe() const;
};

/// Owns the connection state machine, heartbeat and message hygiene.
///
/// DUPLICATE AND ORDERING HYGIENE LIVES HERE, once, rather than in every
/// consumer. A venue that redelivers after a reconnect is normal; applying a
/// fill twice is not, and the defence belongs at the boundary where sequence
/// numbers are still visible.
class LiveConnection {
public:
    LiveConnection(const IClock& clock, ILiveTransport& transport, HeartbeatConfig heartbeat = {},
                   SupervisorConfig supervisor = {});

    [[nodiscard]] Result<bool> connect();
    void disconnect() noexcept;
    /// Attempt a reconnect if the supervisor permits one now.
    [[nodiscard]] Result<bool> maybe_reconnect();

    /// Drain the transport, applying duplicate and ordering filters and
    /// updating the heartbeat. Returns messages the caller should act on.
    [[nodiscard]] std::vector<BrokerMessage> poll();

    /// Re-evaluate state against the clock. Called each loop iteration so a
    /// silent venue is noticed even when no message arrives -- which is exactly
    /// the failure a message-driven check would miss.
    void tick();

    /// Mark synchronisation complete, moving Synchronizing to Ready.
    [[nodiscard]] Result<bool> mark_synchronized();

    [[nodiscard]] ConnectionState state() const noexcept { return state_; }
    [[nodiscard]] bool trading_permitted() const noexcept { return permits_trading(state_); }
    [[nodiscard]] const ConnectionStats& stats() const noexcept { return stats_; }
    [[nodiscard]] const HeartbeatMonitor& heartbeat() const noexcept { return heartbeat_; }
    [[nodiscard]] const ConnectionSupervisor& supervisor() const noexcept { return supervisor_; }
    [[nodiscard]] Result<bool> send(std::string_view payload);

    /// Highest sequence accepted, for persistence across a restart.
    [[nodiscard]] std::uint64_t last_sequence() const noexcept { return last_sequence_; }
    void restore_sequence(std::uint64_t sequence) noexcept { last_sequence_ = sequence; }

private:
    const IClock* clock_;
    ILiveTransport* transport_;
    HeartbeatMonitor heartbeat_;
    ConnectionSupervisor supervisor_;
    ConnectionState state_{ConnectionState::Disconnected};
    ConnectionStats stats_;
    std::uint64_t last_sequence_ = 0;
    /// Recently seen venue fill ids, for duplicate suppression on streams the
    /// venue does not sequence. Bounded: an unbounded set would grow for the
    /// life of the session.
    std::deque<std::string> recent_ids_;
};

}  // namespace ptl::live
