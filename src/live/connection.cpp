#include "ptl/live/connection.hpp"

#include <algorithm>
#include <sstream>

namespace ptl::live {
namespace {

[[nodiscard]] Error bad(std::string message, std::string context = {}) {
    return make_error(ErrorCode::ValidationFailed, std::move(message), std::move(context));
}

/// Cap on remembered venue ids. Large enough to cover a redelivery burst after
/// a reconnect, small enough that the memory is bounded for a session that runs
/// for months.
constexpr std::size_t kRecentIdCapacity = 4096;

}  // namespace

std::string_view to_string(ConnectionState s) noexcept {
    switch (s) {
        case ConnectionState::Disconnected:
            return "disconnected";
        case ConnectionState::Connecting:
            return "connecting";
        case ConnectionState::Connected:
            return "connected";
        case ConnectionState::Synchronizing:
            return "synchronizing";
        case ConnectionState::Ready:
            return "ready";
        case ConnectionState::Degraded:
            return "degraded";
        case ConnectionState::Reconnecting:
            return "reconnecting";
        case ConnectionState::Failed:
            return "failed";
    }
    return "unknown";
}

bool permits_trading(ConnectionState s) noexcept {
    // Ready ONLY. Connected-but-unsynchronised is deliberately excluded: the
    // local book and the venue's may disagree, and trading on a stale book is
    // how a reconnect doubles a position.
    //
    // Degraded is also excluded. A late heartbeat means we cannot be sure our
    // view is current, and the cost of pausing is far below the cost of
    // trading blind.
    return s == ConnectionState::Ready;
}

std::string_view to_string(MessageKind k) noexcept {
    switch (k) {
        case MessageKind::Heartbeat:
            return "heartbeat";
        case MessageKind::OrderAccepted:
            return "order_accepted";
        case MessageKind::OrderRejected:
            return "order_rejected";
        case MessageKind::OrderCancelled:
            return "order_cancelled";
        case MessageKind::OrderReplaced:
            return "order_replaced";
        case MessageKind::PartialFill:
            return "partial_fill";
        case MessageKind::Fill:
            return "fill";
        case MessageKind::AccountUpdate:
            return "account_update";
        case MessageKind::Quote:
            return "quote";
        case MessageKind::Trade:
            return "trade";
        case MessageKind::Unknown:
            return "unknown";
    }
    return "unknown";
}

std::string BrokerMessage::describe() const {
    std::ostringstream ss;
    ss << to_string(kind);
    if (sequence != 0) ss << " seq=" << sequence;
    if (!client_order_id.empty()) ss << " client=" << client_order_id;
    if (!venue_order_id.empty()) ss << " venue=" << venue_order_id;
    if (quantity.get() != 0.0) ss << " qty=" << quantity.get();
    if (price.get() != 0.0) ss << " px=" << price.get();
    if (!text.empty()) ss << " (" << text << ')';
    return ss.str();
}

// ---------------------------------------------------------------------------
// HeartbeatMonitor
// ---------------------------------------------------------------------------

void HeartbeatMonitor::record_heartbeat(Timestamp now) noexcept {
    last_heartbeat_ = now;
    last_activity_ = now;
    ++beats_;
}

void HeartbeatMonitor::record_activity(Timestamp now) noexcept {
    // ANY inbound message proves liveness. A venue busy streaming quotes may
    // legitimately skip a heartbeat, and declaring it dead would be an outage
    // we caused ourselves.
    last_activity_ = now;
    if (!is_set(last_heartbeat_)) last_heartbeat_ = now;
}

Duration HeartbeatMonitor::since_last(Timestamp now) const noexcept {
    const Timestamp reference = is_set(last_activity_) ? last_activity_ : last_heartbeat_;
    if (!is_set(reference) || !is_set(now) || now < reference) return Duration::zero();
    return now - reference;
}

bool HeartbeatMonitor::healthy(Timestamp now) const noexcept {
    if (!is_set(last_activity_) && !is_set(last_heartbeat_)) return false;
    return since_last(now) <= config_.degraded_after;
}

bool HeartbeatMonitor::degraded(Timestamp now) const noexcept {
    const Duration elapsed = since_last(now);
    return elapsed > config_.degraded_after && elapsed <= config_.dead_after;
}

bool HeartbeatMonitor::dead(Timestamp now) const noexcept {
    if (!is_set(last_activity_) && !is_set(last_heartbeat_)) return false;
    return since_last(now) > config_.dead_after;
}

void HeartbeatMonitor::reset(Timestamp now) noexcept {
    last_heartbeat_ = now;
    last_activity_ = now;
}

// ---------------------------------------------------------------------------
// ConnectionSupervisor
// ---------------------------------------------------------------------------

bool ConnectionSupervisor::should_attempt(Timestamp now) const noexcept {
    if (exhausted()) return false;
    if (!is_set(next_attempt_)) return true;  // first attempt is immediate
    return is_set(now) && now >= next_attempt_;
}

void ConnectionSupervisor::record_attempt(Timestamp now) noexcept {
    ++attempts_;
    next_attempt_ = is_set(now) ? now + backoff_ : next_attempt_;

    // Exponential backoff, CAPPED. Unbounded doubling means a session that
    // drops overnight is still asleep when the market opens.
    const auto next = static_cast<std::int64_t>(static_cast<double>(backoff_.count()) *
                                                config_.backoff_multiplier);
    backoff_ = Duration{std::min(next, config_.max_backoff.count())};
}

void ConnectionSupervisor::record_success(Timestamp now) noexcept {
    ++successes_;
    attempts_ = 0;
    backoff_ = config_.initial_backoff;
    next_attempt_ = now;
}

void ConnectionSupervisor::record_failure(Timestamp now) noexcept {
    if (is_set(now)) next_attempt_ = now + backoff_;
}

bool ConnectionSupervisor::exhausted() const noexcept {
    return config_.max_attempts > 0 && attempts_ >= config_.max_attempts;
}

// ---------------------------------------------------------------------------
// LiveConnection
// ---------------------------------------------------------------------------

std::string ConnectionStats::describe() const {
    std::ostringstream ss;
    ss << "connection: " << connects << " connects, " << disconnects << " disconnects, "
       << reconnects << " reconnects\n";
    ss << "  messages   " << messages_received << '\n';
    ss << "  duplicates " << duplicates_dropped << '\n';
    ss << "  reordered  " << out_of_order_dropped << '\n';
    ss << "  unknown    " << unknown_messages << '\n';
    return ss.str();
}

LiveConnection::LiveConnection(const IClock& clock, ILiveTransport& transport,
                               HeartbeatConfig heartbeat, SupervisorConfig supervisor)
    : clock_(&clock), transport_(&transport), heartbeat_(heartbeat), supervisor_(supervisor) {}

Result<bool> LiveConnection::connect() {
    if (state_ == ConnectionState::Ready || state_ == ConnectionState::Connected) {
        return true;
    }
    state_ = ConnectionState::Connecting;
    supervisor_.record_attempt(clock_->now());

    auto opened = transport_->open();
    if (!opened) {
        state_ = ConnectionState::Failed;
        supervisor_.record_failure(clock_->now());
        return fail(opened.error());
    }

    ++stats_.connects;
    supervisor_.record_success(clock_->now());
    heartbeat_.reset(clock_->now());
    // Connected, NOT Ready. Synchronisation with the venue must complete
    // before any order is sent.
    state_ = ConnectionState::Synchronizing;
    return true;
}

void LiveConnection::disconnect() noexcept {
    if (state_ == ConnectionState::Disconnected) return;
    transport_->close();
    ++stats_.disconnects;
    state_ = ConnectionState::Disconnected;
}

Result<bool> LiveConnection::maybe_reconnect() {
    if (!supervisor_.should_attempt(clock_->now())) return false;
    if (supervisor_.exhausted()) {
        state_ = ConnectionState::Failed;
        return fail(bad("reconnect attempts exhausted; manual intervention required"));
    }

    state_ = ConnectionState::Reconnecting;
    transport_->close();
    auto reconnected = connect();
    if (!reconnected) return reconnected;

    ++stats_.reconnects;
    // Sequence tracking is DELIBERATELY NOT reset. The venue may redeliver
    // messages we already applied, and the duplicate filter is what stops a
    // redelivered fill being counted twice.
    return true;
}

Result<bool> LiveConnection::mark_synchronized() {
    if (state_ != ConnectionState::Synchronizing && state_ != ConnectionState::Degraded) {
        return fail(bad("cannot mark synchronized from state " + std::string{to_string(state_)}));
    }
    state_ = ConnectionState::Ready;
    return true;
}

Result<bool> LiveConnection::send(std::string_view payload) {
    if (!permits_trading(state_)) {
        return fail(bad("refusing to send while connection is " + std::string{to_string(state_)}));
    }
    return transport_->send(payload);
}

void LiveConnection::tick() {
    const Timestamp now = clock_->now();

    // Evaluated against the CLOCK, not against arriving messages. A silent
    // venue produces no message to trigger a message-driven check, which is
    // exactly the failure that matters most.
    if (state_ == ConnectionState::Ready || state_ == ConnectionState::Degraded) {
        if (heartbeat_.dead(now)) {
            heartbeat_.note_miss();
            state_ = ConnectionState::Reconnecting;
        } else if (heartbeat_.degraded(now)) {
            state_ = ConnectionState::Degraded;
        } else if (state_ == ConnectionState::Degraded) {
            // Recovered on its own; no reconnect was needed.
            state_ = ConnectionState::Ready;
        }
    }
}

std::vector<BrokerMessage> LiveConnection::poll() {
    std::vector<BrokerMessage> accepted;
    if (!transport_->is_open()) return accepted;

    for (auto& message : transport_->poll()) {
        ++stats_.messages_received;
        const Timestamp now = clock_->now();
        message.received_time = now;
        heartbeat_.record_activity(now);

        if (message.kind == MessageKind::Heartbeat) {
            heartbeat_.record_heartbeat(now);
            continue;  // consumed here; nothing above needs to see it
        }
        if (message.kind == MessageKind::Unknown) {
            // Counted, not silently dropped: an unknown message is evidence of
            // a protocol change, and hiding it defers the discovery until
            // something breaks.
            ++stats_.unknown_messages;
            continue;
        }

        // --- ordering and duplicate hygiene, done ONCE, here ----------------
        if (message.sequence != 0) {
            if (message.sequence <= last_sequence_) {
                // A venue that redelivers after a reconnect is normal; applying
                // a fill twice is not.
                ++stats_.duplicates_dropped;
                continue;
            }
            last_sequence_ = message.sequence;
        } else if (!message.venue_fill_id.empty()) {
            // Unsequenced stream: fall back to venue ids.
            const auto seen =
                std::find(recent_ids_.begin(), recent_ids_.end(), message.venue_fill_id);
            if (seen != recent_ids_.end()) {
                ++stats_.duplicates_dropped;
                continue;
            }
            recent_ids_.push_back(message.venue_fill_id);
            if (recent_ids_.size() > kRecentIdCapacity) recent_ids_.pop_front();
        }

        // Out-of-order venue timestamps are DROPPED rather than reordered.
        // Reordering would require buffering with a deadline, and a stale
        // update applied late is worse than one never applied.
        if (is_set(message.venue_time) && is_set(message.received_time) &&
            message.venue_time > message.received_time + std::chrono::seconds{5}) {
            ++stats_.out_of_order_dropped;
            continue;
        }

        accepted.push_back(std::move(message));
    }
    return accepted;
}

}  // namespace ptl::live
