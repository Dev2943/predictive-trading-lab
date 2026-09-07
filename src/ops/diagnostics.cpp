#include "ptl/ops/diagnostics.hpp"

#include <algorithm>
#include <iomanip>
#include <limits>
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

[[nodiscard]] std::string num(double v, int precision = 6) {
    if (!is_finite(v)) return "null";
    std::ostringstream ss;
    ss << std::fixed << std::setprecision(precision) << v;
    return ss.str();
}

[[nodiscard]] std::string iso_or_empty(Timestamp ts) {
    return is_set(ts) ? to_iso8601(ts) : std::string{};
}

}  // namespace

std::string_view to_string(Severity s) noexcept {
    switch (s) {
        case Severity::Warning:
            return "warning";
        case Severity::Fatal:
            return "fatal";
    }
    return "unknown";
}

std::string ValidationIssue::describe() const {
    std::ostringstream ss;
    ss << '[' << to_string(severity) << "] " << field << ": " << message;
    if (!remedy.empty()) ss << " -- " << remedy;
    return ss.str();
}

bool ValidationReport::ok() const noexcept {
    return fatal_count() == 0;
}

std::size_t ValidationReport::fatal_count() const noexcept {
    return static_cast<std::size_t>(std::count_if(
        issues.begin(), issues.end(),
        [](const ValidationIssue& issue) { return issue.severity == Severity::Fatal; }));
}

std::size_t ValidationReport::warning_count() const noexcept {
    return issues.size() - fatal_count();
}

std::string ValidationReport::describe() const {
    std::ostringstream ss;
    if (issues.empty()) {
        ss << "configuration validated: no issues\n";
        return ss.str();
    }
    ss << "configuration validation: " << fatal_count() << " fatal, " << warning_count()
       << " warning\n";
    for (const auto& issue : issues) ss << "  " << issue.describe() << '\n';
    return ss.str();
}

std::string ValidationReport::to_json() const {
    std::ostringstream ss;
    ss << "{\"ok\": " << (ok() ? "true" : "false") << ", \"fatal\": " << fatal_count()
       << ", \"warnings\": " << warning_count() << ", \"issues\": [";
    for (std::size_t i = 0; i < issues.size(); ++i) {
        if (i != 0) ss << ", ";
        ss << "{\"severity\": \"" << to_string(issues[i].severity) << "\", \"field\": \""
           << json_escape(issues[i].field) << "\", \"message\": \""
           << json_escape(issues[i].message) << "\", \"remedy\": \""
           << json_escape(issues[i].remedy) << "\"}";
    }
    ss << "]}";
    return ss.str();
}

// ---------------------------------------------------------------------------
// ConfigValidator
// ---------------------------------------------------------------------------

ValidationReport ConfigValidator::validate(const config::Config& config) const {
    ValidationReport report;
    const auto add = [&report](Severity severity, std::string field, std::string message,
                               std::string remedy) {
        report.issues.push_back(
            {severity, std::move(field), std::move(message), std::move(remedy)});
    };

    // EVERY problem is collected, not just the first. An operator fixing a
    // config at 6am should get one list, not six consecutive failed starts.

    if (config.run.seed == 0) {
        // Parses perfectly and destroys reproducibility.
        add(Severity::Fatal, "run.seed", "seed is zero, which is almost always an unset field",
            "set a non-zero seed so the run can be reproduced");
    }
    if (config.run.results_dir.empty()) {
        add(Severity::Fatal, "run.results_dir", "no results directory is configured",
            "set run.results_dir to a writable path");
    }
    if (config.databento.max_spend_usd <= 0.0) {
        add(Severity::Warning, "databento.max_spend_usd",
            "spend cap is zero or negative, so no paid data can be fetched",
            "set a positive cap, or ignore this if the run uses cached data only");
    }
    if (!is_finite(config.databento.max_spend_usd)) {
        add(Severity::Fatal, "data.databento.max_spend_usd", "spend cap is not a finite number",
            "set a finite dollar amount");
    }

    if (options_.require_credentials) {
        // The validator checks PRESENCE, never content, and never echoes a
        // value. A validator that prints the key it is checking is a
        // credential leak in the startup log.
        // The market data provider is selected by ADR-0001 tier, not by a
        // config string, so there is nothing to check here. Credentials live
        // in the environment and are validated by validate_credentials.
        if (config.databento.max_spend_usd <= 0.0) {
            add(Severity::Fatal, "databento.max_spend_usd",
                "a live run cannot fetch data with a zero spend cap",
                "set a positive cap before starting a live session");
        }
    }

    if (!options_.expected_symbols.empty()) {
        for (const auto& symbol : options_.expected_symbols) {
            if (symbol.empty()) {
                add(Severity::Fatal, "symbols", "an expected symbol is the empty string",
                    "remove the empty entry from the symbol list");
                continue;
            }
            // A symbol with whitespace or a lowercase letter is nearly always a
            // transcription error, and it will be rejected by the venue at the
            // worst possible moment instead of at startup.
            const bool suspicious = std::any_of(symbol.begin(), symbol.end(), [](char c) {
                return c == ' ' || c == '\t' || (c >= 'a' && c <= 'z');
            });
            if (suspicious) {
                add(Severity::Warning, "symbols",
                    "symbol '" + symbol + "' contains whitespace or lowercase letters",
                    "venue symbols are conventionally uppercase without spaces");
            }
        }
    }
    return report;
}

