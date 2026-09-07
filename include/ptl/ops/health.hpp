#pragma once

/// \file health.hpp
/// Health monitoring, circuit breaking, watchdogs and alerting.
///
/// WHAT THIS IS NOT. `risk::RiskManager` already halts trading on position,
/// concentration and drawdown limits. CircuitBreaker here trips on OPERATIONAL
/// failure -- reject rates, latency, error bursts -- which is a different
/// question with a different remedy. A book that is too large is a risk
/// problem; a venue rejecting nine orders in ten is an operations problem, and
/// conflating them would put a network fault into the risk report.
///
/// Nor does it duplicate `live::HeartbeatMonitor`, which watches ONE connection.
/// HealthMonitor aggregates many components, of which a connection is one.
///
/// CIRCUIT BREAKER IS THE ONE CONTROL IN ptl::ops. Everything else observes.
/// It is a control because an operational fault that nothing stops will keep
/// sending orders into a broken venue, and the cost of that grows with time.

#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "ptl/core/clock.hpp"
#include "ptl/core/result.hpp"
#include "ptl/core/types.hpp"
#include "ptl/ops/metrics.hpp"

namespace ptl::ops {

enum class HealthStatus : std::uint8_t {
    /// Working as expected.
    Healthy,
    /// Working, but a signal is outside its normal range. Trading continues:
    /// halting on every wobble is itself an outage.
    Degraded,
    /// Not working. Trading must not depend on this component.
    Unhealthy,
    /// No report yet. Distinct from Healthy -- a component that has never
    /// checked in is not known to be fine, and defaulting to Healthy would
    /// make a dead subsystem invisible at startup.
    Unknown,
};

[[nodiscard]] std::string_view to_string(HealthStatus) noexcept;
/// Worst of two statuses, for aggregation.
[[nodiscard]] HealthStatus worse_of(HealthStatus, HealthStatus) noexcept;

struct ComponentHealth {
    std::string name;
    HealthStatus status{HealthStatus::Unknown};
    Timestamp last_report{kNoTimestamp};
    std::string detail;
    /// Consecutive failures, for escalation.
    std::size_t consecutive_failures = 0;
    /// True when this component's failure should stop trading. A market data
    /// feed is critical; a metrics scrape endpoint is not.
    bool critical = true;

    [[nodiscard]] std::string to_json() const;
};

struct HealthConfig {
    /// A component silent for longer than this is Unknown regardless of its
    /// last reported status. A stale green is worse than a red, because nobody
    /// looks at it.
    Duration staleness_timeout{std::chrono::seconds{60}};
    /// Consecutive failures before a Degraded component becomes Unhealthy.
    std::size_t failures_before_unhealthy = 3;
};

/// Aggregates the health of every monitored component.
class HealthMonitor {
public:
    HealthMonitor(const IClock& clock, HealthConfig config = {}) noexcept
        : clock_(&clock), config_(config) {}

    /// Register a component. Critical components gate trading; others do not.
    [[nodiscard]] Result<bool> register_component(std::string name, bool critical = true);

    void report(std::string_view name, HealthStatus, std::string detail = {});
    void report_healthy(std::string_view name);
    void report_failure(std::string_view name, std::string detail);

    [[nodiscard]] HealthStatus status_of(std::string_view name) const;
    /// Worst status across all components, with staleness applied.
    [[nodiscard]] HealthStatus overall() const;
    /// Worst status across CRITICAL components only. This is the one that
    /// decides whether trading may continue.
    [[nodiscard]] HealthStatus critical_status() const;
    [[nodiscard]] bool trading_permitted() const;

    [[nodiscard]] std::vector<ComponentHealth> components() const;
    [[nodiscard]] std::vector<std::string> unhealthy_components() const;
    [[nodiscard]] std::string to_json() const;
    [[nodiscard]] std::size_t size() const noexcept { return components_.size(); }

private:
    [[nodiscard]] HealthStatus effective_status(const ComponentHealth&) const;

    const IClock* clock_;
    HealthConfig config_;
    std::map<std::string, ComponentHealth, std::less<>> components_;
};

enum class BreakerState : std::uint8_t {
    /// Normal operation.
    Closed,
    /// Tripped: requests are refused.
    Open,
    /// Probing: one request is allowed through to test recovery. Without this
    /// state a breaker either stays open forever or slams back to full traffic
    /// the moment the timeout expires -- and the second is how a recovering
    /// venue gets knocked over again.
    HalfOpen,
};

[[nodiscard]] std::string_view to_string(BreakerState) noexcept;

struct CircuitBreakerConfig {
    /// Consecutive failures that trip the breaker.
    std::size_t failure_threshold = 5;
    /// Failure RATE that trips it, over the rolling window. A rate catches the
    /// case a consecutive count misses: nine failures alternating with one
    /// success never trips a consecutive counter.
    double rate_threshold = 0.5;
    /// Minimum samples before the rate is considered. Without it, the first
    /// failure is a 100% rate and trips immediately.
    std::size_t rate_minimum_samples = 20;
    /// Rolling window size for the rate.
    std::size_t window = 100;
    /// How long the breaker stays open before probing.
    Duration open_duration{std::chrono::seconds{30}};
    /// Successful probes required to close again.
    std::size_t probes_to_close = 3;
};

/// Trips on operational failure and refuses work until recovery.
class CircuitBreaker {
public:
    CircuitBreaker(const IClock& clock, std::string name, CircuitBreakerConfig config = {});

    /// Whether an operation may proceed now. Advances the state machine, so a
    /// breaker whose open period has elapsed becomes HalfOpen here.
    [[nodiscard]] bool allow();

    void record_success();
    void record_failure();

