#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <limits>
#include <map>
#include <string>
#include <vector>

#include "ptl/ops/diagnostics.hpp"
#include "support/ptl_catch.hpp"

using namespace ptl;
using namespace ptl::ops;
using namespace std::chrono;

namespace {

Timestamp at(const char* iso) {
    Timestamp ts{};
    REQUIRE(parse_timestamp(iso, ts));
    return ts;
}

/// A resource reader with no operating system behind it. The real one reads
/// /proc and is non-deterministic by nature; substituting it is what keeps
/// everything that depends on resource pressure testable.
class FakeResourceReader final : public IResourceReader {
public:
    [[nodiscard]] Result<ResourceSample> read() override {
        if (fail) return fail_with;
        ResourceSample sample;
        sample.available = available;
        sample.resident_bytes = resident;
        sample.cpu_seconds = cpu_seconds;
        sample.open_file_descriptors = fds;
        return sample;
    }

    bool available = true;
    bool fail = false;
    Result<ResourceSample> fail_with =
        ptl::fail(make_error(ErrorCode::IoError, "scripted failure"));
    std::uint64_t resident = 100 * 1024 * 1024;
    double cpu_seconds = 0.0;
    std::size_t fds = 12;
};

}  // namespace

// ---------------------------------------------------------------------------
// Metrics
// ---------------------------------------------------------------------------

TEST_CASE("counters accumulate and gauges replace", "[ops][metrics]") {
    MetricsRegistry registry;
    registry.increment(metric_names::kOrdersSubmitted);
    registry.increment(metric_names::kOrdersSubmitted, 4);
    REQUIRE(registry.counter(metric_names::kOrdersSubmitted) == 5);

    registry.set_gauge(metric_names::kEquity, 100.0);
    registry.set_gauge(metric_names::kEquity, 250.0);
    REQUIRE(registry.gauge(metric_names::kEquity) == Catch::Approx(250.0));

    // An unknown metric reads zero rather than failing: a dashboard querying a
    // counter that has not fired yet is normal.
    REQUIRE(registry.counter("never.incremented") == 0);
    REQUIRE(registry.gauge("never.set") == Catch::Approx(0.0));
}

TEST_CASE("a ratio with a zero denominator is zero, not NaN", "[ops][metrics][edge]") {
    // A reject rate on the first scrape has no samples, and a NaN there poisons
    // every alert threshold downstream.
    MetricsRegistry registry;
    REQUIRE(registry.ratio(metric_names::kOrdersRejected, metric_names::kOrdersSubmitted) ==
            Catch::Approx(0.0));

    registry.increment(metric_names::kOrdersSubmitted, 10);
    registry.increment(metric_names::kOrdersRejected, 3);
    REQUIRE(registry.ratio(metric_names::kOrdersRejected, metric_names::kOrdersSubmitted) ==
            Catch::Approx(0.3));
}

TEST_CASE("a non-finite gauge is dropped, not stored", "[ops][metrics][edge]") {
    // It would propagate into every aggregate that reads it, and a missing
    // sample is easier to notice than a NaN.
    MetricsRegistry registry;
    registry.set_gauge(metric_names::kEquity, 100.0);
    registry.set_gauge(metric_names::kEquity, std::numeric_limits<double>::quiet_NaN());
    REQUIRE(registry.gauge(metric_names::kEquity) == Catch::Approx(100.0));
}

TEST_CASE("the latency histogram summarises a distribution", "[ops][metrics][property]") {
    LatencyHistogram histogram;
    for (int i = 1; i <= 1000; ++i) {
        histogram.record_micros(static_cast<double>(i));
    }
    REQUIRE(histogram.count() == 1000);
    REQUIRE(histogram.max_micros() == Catch::Approx(1000.0));
    // Bucketed and interpolated, so the quantiles are approximate by
    // construction -- the assertion is deliberately loose.
    REQUIRE(histogram.p50() > 100.0);
    REQUIRE(histogram.p50() < 900.0);
    REQUIRE(histogram.p99() >= histogram.p50());
    REQUIRE(histogram.mean_micros() == Catch::Approx(500.5).epsilon(0.01));
}

