#pragma once

/// \file diagnostics.hpp
/// Startup validation, runtime diagnostics and service supervision.
///
/// CONFIG VALIDATION IS NOT CONFIG PARSING. `config::load` already rejects a
/// file that is malformed or has an unknown key -- that is a SYNTACTIC question
/// and it is settled. This asks a SEMANTIC one: are the values, individually
/// and together, safe to trade with? A risk limit of zero parses perfectly and
/// halts the strategy on its first order; a seed of zero parses and destroys
/// reproducibility. Neither is a parse error and both must stop startup.
///
/// FAIL FAST, AND SAY EVERYTHING. Validation collects EVERY problem before
/// returning rather than stopping at the first. An operator fixing a config at
/// 6am should get one list, not six consecutive failed starts.

#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "ptl/config/config.hpp"
#include "ptl/core/clock.hpp"
#include "ptl/core/result.hpp"
#include "ptl/oms/order_manager.hpp"
#include "ptl/ops/health.hpp"
#include "ptl/ops/metrics.hpp"
#include "ptl/portfolio/portfolio.hpp"
#include "ptl/risk/risk_manager.hpp"

namespace ptl::ops {

enum class Severity : std::uint8_t {
    /// Startup continues. Something is unusual but defensible.
    Warning,
    /// Startup MUST NOT continue.
    Fatal,
};

[[nodiscard]] std::string_view to_string(Severity) noexcept;

struct ValidationIssue {
    Severity severity{Severity::Warning};
    std::string field;
    std::string message;
    /// What a correct value would look like. An error that says only "invalid"
    /// makes the operator guess.
    std::string remedy;

    [[nodiscard]] std::string describe() const;
};

struct ValidationReport {
    std::vector<ValidationIssue> issues;

    [[nodiscard]] bool ok() const noexcept;  ///< no fatal issues
    [[nodiscard]] std::size_t fatal_count() const noexcept;
    [[nodiscard]] std::size_t warning_count() const noexcept;
    [[nodiscard]] std::string describe() const;
    [[nodiscard]] std::string to_json() const;
};

/// Semantic validation of a loaded configuration.
///
/// Takes the ALREADY-PARSED config: parsing is config::load's job and doing it
/// twice would give two answers to one question.
class ConfigValidator {
public:
    struct Options {
        /// Whether live credentials are required. False for a backtest, where
        /// demanding an API key would block every research run.
        bool require_credentials = false;
        /// Whether risk limits must be set. A backtest may legitimately run
        /// unlimited; a live session may not.
        bool require_risk_limits = false;
        /// Symbols the run expects to trade, checked against the instrument
        /// table.
        std::vector<std::string> expected_symbols;
    };

    ConfigValidator() = default;
    explicit ConfigValidator(Options options) : options_(std::move(options)) {}

    [[nodiscard]] ValidationReport validate(const config::Config&) const;
    /// Risk limits, validated separately because they arrive from a different
    /// place than the TOML in a live deployment.
    [[nodiscard]] ValidationReport validate_risk(const risk::RiskLimits&) const;
    /// Credential presence. Values are NEVER logged or echoed -- a validator
    /// that prints the key it is checking is a credential leak.
    [[nodiscard]] ValidationReport validate_credentials(
        const std::map<std::string, std::string, std::less<>>& environment) const;

private:
    Options options_;
};

/// A point-in-time picture of the running system.
///
/// READ-ONLY over every subsystem it reports on. Diagnostics that mutated what
/// they inspect would change behaviour under observation, which is the one
/// thing an observability layer must never do.
struct DiagnosticsSnapshot {
    Timestamp ts{kNoTimestamp};

    std::string session_id;
    std::string session_phase;
    HealthStatus health{HealthStatus::Unknown};
    bool trading_permitted = false;

    std::size_t active_strategies = 0;
    std::size_t open_orders = 0;
    std::size_t open_positions = 0;