ValidationReport ConfigValidator::validate_risk(const risk::RiskLimits& limits) const {
    ValidationReport report;
    const auto add = [&report](Severity severity, std::string field, std::string message,
                               std::string remedy) {
        report.issues.push_back(
            {severity, std::move(field), std::move(message), std::move(remedy)});
    };

    // A limit of ZERO parses perfectly and halts the strategy on its first
    // order. That is not a parse error and it must stop startup.
    if (limits.max_order_notional.get() <= 0.0) {
        add(Severity::Fatal, "risk.max_order_notional",
            "order notional limit is zero or negative; every order would be rejected",
            "set a positive notional limit");
    }
    if (limits.max_gross_leverage <= 0.0) {
        add(Severity::Fatal, "risk.max_gross_leverage",
            "gross leverage limit is zero or negative; no position could be held",
            "1.0 permits a fully invested unlevered book");
    }
    if (limits.max_position_notional.get() <= 0.0) {
        add(Severity::Fatal, "risk.max_position_notional",
            "position notional limit is zero or negative; every position would be "
            "rejected",
            "set a positive per-position notional limit");
    }
    if (limits.max_concentration <= 0.0 || limits.max_concentration > 1.0) {
        add(Severity::Fatal, "risk.max_concentration", "concentration limit must lie in (0, 1]",
            "0.10 permits a single name to be a tenth of the book");
    }
    if (limits.max_drawdown_pct <= 0.0 || limits.max_drawdown_pct >= 1.0) {
        add(Severity::Fatal, "risk.max_drawdown_pct", "drawdown limit must lie in (0, 1)",
            "0.20 halts new risk after a twenty percent peak-to-trough loss");
    }
    if (options_.require_risk_limits && limits.max_daily_turnover <= 0.0) {
        add(Severity::Fatal, "risk.max_daily_turnover", "a live session requires a turnover budget",
            "set a positive daily turnover multiple of equity");
    }

    // A limit so large it can never bind is a limit in name only, and it will
    // read as protection on a control report.
    if (limits.max_gross_leverage > 1000.0) {
        add(Severity::Warning, "risk.max_gross_leverage",
            "gross leverage limit is so large it can never bind",
            "a limit that cannot trigger provides no protection");
    }
    return report;
}

ValidationReport ConfigValidator::validate_credentials(
    const std::map<std::string, std::string, std::less<>>& environment) const {
    ValidationReport report;
    if (!options_.require_credentials) return report;

    static constexpr std::string_view kRequired[] = {"PTL_BROKER_KEY", "PTL_BROKER_SECRET"};
    for (const auto& name : kRequired) {
        const auto it = environment.find(name);
        if (it == environment.end() || it->second.empty()) {
            // The NAME is reported, never the value, and not even a masked
            // prefix: a masked credential in a log is still a credential in a
            // log.
            report.issues.push_back({Severity::Fatal, std::string{name},
                                     "required credential is not set",
                                     "export the variable before starting a live "
                                     "session"});
        }
    }
    return report;
}

// ---------------------------------------------------------------------------
// RuntimeDiagnostics
// ---------------------------------------------------------------------------