TEST_CASE("a negative latency is refused", "[ops][metrics][edge]") {
    // Time cannot run backwards; a negative sample is a bug upstream and
    // recording it would corrupt every quantile.
    LatencyHistogram histogram;
    histogram.record_micros(-5.0);
    histogram.record_micros(std::numeric_limits<double>::quiet_NaN());
    REQUIRE(histogram.count() == 0);
    // An empty histogram answers zero rather than dividing by nothing.
    REQUIRE(histogram.p50() == Catch::Approx(0.0));
    REQUIRE(histogram.mean_micros() == Catch::Approx(0.0));
}

TEST_CASE("metric snapshots are ordered and diffable", "[ops][metrics][determinism]") {
    MetricsRegistry registry;
    registry.increment("zebra.counter");
    registry.increment("alpha.counter");
    registry.set_gauge("middle.gauge", 1.0);
    registry.observe_micros("beta.latency", 100.0);

    const auto snapshot = registry.snapshot();
    REQUIRE(snapshot.size() == 4);
    for (std::size_t i = 1; i < snapshot.size(); ++i) {
        REQUIRE(snapshot[i - 1].name < snapshot[i].name);
    }
    // Same input, same bytes.
    REQUIRE(registry.to_json() == registry.to_json());
}

TEST_CASE("CPU utilisation needs two samples", "[ops][metrics][edge]") {
    // The first sample has no interval to divide by; reporting a fraction
    // computed against process start would show a lifetime average as though
    // it were current load.
    SimulatedClock clock{at("2024-07-02T15:00:00Z")};
    FakeResourceReader reader;
    ResourceMonitor monitor{clock, reader};

    auto first = monitor.sample();
    REQUIRE(first.has_value());
    REQUIRE(first->cpu_fraction == Catch::Approx(0.0));

    clock.advance_by(seconds{10});
    reader.cpu_seconds = 5.0;  // five CPU seconds over ten wall seconds
    auto second = monitor.sample();
    REQUIRE(second.has_value());
    REQUIRE(second->cpu_fraction == Catch::Approx(0.5));

    MetricsRegistry registry;
    monitor.publish(registry);
    REQUIRE(registry.gauge(metric_names::kCpuFraction) == Catch::Approx(0.5));
}

TEST_CASE("an unavailable resource reading is not a failure", "[ops][metrics][edge]") {
    // An absent /proc is not a reason to stop trading.
    SimulatedClock clock{at("2024-07-02T15:00:00Z")};
    FakeResourceReader reader;
    reader.available = false;
    ResourceMonitor monitor{clock, reader};

    auto sample = monitor.sample();
    REQUIRE(sample.has_value());
    REQUIRE_FALSE(sample->available);

    MetricsRegistry registry;
    monitor.publish(registry);
    // Nothing published, rather than zeros that would read as real readings.
    REQUIRE(registry.size() == 0);
}

// ---------------------------------------------------------------------------
// Health
// ---------------------------------------------------------------------------

TEST_CASE("an unreported component is Unknown, never Healthy", "[ops][health][leakage]") {
    // Defaulting to Healthy would make a subsystem that never starts invisible.
    SimulatedClock clock{at("2024-07-02T15:00:00Z")};
    HealthMonitor monitor{clock};

    REQUIRE(monitor.register_component("market_data").has_value());
    REQUIRE(monitor.status_of("market_data") == HealthStatus::Unknown);
    REQUIRE_FALSE(monitor.trading_permitted());

    monitor.report_healthy("market_data");
    REQUIRE(monitor.status_of("market_data") == HealthStatus::Healthy);
    REQUIRE(monitor.trading_permitted());

    REQUIRE_FALSE(monitor.register_component("market_data").has_value());
    REQUIRE_FALSE(monitor.register_component("").has_value());
}

TEST_CASE("a stale green becomes Unknown", "[ops][health][property]") {
    // A stale green is worse than a red, because nobody looks at it.
    HealthConfig config;
    config.staleness_timeout = seconds{60};
    SimulatedClock clock{at("2024-07-02T15:00:00Z")};
    HealthMonitor monitor{clock, config};

    REQUIRE(monitor.register_component("broker").has_value());
    monitor.report_healthy("broker");
    REQUIRE(monitor.status_of("broker") == HealthStatus::Healthy);

    clock.advance_by(seconds{120});
    REQUIRE(monitor.status_of("broker") == HealthStatus::Unknown);
    REQUIRE_FALSE(monitor.trading_permitted());
}