    [[nodiscard]] BreakerState state() const noexcept { return state_; }
    [[nodiscard]] const std::string& name() const noexcept { return name_; }
    [[nodiscard]] double failure_rate() const noexcept;
    [[nodiscard]] std::size_t trips() const noexcept { return trips_; }
    [[nodiscard]] Timestamp opened_at() const noexcept { return opened_at_; }
    /// Force closed after manual intervention.
    void reset();
    [[nodiscard]] std::string to_json() const;

private:
    void trip();

    const IClock* clock_;
    std::string name_;
    CircuitBreakerConfig config_;
    BreakerState state_{BreakerState::Closed};
    std::deque<bool> window_;  ///< true = success
    std::size_t consecutive_failures_ = 0;
    std::size_t probe_successes_ = 0;
    std::size_t trips_ = 0;
    Timestamp opened_at_{kNoTimestamp};
};

/// Detects a stalled subsystem by absence of activity.
///
/// Distinct from a heartbeat: a heartbeat is a message the peer sends, whereas
/// a watchdog is a timer OUR code must reset. It catches a loop that is alive
/// but no longer making progress, which no peer can observe.
class Watchdog {
public:
    Watchdog(const IClock& clock, std::string name, Duration timeout) noexcept
        : clock_(&clock), name_(std::move(name)), timeout_(timeout) {}

    /// Reset the timer. Called from the loop being watched.
    void kick();
    [[nodiscard]] bool expired() const;
    [[nodiscard]] Duration since_kick() const;
    [[nodiscard]] const std::string& name() const noexcept { return name_; }
    [[nodiscard]] std::size_t expirations() const noexcept { return expirations_; }
    /// Check and count an expiry. Separate from expired() so a poll does not
    /// silently inflate the count.
    [[nodiscard]] bool check_and_count();

private:
    const IClock* clock_;
    std::string name_;
    Duration timeout_;
    Timestamp last_kick_{kNoTimestamp};
    std::size_t expirations_ = 0;
};

enum class AlertSeverity : std::uint8_t { Info, Warning, Error, Critical };

[[nodiscard]] std::string_view to_string(AlertSeverity) noexcept;

struct Alert {
    std::string key;  ///< stable identity, for deduplication
    AlertSeverity severity{AlertSeverity::Info};
    std::string summary;
    std::string detail;
    Timestamp raised_at{kNoTimestamp};
    Timestamp last_seen{kNoTimestamp};
    std::size_t occurrences = 1;
    bool resolved = false;
    Timestamp resolved_at{kNoTimestamp};

    [[nodiscard]] std::string to_json() const;
};

struct AlertConfig {
    /// Repeat suppression window. A disconnect that flaps produces one alert
    /// with a count, not four hundred -- and four hundred alerts is
    /// indistinguishable from none.
    Duration deduplication_window{std::chrono::seconds{300}};
    /// Cap on retained alerts. Bounded so a long session cannot grow without
    /// limit.
    std::size_t max_active = 512;
    std::size_t max_history = 1024;
};

/// Raises, deduplicates and resolves alerts.
///
/// Deliberately has NO transport. Delivery -- email, pager, webhook -- is
/// deployment concern that this phase explicitly excludes, and building it in
/// would make the alert logic untestable without a network.
class AlertManager {
public:
    AlertManager(const IClock& clock, AlertConfig config = {}) noexcept
        : clock_(&clock), config_(config) {}

    /// Raise an alert. Repeats within the window increment the count rather
    /// than creating a duplicate.
    void raise(std::string key, AlertSeverity, std::string summary, std::string detail = {});
    /// Mark an alert resolved. Silently does nothing if it was never raised,
    /// because resolving twice is normal in a recovery path.
    void resolve(std::string_view key);

    [[nodiscard]] std::vector<Alert> active() const;
    [[nodiscard]] std::vector<Alert> history() const;
    [[nodiscard]] std::optional<Alert> find(std::string_view key) const;
    [[nodiscard]] std::size_t active_count() const noexcept { return active_.size(); }
    [[nodiscard]] std::size_t raised_total() const noexcept { return raised_total_; }
    [[nodiscard]] std::size_t suppressed() const noexcept { return suppressed_; }
    /// Highest severity currently active.
    [[nodiscard]] std::optional<AlertSeverity> worst_active() const;
    [[nodiscard]] std::string to_json() const;

private:
    const IClock* clock_;
    AlertConfig config_;
    std::map<std::string, Alert, std::less<>> active_;
    std::deque<Alert> history_;
    std::size_t raised_total_ = 0;
    std::size_t suppressed_ = 0;
};

/// Canonical alert keys, for the same reason metric names are constants.
namespace alert_keys {
inline constexpr std::string_view kBrokerDisconnected = "broker.disconnected";
inline constexpr std::string_view kMarketDataDisconnected = "marketdata.disconnected";
inline constexpr std::string_view kHeartbeatLate = "connection.heartbeat_late";
inline constexpr std::string_view kExcessLatency = "latency.excessive";
inline constexpr std::string_view kRiskBreach = "risk.limit_breached";
inline constexpr std::string_view kMarginViolation = "account.margin_violation";
inline constexpr std::string_view kOrderFailures = "orders.failure_rate";
inline constexpr std::string_view kRepeatedReconnects = "connection.reconnect_storm";
inline constexpr std::string_view kReconciliationDrift = "account.reconciliation_drift";
inline constexpr std::string_view kCircuitOpen = "circuit.open";
inline constexpr std::string_view kWatchdogExpired = "watchdog.expired";
inline constexpr std::string_view kResourcePressure = "resource.pressure";
}  // namespace alert_keys

}  // namespace ptl::ops