RuntimeDiagnostics::RuntimeDiagnostics(const IClock& clock, const portfolio::Portfolio& portfolio,
                                       const oms::OrderManager& oms, const risk::RiskManager& risk,
                                       const HealthMonitor& health, const AlertManager& alerts,
                                       const MetricsRegistry& metrics) noexcept
    : clock_(&clock),
      portfolio_(&portfolio),
      oms_(&oms),
      risk_(&risk),
      health_(&health),
      alerts_(&alerts),
      metrics_(&metrics) {}

void RuntimeDiagnostics::set_session(std::string id, std::string phase) {
    session_id_ = std::move(id);
    session_phase_ = std::move(phase);
}

DiagnosticsSnapshot RuntimeDiagnostics::snapshot() const {
    DiagnosticsSnapshot out;
    out.ts = clock_->now();
    out.session_id = session_id_;
    out.session_phase = session_phase_;
    out.health = health_->overall();
    out.trading_permitted = health_->trading_permitted();
    out.active_strategies = strategies_;

    // READ-ONLY throughout. Diagnostics that mutated what they inspect would
    // change behaviour under observation.
    out.open_orders = oms_->working().size();
    out.equity = portfolio_->equity();
    out.cash = portfolio_->cash();
    out.realized_pnl = portfolio_->realized_pnl();
    out.unrealized_pnl = portfolio_->unrealized_pnl();
    out.gross_exposure = portfolio_->gross_exposure();
    out.net_exposure = portfolio_->net_exposure();

    std::size_t open_positions = 0;
    for (const auto& [key, position] : portfolio_->positions()) {
        if (!position.is_flat()) ++open_positions;
    }
    out.open_positions = open_positions;

    // Drawdown comes from the METRICS REGISTRY, not recomputed here. The
    // analytics layer owns its definition, and a second computation in a
    // diagnostics dump could disagree with the risk engine that halts on it.
    // Reading it here is also what makes metrics_ a real dependency rather than
    // a stored pointer nobody uses -- Clang's unused-private-field warning
    // caught that this field was dead and the drawdown was never populated.
    out.drawdown = metrics_->gauge(metric_names::kDrawdown);
    out.risk_rejections = risk_->rejection_count();
    out.active_alerts = alerts_->active_count();
    out.worst_alert = alerts_->worst_active();
    return out;
}

void RuntimeDiagnostics::publish_metrics(MetricsRegistry& registry) const {
    registry.set_gauge(metric_names::kEquity, portfolio_->equity().get());
    registry.set_gauge(metric_names::kGrossExposure, portfolio_->gross_exposure().get());
    registry.set_gauge(metric_names::kNetExposure, portfolio_->net_exposure().get());
    registry.set_gauge(metric_names::kRealizedPnl, portfolio_->realized_pnl().get());
    registry.set_gauge(metric_names::kUnrealizedPnl, portfolio_->unrealized_pnl().get());
    registry.set_gauge(metric_names::kQueueDepth, static_cast<double>(oms_->working().size()));
}

std::string DiagnosticsSnapshot::describe() const {
    std::ostringstream ss;
    ss.precision(2);
    ss << std::fixed;
    ss << "diagnostics at " << iso_or_empty(ts) << '\n';
    ss << "  session       " << session_id << " [" << session_phase << "]\n";
    ss << "  health        " << to_string(health)
       << (trading_permitted ? " (trading permitted)" : " (TRADING HALTED)") << '\n';
    ss << "  strategies    " << active_strategies << '\n';
    ss << "  open orders   " << open_orders << '\n';
    ss << "  positions     " << open_positions << '\n';
    ss << "  equity        " << equity.get() << '\n';
    ss << "  gross/net     " << gross_exposure.get() << " / " << net_exposure.get() << '\n';
    ss << "  risk rejects  " << risk_rejections << '\n';
    ss << "  alerts        " << active_alerts;
    if (worst_alert.has_value()) ss << " (worst: " << to_string(*worst_alert) << ')';
    ss << '\n';
    return ss.str();
}