TEST_CASE("one failure degrades, repeated failure goes unhealthy", "[ops][health][property]") {
    // Escalating on the first wobble would halt trading over one slow response.
    HealthConfig config;
    config.failures_before_unhealthy = 3;
    SimulatedClock clock{at("2024-07-02T15:00:00Z")};
    HealthMonitor monitor{clock, config};
    REQUIRE(monitor.register_component("oms").has_value());

    monitor.report_failure("oms", "slow");
    REQUIRE(monitor.status_of("oms") == HealthStatus::Degraded);
    // Degraded still trades.
    REQUIRE(monitor.trading_permitted());

    monitor.report_failure("oms", "slow");
    monitor.report_failure("oms", "slow");
    REQUIRE(monitor.status_of("oms") == HealthStatus::Unhealthy);
    REQUIRE_FALSE(monitor.trading_permitted());

    // One success clears the streak.
    monitor.report_healthy("oms");
    REQUIRE(monitor.status_of("oms") == HealthStatus::Healthy);
}

TEST_CASE("only critical components gate trading", "[ops][health][property]") {
    // A market data feed is critical; a metrics endpoint is not.
    SimulatedClock clock{at("2024-07-02T15:00:00Z")};
    HealthMonitor monitor{clock};
    REQUIRE(monitor.register_component("market_data", true).has_value());
    REQUIRE(monitor.register_component("metrics_scrape", false).has_value());

    monitor.report_healthy("market_data");
    monitor.report("metrics_scrape", HealthStatus::Unhealthy, "endpoint down");

    // Overall is unhealthy, but trading continues.
    REQUIRE(monitor.overall() == HealthStatus::Unhealthy);
    REQUIRE(monitor.critical_status() == HealthStatus::Healthy);
    REQUIRE(monitor.trading_permitted());
}

TEST_CASE("Unknown outranks Degraded when aggregating", "[ops][health][property]") {
    // A component nobody has heard from is more alarming than one reporting a
    // known wobble.
    REQUIRE(worse_of(HealthStatus::Degraded, HealthStatus::Unknown) == HealthStatus::Unknown);
    REQUIRE(worse_of(HealthStatus::Unknown, HealthStatus::Unhealthy) == HealthStatus::Unhealthy);
    REQUIRE(worse_of(HealthStatus::Healthy, HealthStatus::Degraded) == HealthStatus::Degraded);
}

// ---------------------------------------------------------------------------
// Circuit breaker
// ---------------------------------------------------------------------------

TEST_CASE("the breaker trips on consecutive failures", "[ops][breaker][property]") {
    CircuitBreakerConfig config;
    config.failure_threshold = 3;
    config.open_duration = seconds{30};
    SimulatedClock clock{at("2024-07-02T15:00:00Z")};
    CircuitBreaker breaker{clock, "broker", config};

    REQUIRE(breaker.allow());
    breaker.record_failure();
    breaker.record_failure();
    REQUIRE(breaker.state() == BreakerState::Closed);

    breaker.record_failure();
    REQUIRE(breaker.state() == BreakerState::Open);
    REQUIRE_FALSE(breaker.allow());
    REQUIRE(breaker.trips() == 1);
}

TEST_CASE("the breaker trips on a failure rate a streak would miss", "[ops][breaker][property]") {
    // Nine failures alternating with one success never trips a consecutive
    // counter.
    CircuitBreakerConfig config;
    config.failure_threshold = 100;  // effectively disabled
    config.rate_threshold = 0.5;
    config.rate_minimum_samples = 10;
    SimulatedClock clock{at("2024-07-02T15:00:00Z")};
    CircuitBreaker breaker{clock, "venue", config};

    for (int i = 0; i < 20; ++i) {
        if (i % 2 == 0)
            breaker.record_failure();
        else
            breaker.record_success();
    }
    REQUIRE(breaker.failure_rate() >= 0.5);
    REQUIRE(breaker.state() == BreakerState::Open);
}