    Notional equity{};
    Notional cash{};
    Notional realized_pnl{};
    Notional unrealized_pnl{};
    Notional gross_exposure{};
    Notional net_exposure{};
    double drawdown = 0.0;

    std::size_t risk_rejections = 0;
    std::size_t active_alerts = 0;
    std::optional<AlertSeverity> worst_alert;

    [[nodiscard]] std::string describe() const;
    [[nodiscard]] std::string to_json() const;
};

/// Assembles diagnostics from live subsystems.
class RuntimeDiagnostics {
public:
    /// Every reference is borrowed and CONST where the subsystem allows it.
    RuntimeDiagnostics(const IClock& clock, const portfolio::Portfolio& portfolio,
                       const oms::OrderManager& oms, const risk::RiskManager& risk,
                       const HealthMonitor& health, const AlertManager& alerts,
                       const MetricsRegistry& metrics) noexcept;

    [[nodiscard]] DiagnosticsSnapshot snapshot() const;
    void set_session(std::string id, std::string phase);
    void set_active_strategies(std::size_t count) noexcept { strategies_ = count; }

    /// Publish portfolio state into the metrics registry. The one method here
    /// that writes, and it writes only to metrics.
    void publish_metrics(MetricsRegistry&) const;

    [[nodiscard]] std::string to_json() const;

private:
    const IClock* clock_;
    const portfolio::Portfolio* portfolio_;
    const oms::OrderManager* oms_;
    const risk::RiskManager* risk_;
    const HealthMonitor* health_;
    const AlertManager* alerts_;
    const MetricsRegistry* metrics_;
    std::string session_id_;
    std::string session_phase_;
    std::size_t strategies_ = 0;
};

enum class ServiceState : std::uint8_t {
    Stopped,
    Starting,
    Running,
    Draining,  ///< finishing in-flight work, accepting nothing new
    Stopping,
    Failed,
};

[[nodiscard]] std::string_view to_string(ServiceState) noexcept;

struct SupervisorConfig {
    /// How long to allow in-flight work to finish during a graceful shutdown.
    Duration drain_timeout{std::chrono::seconds{30}};
    /// Restart automatically after a failure.
    bool auto_restart = true;
    /// Cap on automatic restarts. A service that fails repeatedly needs a
    /// human, and an unbounded restart loop hides the fault while burning the
    /// account.
    std::size_t max_restarts = 3;
    /// Window over which restarts are counted.
    Duration restart_window{std::chrono::minutes{10}};
};

/// Owns the process lifecycle: start, drain, stop, restart.
///
/// Deliberately has no threads and spawns nothing. It is a STATE MACHINE the
/// caller drives, so a graceful shutdown is testable and behaves identically
/// under replay.
class ServiceSupervisor {
public:
    ServiceSupervisor(const IClock& clock, SupervisorConfig config = {}) noexcept
        : clock_(&clock), config_(config) {}

    [[nodiscard]] Result<bool> start();
    /// Begin a graceful shutdown. In-flight work continues until the drain
    /// timeout, then the service stops regardless.
    [[nodiscard]] Result<bool> begin_drain(std::string reason);
    /// Whether the drain has finished or timed out.
    [[nodiscard]] bool drain_complete(std::size_t in_flight) const;
    [[nodiscard]] Result<bool> stop();
    /// Record a failure and decide whether to restart.
    [[nodiscard]] Result<bool> record_failure(std::string reason);
    [[nodiscard]] bool should_restart() const;

    [[nodiscard]] ServiceState state() const noexcept { return state_; }
    [[nodiscard]] std::size_t restarts() const noexcept { return restarts_.size(); }
    [[nodiscard]] const std::string& last_reason() const noexcept { return reason_; }
    [[nodiscard]] std::string to_json() const;

private:
    void prune_restarts();

    const IClock* clock_;
    SupervisorConfig config_;
    ServiceState state_{ServiceState::Stopped};
    std::string reason_;
    std::vector<Timestamp> restarts_;
    Timestamp drain_began_{kNoTimestamp};
};

}  // namespace ptl::ops
