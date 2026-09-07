#include "ptl/ops/health.hpp"

#include <algorithm>
#include <sstream>

namespace ptl::ops {
namespace {

[[nodiscard]] Error bad(std::string message, std::string context = {}) {
    return make_error(ErrorCode::ValidationFailed, std::move(message), std::move(context));
}

[[nodiscard]] std::string json_escape(std::string_view in) {
    std::string out;
    out.reserve(in.size() + 8);
    for (const char c : in) {
        switch (c) {
            case '"':
                out += "\\\"";
                break;
            case '\\':
                out += "\\\\";
                break;
            case '\n':
                out += "\\n";
                break;
            default:
                out += c;
        }
    }
    return out;
}

/// An unset timestamp serializes as an empty string, never as a date:
/// to_iso8601 on kNoTimestamp overflows int64 days.
[[nodiscard]] std::string iso_or_empty(Timestamp ts) {
    return is_set(ts) ? to_iso8601(ts) : std::string{};
}

}  // namespace

// ---------------------------------------------------------------------------
// Health
// ---------------------------------------------------------------------------

std::string_view to_string(HealthStatus s) noexcept {
    switch (s) {
        case HealthStatus::Healthy:
            return "healthy";
        case HealthStatus::Degraded:
            return "degraded";
        case HealthStatus::Unhealthy:
            return "unhealthy";
        case HealthStatus::Unknown:
            return "unknown";
    }
    return "unknown";
}

HealthStatus worse_of(HealthStatus a, HealthStatus b) noexcept {
    // Ordering by severity, NOT by enum value. Unknown ranks above Degraded:
    // a component that has never reported is more alarming than one reporting
    // a known wobble, because nobody knows what it is doing.
    const auto rank = [](HealthStatus s) -> int {
        switch (s) {
            case HealthStatus::Healthy:
                return 0;
            case HealthStatus::Degraded:
                return 1;
            case HealthStatus::Unknown:
                return 2;
            case HealthStatus::Unhealthy:
                return 3;
        }
        return 3;
    };
    return rank(a) >= rank(b) ? a : b;
}

std::string ComponentHealth::to_json() const {
    std::ostringstream ss;
    ss << "{\"name\": \"" << json_escape(name) << "\", \"status\": \"" << to_string(status)
       << "\", \"critical\": " << (critical ? "true" : "false")
       << ", \"consecutive_failures\": " << consecutive_failures << ", \"last_report\": \""
       << iso_or_empty(last_report) << "\", \"detail\": \"" << json_escape(detail) << "\"}";
    return ss.str();
}

Result<bool> HealthMonitor::register_component(std::string name, bool critical) {
    if (name.empty()) return fail(bad("component name cannot be empty"));
    if (components_.contains(name)) {
        return fail(bad("component is already registered", name));
    }
    ComponentHealth component;
    component.name = name;
    component.critical = critical;
    // Starts UNKNOWN, not Healthy. A component that has never checked in is not
    // known to be fine, and defaulting to Healthy would make a subsystem that
    // never starts invisible.
    component.status = HealthStatus::Unknown;
    components_.emplace(std::move(name), std::move(component));
    return true;
}

void HealthMonitor::report(std::string_view name, HealthStatus status, std::string detail) {
    const auto it = components_.find(name);
    if (it == components_.end()) return;  // unregistered reports are ignored

    it->second.last_report = clock_->now();
    it->second.detail = std::move(detail);

    if (status == HealthStatus::Healthy) {
        it->second.consecutive_failures = 0;
        it->second.status = HealthStatus::Healthy;
        return;
    }

    ++it->second.consecutive_failures;
    // A single failure is DEGRADED; repeated failure is UNHEALTHY. Escalating
    // on the first wobble would halt trading over one slow response.
    if (status == HealthStatus::Unhealthy ||
        it->second.consecutive_failures >= config_.failures_before_unhealthy) {
        it->second.status = HealthStatus::Unhealthy;
    } else {
        it->second.status = HealthStatus::Degraded;
    }
}

void HealthMonitor::report_healthy(std::string_view name) {
    report(name, HealthStatus::Healthy);
}

void HealthMonitor::report_failure(std::string_view name, std::string detail) {
    report(name, HealthStatus::Degraded, std::move(detail));
}

HealthStatus HealthMonitor::effective_status(const ComponentHealth& component) const {
    // STALENESS OVERRIDES A STORED STATUS. A component silent for longer than
    // the timeout is Unknown regardless of what it last said: a stale green is
    // worse than a red, because nobody looks at it.
    if (!is_set(component.last_report)) return HealthStatus::Unknown;
    const Timestamp now = clock_->now();
    if (is_set(now) && now - component.last_report > config_.staleness_timeout) {
        return HealthStatus::Unknown;
    }
    return component.status;
}

HealthStatus HealthMonitor::status_of(std::string_view name) const {
    const auto it = components_.find(name);
    if (it == components_.end()) return HealthStatus::Unknown;
    return effective_status(it->second);
}

HealthStatus HealthMonitor::overall() const {
    if (components_.empty()) return HealthStatus::Unknown;
    HealthStatus worst = HealthStatus::Healthy;
    for (const auto& [name, component] : components_) {
        worst = worse_of(worst, effective_status(component));
    }
    return worst;
}

HealthStatus HealthMonitor::critical_status() const {
    HealthStatus worst = HealthStatus::Healthy;
    bool any_critical = false;
    for (const auto& [name, component] : components_) {
        if (!component.critical) continue;
        any_critical = true;
        worst = worse_of(worst, effective_status(component));
    }
    return any_critical ? worst : HealthStatus::Unknown;
}

bool HealthMonitor::trading_permitted() const {
    const auto status = critical_status();
    // Degraded still trades: halting on every wobble is itself an outage.
    // Unknown does not, because an unreported critical component is a component
    // nobody can vouch for.
    return status == HealthStatus::Healthy || status == HealthStatus::Degraded;
}

std::vector<ComponentHealth> HealthMonitor::components() const {
    std::vector<ComponentHealth> out;
    out.reserve(components_.size());
    // std::map: ordered by name, so a diagnostics dump is reproducible.
    for (const auto& [name, component] : components_) {
        ComponentHealth copy = component;
        copy.status = effective_status(component);
        out.push_back(std::move(copy));
    }
    return out;
}

std::vector<std::string> HealthMonitor::unhealthy_components() const {
    std::vector<std::string> out;
    for (const auto& [name, component] : components_) {
        const auto status = effective_status(component);
        if (status == HealthStatus::Unhealthy || status == HealthStatus::Unknown) {
            out.push_back(name);
        }
    }
    return out;
}

std::string HealthMonitor::to_json() const {
    std::ostringstream ss;
    ss << "{\"overall\": \"" << to_string(overall()) << "\", \"critical\": \""
       << to_string(critical_status())
       << "\", \"trading_permitted\": " << (trading_permitted() ? "true" : "false")
       << ", \"components\": [";
    bool first = true;
    for (const auto& component : components()) {
        if (!first) ss << ", ";
        first = false;
        ss << component.to_json();
    }
    ss << "]}";
    return ss.str();
}

// ---------------------------------------------------------------------------
// CircuitBreaker
// ---------------------------------------------------------------------------

std::string_view to_string(BreakerState s) noexcept {
    switch (s) {
        case BreakerState::Closed:
            return "closed";
        case BreakerState::Open:
            return "open";
        case BreakerState::HalfOpen:
            return "half_open";
    }
    return "unknown";
}

CircuitBreaker::CircuitBreaker(const IClock& clock, std::string name, CircuitBreakerConfig config)
    : clock_(&clock), name_(std::move(name)), config_(config) {}

bool CircuitBreaker::allow() {
    if (state_ == BreakerState::Closed) return true;

    if (state_ == BreakerState::Open) {
        const Timestamp now = clock_->now();
        if (is_set(opened_at_) && is_set(now) && now - opened_at_ >= config_.open_duration) {
            // Probing, not fully closing. Slamming back to full traffic the
            // moment the timeout expires is how a recovering venue gets knocked
            // over a second time.
            state_ = BreakerState::HalfOpen;
            probe_successes_ = 0;
            return true;
        }
        return false;
    }

    // HalfOpen: let probes through one at a time.
    return true;
}

void CircuitBreaker::record_success() {
    consecutive_failures_ = 0;
    window_.push_back(true);
    if (window_.size() > config_.window) window_.pop_front();

    if (state_ == BreakerState::HalfOpen) {
        ++probe_successes_;
        if (probe_successes_ >= config_.probes_to_close) {
            state_ = BreakerState::Closed;
            window_.clear();  // a fresh window; the old failures are history
            probe_successes_ = 0;
        }
    }
}

void CircuitBreaker::record_failure() {
    ++consecutive_failures_;
    window_.push_back(false);
    if (window_.size() > config_.window) window_.pop_front();

    if (state_ == BreakerState::HalfOpen) {
        // A failed probe reopens immediately. Continuing to probe a venue that
        // just failed is how a breaker becomes decorative.
        trip();
        return;
    }
    if (state_ != BreakerState::Closed) return;

    if (consecutive_failures_ >= config_.failure_threshold) {
        trip();
        return;
    }
    // The RATE catches what a consecutive count misses: nine failures
    // alternating with one success never trips a consecutive counter.
    if (window_.size() >= config_.rate_minimum_samples &&
        failure_rate() >= config_.rate_threshold) {
        trip();
    }
}

void CircuitBreaker::trip() {
    state_ = BreakerState::Open;
    opened_at_ = clock_->now();
    ++trips_;
    probe_successes_ = 0;
}

double CircuitBreaker::failure_rate() const noexcept {
    if (window_.empty()) return 0.0;
    const auto failures = static_cast<double>(std::count(window_.begin(), window_.end(), false));
    return failures / static_cast<double>(window_.size());
}

void CircuitBreaker::reset() {
    state_ = BreakerState::Closed;
    window_.clear();
    consecutive_failures_ = 0;
    probe_successes_ = 0;
    opened_at_ = kNoTimestamp;
}

std::string CircuitBreaker::to_json() const {
    std::ostringstream ss;
    ss << "{\"name\": \"" << json_escape(name_) << "\", \"state\": \"" << to_string(state_)
       << "\", \"trips\": " << trips_ << ", \"failure_rate\": " << failure_rate()
       << ", \"consecutive_failures\": " << consecutive_failures_ << ", \"opened_at\": \""
       << iso_or_empty(opened_at_) << "\"}";
    return ss.str();
}

// ---------------------------------------------------------------------------
// Watchdog
// ---------------------------------------------------------------------------

void Watchdog::kick() {
    last_kick_ = clock_->now();
}

Duration Watchdog::since_kick() const {
    const Timestamp now = clock_->now();
    if (!is_set(last_kick_) || !is_set(now) || now < last_kick_) return Duration::zero();
    return now - last_kick_;
}

bool Watchdog::expired() const {
    // Never kicked is NOT expired. A watchdog that fires before its loop has
    // started would alert on every startup.
    if (!is_set(last_kick_)) return false;
    return since_kick() > timeout_;
}

bool Watchdog::check_and_count() {
    if (!expired()) return false;
    ++expirations_;
    return true;
}

// ---------------------------------------------------------------------------
// Alerts
// ---------------------------------------------------------------------------

std::string_view to_string(AlertSeverity s) noexcept {
    switch (s) {
        case AlertSeverity::Info:
            return "info";
        case AlertSeverity::Warning:
            return "warning";
        case AlertSeverity::Error:
            return "error";
        case AlertSeverity::Critical:
            return "critical";
    }
    return "unknown";
}

std::string Alert::to_json() const {
    std::ostringstream ss;
    ss << "{\"key\": \"" << json_escape(key) << "\", \"severity\": \"" << to_string(severity)
       << "\", \"summary\": \"" << json_escape(summary) << "\", \"detail\": \""
       << json_escape(detail) << "\", \"raised_at\": \"" << iso_or_empty(raised_at)
       << "\", \"last_seen\": \"" << iso_or_empty(last_seen)
       << "\", \"occurrences\": " << occurrences
       << ", \"resolved\": " << (resolved ? "true" : "false") << '}';
    return ss.str();
}

void AlertManager::raise(std::string key, AlertSeverity severity, std::string summary,
                         std::string detail) {
    if (key.empty()) return;
    const Timestamp now = clock_->now();
    ++raised_total_;

    const auto it = active_.find(key);
    if (it != active_.end()) {
        // DEDUPLICATED. A disconnect that flaps produces one alert with a
        // count, not four hundred -- and four hundred alerts is
        // indistinguishable from none.
        ++it->second.occurrences;
        it->second.last_seen = now;
        // Severity only ever escalates within one alert: a recurrence that
        // arrives as Info must not mask an Error already raised.
        if (static_cast<int>(severity) > static_cast<int>(it->second.severity)) {
            it->second.severity = severity;
            it->second.summary = std::move(summary);
            it->second.detail = std::move(detail);
        }
        ++suppressed_;
        return;
    }

    Alert alert;
    alert.key = key;
    alert.severity = severity;
    alert.summary = std::move(summary);
    alert.detail = std::move(detail);
    alert.raised_at = now;
    alert.last_seen = now;
    active_.emplace(std::move(key), std::move(alert));

    // Bounded, so a long session cannot grow without limit. The OLDEST is
    // dropped, because a stale alert is less useful than a current one.
    while (active_.size() > config_.max_active) {
        active_.erase(active_.begin());
    }
}

void AlertManager::resolve(std::string_view key) {
    const auto it = active_.find(key);
    // Resolving twice is normal in a recovery path, so this is silent.
    if (it == active_.end()) return;

    Alert resolved = it->second;
    resolved.resolved = true;
    resolved.resolved_at = clock_->now();
    active_.erase(it);

    history_.push_back(std::move(resolved));
    while (history_.size() > config_.max_history) history_.pop_front();
}

std::vector<Alert> AlertManager::active() const {
    std::vector<Alert> out;
    out.reserve(active_.size());
    for (const auto& [key, alert] : active_) out.push_back(alert);
    return out;
}

std::vector<Alert> AlertManager::history() const {
    return {history_.begin(), history_.end()};
}

std::optional<Alert> AlertManager::find(std::string_view key) const {
    const auto it = active_.find(key);
    if (it == active_.end()) return std::nullopt;
    return it->second;
}

std::optional<AlertSeverity> AlertManager::worst_active() const {
    if (active_.empty()) return std::nullopt;
    AlertSeverity worst = AlertSeverity::Info;
    for (const auto& [key, alert] : active_) {
        if (static_cast<int>(alert.severity) > static_cast<int>(worst)) {
            worst = alert.severity;
        }
    }
    return worst;
}

std::string AlertManager::to_json() const {
    std::ostringstream ss;
    ss << "{\"active_count\": " << active_.size() << ", \"raised_total\": " << raised_total_
       << ", \"suppressed\": " << suppressed_ << ", \"active\": [";
    bool first = true;
    for (const auto& [key, alert] : active_) {
        if (!first) ss << ", ";
        first = false;
        ss << alert.to_json();
    }
    ss << "]}";
    return ss.str();
}

}  // namespace ptl::ops