TEST_CASE("a tripped breaker probes before closing", "[ops][breaker][property]") {
    // Slamming back to full traffic the moment the timeout expires is how a
    // recovering venue gets knocked over a second time.
    CircuitBreakerConfig config;
    config.failure_threshold = 1;
    config.open_duration = seconds{30};
    config.probes_to_close = 2;
    SimulatedClock clock{at("2024-07-02T15:00:00Z")};
    CircuitBreaker breaker{clock, "venue", config};

    breaker.record_failure();
    REQUIRE(breaker.state() == BreakerState::Open);
    REQUIRE_FALSE(breaker.allow());

    clock.advance_by(seconds{31});
    REQUIRE(breaker.allow());
    REQUIRE(breaker.state() == BreakerState::HalfOpen);

    breaker.record_success();
    REQUIRE(breaker.state() == BreakerState::HalfOpen);
    breaker.record_success();
    REQUIRE(breaker.state() == BreakerState::Closed);
}

TEST_CASE("a failed probe reopens the breaker immediately", "[ops][breaker][edge]") {
    // Continuing to probe a venue that just failed is how a breaker becomes
    // decorative.
    CircuitBreakerConfig config;
    config.failure_threshold = 1;
    config.open_duration = seconds{10};
    SimulatedClock clock{at("2024-07-02T15:00:00Z")};
    CircuitBreaker breaker{clock, "venue", config};

    breaker.record_failure();
    clock.advance_by(seconds{11});
    REQUIRE(breaker.allow());
    REQUIRE(breaker.state() == BreakerState::HalfOpen);

    breaker.record_failure();
    REQUIRE(breaker.state() == BreakerState::Open);
    REQUIRE(breaker.trips() == 2);

    breaker.reset();
    REQUIRE(breaker.state() == BreakerState::Closed);
}

// ---------------------------------------------------------------------------
// Watchdog
// ---------------------------------------------------------------------------

TEST_CASE("a watchdog that was never kicked has not expired", "[ops][watchdog][edge]") {
    // Firing before the loop has started would alert on every startup.
    SimulatedClock clock{at("2024-07-02T15:00:00Z")};
    Watchdog watchdog{clock, "event_loop", seconds{10}};

    REQUIRE_FALSE(watchdog.expired());
    clock.advance_by(hours{1});
    REQUIRE_FALSE(watchdog.expired());

    watchdog.kick();
    REQUIRE_FALSE(watchdog.expired());
    clock.advance_by(seconds{11});
    REQUIRE(watchdog.expired());
}

TEST_CASE("checking a watchdog counts an expiry only once per check", "[ops][watchdog]") {
    // A poll must not silently inflate the count.
    SimulatedClock clock{at("2024-07-02T15:00:00Z")};
    Watchdog watchdog{clock, "loop", seconds{5}};
    watchdog.kick();

    REQUIRE_FALSE(watchdog.check_and_count());
    REQUIRE(watchdog.expirations() == 0);

    clock.advance_by(seconds{6});
    REQUIRE(watchdog.expired());
    REQUIRE(watchdog.expirations() == 0);  // expired() alone does not count
    REQUIRE(watchdog.check_and_count());
    REQUIRE(watchdog.expirations() == 1);

    watchdog.kick();
    REQUIRE_FALSE(watchdog.check_and_count());
}

// ---------------------------------------------------------------------------
// Alerts
// ---------------------------------------------------------------------------

TEST_CASE("repeated alerts deduplicate into one with a count", "[ops][alerts][property]") {
    // Four hundred alerts is indistinguishable from none.
    SimulatedClock clock{at("2024-07-02T15:00:00Z")};
    AlertManager alerts{clock};

    for (int i = 0; i < 50; ++i) {
        alerts.raise(std::string{alert_keys::kBrokerDisconnected}, AlertSeverity::Error,
                     "broker disconnected");
    }
    REQUIRE(alerts.active_count() == 1);
    REQUIRE(alerts.raised_total() == 50);
    REQUIRE(alerts.suppressed() == 49);

    const auto alert = alerts.find(alert_keys::kBrokerDisconnected);
    REQUIRE(alert.has_value());
    REQUIRE(alert->occurrences == 50);
}