std::string DiagnosticsSnapshot::to_json() const {
    std::ostringstream ss;
    ss << "{\"ts\": \"" << iso_or_empty(ts) << "\", \"session_id\": \"" << json_escape(session_id)
       << "\", \"session_phase\": \"" << json_escape(session_phase) << "\", \"health\": \""
       << to_string(health)
       << "\", \"trading_permitted\": " << (trading_permitted ? "true" : "false")
       << ", \"active_strategies\": " << active_strategies << ", \"open_orders\": " << open_orders
       << ", \"open_positions\": " << open_positions << ", \"equity\": " << num(equity.get())
       << ", \"cash\": " << num(cash.get()) << ", \"realized_pnl\": " << num(realized_pnl.get())
       << ", \"unrealized_pnl\": " << num(unrealized_pnl.get())
       << ", \"gross_exposure\": " << num(gross_exposure.get())
       << ", \"net_exposure\": " << num(net_exposure.get()) << ", \"drawdown\": " << num(drawdown)
       << ", \"risk_rejections\": " << risk_rejections << ", \"active_alerts\": " << active_alerts
       << '}';
    return ss.str();
}

std::string RuntimeDiagnostics::to_json() const {
    return snapshot().to_json();
}

// ---------------------------------------------------------------------------
// ServiceSupervisor
// ---------------------------------------------------------------------------

std::string_view to_string(ServiceState s) noexcept {
    switch (s) {
        case ServiceState::Stopped:
            return "stopped";
        case ServiceState::Starting:
            return "starting";
        case ServiceState::Running:
            return "running";
        case ServiceState::Draining:
            return "draining";
        case ServiceState::Stopping:
            return "stopping";
        case ServiceState::Failed:
            return "failed";
    }
    return "unknown";
}

Result<bool> ServiceSupervisor::start() {
    if (state_ == ServiceState::Running) return true;
    if (state_ == ServiceState::Draining || state_ == ServiceState::Stopping) {
        return fail(
            bad("cannot start while shutting down; wait for the drain to "
                "complete"));
    }
    state_ = ServiceState::Starting;
    reason_.clear();
    state_ = ServiceState::Running;
    return true;
}

Result<bool> ServiceSupervisor::begin_drain(std::string reason) {
    if (state_ != ServiceState::Running) {
        return fail(
            bad("only a running service can drain; this one is " + std::string{to_string(state_)}));
    }
    state_ = ServiceState::Draining;
    reason_ = std::move(reason);
    drain_began_ = clock_->now();
    return true;
}

bool ServiceSupervisor::drain_complete(std::size_t in_flight) const {
    if (state_ != ServiceState::Draining) return true;
    if (in_flight == 0) return true;

    // The timeout is a BACKSTOP, not the normal path. Waiting forever for one
    // stuck order would turn a graceful shutdown into a hang, which is the
    // failure mode a graceful shutdown exists to prevent.
    const Timestamp now = clock_->now();
    if (!is_set(drain_began_) || !is_set(now)) return false;
    return now - drain_began_ >= config_.drain_timeout;
}

Result<bool> ServiceSupervisor::stop() {
    if (state_ == ServiceState::Stopped) return true;
    state_ = ServiceState::Stopping;
    state_ = ServiceState::Stopped;
    return true;
}

void ServiceSupervisor::prune_restarts() {
    const Timestamp now = clock_->now();
    if (!is_set(now)) return;
    // Restarts are counted over a WINDOW. A service that restarted twice last
    // month is not the same as one restarting twice a minute.
    const auto cutoff = now - config_.restart_window;
    restarts_.erase(std::remove_if(restarts_.begin(), restarts_.end(),
                                   [cutoff](Timestamp t) { return t < cutoff; }),
                    restarts_.end());
}

Result<bool> ServiceSupervisor::record_failure(std::string reason) {
    state_ = ServiceState::Failed;
    reason_ = std::move(reason);
    restarts_.push_back(clock_->now());
    prune_restarts();
    return true;
}

bool ServiceSupervisor::should_restart() const {
    if (!config_.auto_restart) return false;
    if (state_ != ServiceState::Failed) return false;
    // A service that fails repeatedly needs a human. An unbounded restart loop
    // hides the fault while the account keeps trading through it.
    return restarts_.size() <= config_.max_restarts;
}

std::string ServiceSupervisor::to_json() const {
    std::ostringstream ss;
    ss << "{\"state\": \"" << to_string(state_) << "\", \"restarts\": " << restarts_.size()
       << ", \"should_restart\": " << (should_restart() ? "true" : "false") << ", \"reason\": \""
       << json_escape(reason_) << "\"}";
    return ss.str();
}

}  // namespace ptl::ops