TEST_CASE("alert severity escalates but never silently downgrades", "[ops][alerts][property]") {
    // A recurrence arriving as Info must not mask an Error already raised.
    SimulatedClock clock{at("2024-07-02T15:00:00Z")};
    AlertManager alerts{clock};

    alerts.raise("k", AlertSeverity::Warning, "first");
    alerts.raise("k", AlertSeverity::Critical, "worse");
    REQUIRE(alerts.find("k")->severity == AlertSeverity::Critical);

    alerts.raise("k", AlertSeverity::Info, "milder");
    REQUIRE(alerts.find("k")->severity == AlertSeverity::Critical);
    REQUIRE(alerts.worst_active().value() == AlertSeverity::Critical);
}

TEST_CASE("resolving moves an alert to history", "[ops][alerts][recovery]") {
    SimulatedClock clock{at("2024-07-02T15:00:00Z")};
    AlertManager alerts{clock};

    alerts.raise("k", AlertSeverity::Error, "down");
    REQUIRE(alerts.active_count() == 1);

    alerts.resolve("k");
    REQUIRE(alerts.active_count() == 0);
    REQUIRE(alerts.history().size() == 1);
    REQUIRE(alerts.history().front().resolved);

    // Resolving twice is normal in a recovery path, so it is silent.
    alerts.resolve("k");
    REQUIRE(alerts.history().size() == 1);
    REQUIRE_FALSE(alerts.worst_active().has_value());
}

TEST_CASE("an empty alert key is refused", "[ops][alerts][edge]") {
    SimulatedClock clock{at("2024-07-02T15:00:00Z")};
    AlertManager alerts{clock};
    alerts.raise("", AlertSeverity::Error, "nameless");
    REQUIRE(alerts.active_count() == 0);
}

// ---------------------------------------------------------------------------
// Config validation
// ---------------------------------------------------------------------------

TEST_CASE("validation collects every problem, not just the first", "[ops][config][property]") {
    // An operator fixing a config at 6am should get one list, not six
    // consecutive failed starts.
    risk::RiskLimits broken;
    broken.max_order_notional = Notional{0.0};
    broken.max_position_notional = Notional{0.0};
    broken.max_gross_leverage = 0.0;
    broken.max_concentration = 5.0;
    broken.max_drawdown_pct = 0.0;

    const ConfigValidator validator;
    const auto report = validator.validate_risk(broken);
    REQUIRE_FALSE(report.ok());
    // Five independent problems, all reported together.
    REQUIRE(report.fatal_count() >= 4);
    REQUIRE(report.describe().find("max_order_notional") != std::string::npos);
    REQUIRE(report.describe().find("max_drawdown_pct") != std::string::npos);
}

TEST_CASE("a zero risk limit is fatal, not a warning", "[ops][config][leakage]") {
    // It parses perfectly and halts the strategy on its first order.
    risk::RiskLimits limits;
    limits.max_order_notional = Notional{0.0};

    const ConfigValidator validator;
    const auto report = validator.validate_risk(limits);
    REQUIRE_FALSE(report.ok());

    bool fatal_on_notional = false;
    for (const auto& issue : report.issues) {
        if (issue.field == "risk.max_order_notional") {
            fatal_on_notional = issue.severity == Severity::Fatal;
        }
    }
    REQUIRE(fatal_on_notional);
}

TEST_CASE("a limit too large to bind is warned about", "[ops][config][edge]") {
    // A limit that cannot trigger provides no protection but reads as
    // protection on a control report.
    risk::RiskLimits limits;
    limits.max_gross_leverage = 1e9;

    const ConfigValidator validator;
    const auto report = validator.validate_risk(limits);
    REQUIRE(report.ok());  // a warning, not fatal
    REQUIRE(report.warning_count() >= 1);
}

TEST_CASE("missing credentials are fatal and never echoed", "[ops][config][leakage]") {
    // A validator that prints the key it is checking is a credential leak in
    // the startup log.
    ConfigValidator::Options options;
    options.require_credentials = true;
    const ConfigValidator validator{options};

    std::map<std::string, std::string, std::less<>> environment;
    environment["PTL_BROKER_KEY"] = "super-secret-value";
    // PTL_BROKER_SECRET deliberately absent.

    const auto report = validator.validate_credentials(environment);
    REQUIRE_FALSE(report.ok());
    const auto text = report.describe() + report.to_json();
    REQUIRE(text.find("PTL_BROKER_SECRET") != std::string::npos);
    // The VALUE of the credential that IS set never appears anywhere.
    REQUIRE(text.find("super-secret-value") == std::string::npos);
}

TEST_CASE("credentials are not required for a backtest", "[ops][config][edge]") {
    // Demanding an API key would block every research run.
    const ConfigValidator validator;
    REQUIRE(validator.validate_credentials({}).ok());
}

TEST_CASE("a suspicious symbol is warned about", "[ops][config]") {
    ConfigValidator::Options options;
    options.expected_symbols = {"SPY", "spy lower", ""};
    const ConfigValidator validator{options};

    config::Config config;
    config.run.seed = 42;
    config.run.results_dir = "results";
    const auto report = validator.validate(config);
    REQUIRE_FALSE(report.ok());  // the empty symbol is fatal
    REQUIRE(report.warning_count() >= 1);
}

TEST_CASE("a zero seed is fatal", "[ops][config][determinism]") {
    // Parses perfectly and destroys reproducibility.
    config::Config config;
    config.run.seed = 0;
    config.run.results_dir = "results";

    const ConfigValidator validator;
    const auto report = validator.validate(config);
    REQUIRE_FALSE(report.ok());
    REQUIRE(report.describe().find("run.seed") != std::string::npos);
}

// ---------------------------------------------------------------------------
// Service supervisor
// ---------------------------------------------------------------------------

TEST_CASE("a graceful drain waits for in-flight work, then gives up",
          "[ops][supervisor][property]") {
    // Waiting forever for one stuck order would turn a graceful shutdown into a
    // hang, which is the failure a graceful shutdown exists to prevent.
    SupervisorConfig config;
    config.drain_timeout = seconds{30};
    SimulatedClock clock{at("2024-07-02T15:00:00Z")};
    ServiceSupervisor supervisor{clock, config};

    REQUIRE(supervisor.start().has_value());
    REQUIRE(supervisor.state() == ServiceState::Running);

    REQUIRE(supervisor.begin_drain("market close").has_value());
    REQUIRE(supervisor.state() == ServiceState::Draining);

    // Work outstanding, timeout not reached.
    REQUIRE_FALSE(supervisor.drain_complete(3));
    // No work left: done immediately.
    REQUIRE(supervisor.drain_complete(0));

    clock.advance_by(seconds{31});
    // Timed out with work still outstanding: proceed anyway.
    REQUIRE(supervisor.drain_complete(3));

    REQUIRE(supervisor.stop().has_value());
    REQUIRE(supervisor.state() == ServiceState::Stopped);
}

TEST_CASE("restarts are bounded within a window", "[ops][supervisor][recovery]") {
    // An unbounded restart loop hides the fault while the account keeps trading
    // through it.
    SupervisorConfig config;
    config.max_restarts = 2;
    config.restart_window = minutes{10};
    SimulatedClock clock{at("2024-07-02T15:00:00Z")};
    ServiceSupervisor supervisor{clock, config};

    REQUIRE(supervisor.start().has_value());
    REQUIRE(supervisor.record_failure("crash").has_value());
    REQUIRE(supervisor.should_restart());

    REQUIRE(supervisor.record_failure("crash").has_value());
    REQUIRE(supervisor.should_restart());

    REQUIRE(supervisor.record_failure("crash").has_value());
    // Past the cap: a human is needed.
    REQUIRE_FALSE(supervisor.should_restart());

    // Old failures age out of the window.
    clock.advance_by(minutes{20});
    REQUIRE(supervisor.record_failure("much later").has_value());
    REQUIRE(supervisor.should_restart());
}

TEST_CASE("a draining service cannot be restarted", "[ops][supervisor][edge]") {
    SimulatedClock clock{at("2024-07-02T15:00:00Z")};
    ServiceSupervisor supervisor{clock};
    REQUIRE(supervisor.start().has_value());
    REQUIRE(supervisor.begin_drain("shutdown").has_value());
    REQUIRE_FALSE(supervisor.start().has_value());
    // Draining a service that is not running is refused too.
    REQUIRE_FALSE(supervisor.begin_drain("again").has_value());
}

// ---------------------------------------------------------------------------
// Diagnostics
// ---------------------------------------------------------------------------

TEST_CASE("diagnostics read every subsystem without mutating them", "[ops][diagnostics][leakage]") {
    // Diagnostics that mutated what they inspect would change behaviour under
    // observation, which is the one thing an observability layer must not do.
    SimulatedClock clock{at("2024-07-02T15:00:00Z")};
    portfolio::Portfolio portfolio;
    oms::OrderManager oms;
    risk::RiskManager risk{risk::RiskLimits{}};
    HealthMonitor health{clock};
    AlertManager alerts{clock};
    MetricsRegistry metrics;

    REQUIRE(health.register_component("market_data").has_value());
    health.report_healthy("market_data");

    RuntimeDiagnostics diagnostics{clock, portfolio, oms, risk, health, alerts, metrics};
    diagnostics.set_session("live1", "running");
    diagnostics.set_active_strategies(3);

    const auto equity_before = portfolio.equity().get();
    const auto working_before = oms.working().size();

    const auto snapshot = diagnostics.snapshot();
    REQUIRE(snapshot.session_id == "live1");
    REQUIRE(snapshot.active_strategies == 3);
    REQUIRE(snapshot.health == HealthStatus::Healthy);
    REQUIRE(snapshot.trading_permitted);
    REQUIRE(snapshot.equity.get() == Catch::Approx(equity_before));

    // Nothing observed was disturbed.
    REQUIRE(portfolio.equity().get() == equity_before);
    REQUIRE(oms.working().size() == working_before);

    REQUIRE(diagnostics.to_json() == diagnostics.to_json());
    REQUIRE(snapshot.describe().find("live1") != std::string::npos);
}

TEST_CASE("diagnostics read drawdown from metrics rather than recomputing it",
          "[ops][diagnostics][property]") {
    // The analytics layer owns the definition of drawdown; a second computation
    // in a diagnostics dump could disagree with the risk engine that halts on
    // it.
    SimulatedClock clock{at("2024-07-02T15:00:00Z")};
    portfolio::Portfolio portfolio;
    oms::OrderManager oms;
    risk::RiskManager risk{risk::RiskLimits{}};
    HealthMonitor health{clock};
    AlertManager alerts{clock};
    MetricsRegistry metrics;

    metrics.set_gauge(metric_names::kDrawdown, 0.0725);

    RuntimeDiagnostics diagnostics{clock, portfolio, oms, risk, health, alerts, metrics};
    REQUIRE(diagnostics.snapshot().drawdown == Catch::Approx(0.0725));
}

TEST_CASE("publishing diagnostics writes only to metrics", "[ops][diagnostics][property]") {
    SimulatedClock clock{at("2024-07-02T15:00:00Z")};
    portfolio::Portfolio portfolio;
    oms::OrderManager oms;
    risk::RiskManager risk{risk::RiskLimits{}};
    HealthMonitor health{clock};
    AlertManager alerts{clock};
    MetricsRegistry metrics;

    RuntimeDiagnostics diagnostics{clock, portfolio, oms, risk, health, alerts, metrics};
    MetricsRegistry target;
    diagnostics.publish_metrics(target);

    REQUIRE(target.gauge(metric_names::kEquity) == Catch::Approx(portfolio.equity().get()));
    REQUIRE(target.size() > 0);
}

TEST_CASE("an alert raised is visible in diagnostics", "[ops][diagnostics]") {
    SimulatedClock clock{at("2024-07-02T15:00:00Z")};
    portfolio::Portfolio portfolio;
    oms::OrderManager oms;
    risk::RiskManager risk{risk::RiskLimits{}};
    HealthMonitor health{clock};
    AlertManager alerts{clock};
    MetricsRegistry metrics;

    alerts.raise(std::string{alert_keys::kRiskBreach}, AlertSeverity::Critical,
                 "drawdown limit hit");

    RuntimeDiagnostics diagnostics{clock, portfolio, oms, risk, health, alerts, metrics};
    const auto snapshot = diagnostics.snapshot();
    REQUIRE(snapshot.active_alerts == 1);
    REQUIRE(snapshot.worst_alert.value() == AlertSeverity::Critical);
}
