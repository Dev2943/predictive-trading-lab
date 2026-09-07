#pragma once

/// \file metrics.hpp
/// Operational telemetry.
///
/// NOT analytics::MetricsEngine. That computes what the STRATEGY did -- Sharpe,
/// drawdown, attribution -- from a completed equity curve. This counts what the
/// PROCESS did: orders sent, rejects, latencies, queue depths, bytes. The two
/// are asked by different people at different times, and merging them would put
/// a p99 latency histogram inside a performance report.
///
/// OBSERVE-ONLY. Nothing in ptl::ops may change trading behaviour. Every type
/// here records; none decides. The one exception is CircuitBreaker (health.hpp),
/// which is explicitly a control and is documented as such.
///
/// DETERMINISM. Recording a metric must not perturb a replay, so every counter
/// is a plain integer and every timestamp is supplied by the caller's clock.
/// ResourceMonitor is the sole component that reads the operating system, and
/// it is deliberately quarantined behind an interface for exactly that reason.

#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "ptl/core/clock.hpp"
#include "ptl/core/result.hpp"
#include "ptl/core/types.hpp"

namespace ptl::ops {

enum class MetricKind : std::uint8_t {
    /// Monotonically increasing. Never reset during a session: a counter that
    /// can go down cannot be differenced across a scrape interval.
    Counter,
    /// Instantaneous value that may rise or fall.
    Gauge,
    /// Distribution, summarised by quantiles.
    Histogram,
};

[[nodiscard]] std::string_view to_string(MetricKind) noexcept;

/// Fixed-bucket latency histogram.
///
/// Buckets rather than stored samples: a live session records millions of
/// latencies and keeping them all to compute an exact quantile would use
/// unbounded memory for a number nobody needs to more than two digits.
///
/// Bucket bounds are POWERS OF TEN IN MICROSECONDS. A latency histogram whose
/// buckets are linear wastes all its resolution on the tail and has none where
/// the median lives.
class LatencyHistogram {
public:
    LatencyHistogram();

    void record(Duration) noexcept;
    void record_micros(double) noexcept;

    [[nodiscard]] std::size_t count() const noexcept { return count_; }
    [[nodiscard]] double mean_micros() const noexcept;
    [[nodiscard]] double max_micros() const noexcept { return max_micros_; }
    [[nodiscard]] double min_micros() const noexcept;

    /// Interpolated within the containing bucket. Approximate by construction,
    /// and documented as such so nobody builds an SLA on the third digit.
    [[nodiscard]] double quantile_micros(double q) const noexcept;
    [[nodiscard]] double p50() const noexcept { return quantile_micros(0.50); }
    [[nodiscard]] double p95() const noexcept { return quantile_micros(0.95); }
    [[nodiscard]] double p99() const noexcept { return quantile_micros(0.99); }

    void reset() noexcept;
    [[nodiscard]] std::string to_json() const;

private:
    static constexpr std::size_t kBuckets = 12;
    std::array<std::uint64_t, kBuckets> counts_{};
    std::array<double, kBuckets> bounds_{};
    std::size_t count_ = 0;
    double sum_micros_ = 0.0;
    double max_micros_ = 0.0;
    double min_micros_ = 0.0;
};

struct MetricSnapshot {
    std::string name;
    MetricKind kind{MetricKind::Counter};
    double value = 0.0;
    /// Populated for histograms only.
    std::optional<double> p50;
    std::optional<double> p95;
    std::optional<double> p99;
    std::size_t samples = 0;
};

/// The operational metric store.
///
/// NO SINGLETON. A registry is an object the caller owns, so two sessions in one
/// process do not silently share counters -- which would make every per-session
/// rate meaningless.
class MetricsRegistry {
public:
    /// Counters and gauges are created on first use. Pre-registration would add
    /// a failure mode ("metric not declared") for no benefit at this scale.
    void increment(std::string_view name, std::uint64_t by = 1);
    void set_gauge(std::string_view name, double value);
    void observe(std::string_view name, Duration latency);
    void observe_micros(std::string_view name, double micros);

    [[nodiscard]] std::uint64_t counter(std::string_view name) const noexcept;
    [[nodiscard]] double gauge(std::string_view name) const noexcept;
    [[nodiscard]] const LatencyHistogram* histogram(std::string_view name) const noexcept;

    /// Ratio of two counters, guarding the zero denominator that a naive reject
    /// rate hits on its first scrape.
    [[nodiscard]] double ratio(std::string_view numerator,
                               std::string_view denominator) const noexcept;

    /// All metrics, ordered by name so two scrapes are diffable.
    [[nodiscard]] std::vector<MetricSnapshot> snapshot() const;
    [[nodiscard]] std::string to_json() const;

    [[nodiscard]] std::size_t size() const noexcept;
    void reset() noexcept;

private:
    std::map<std::string, std::uint64_t, std::less<>> counters_;
    std::map<std::string, double, std::less<>> gauges_;
    std::map<std::string, LatencyHistogram, std::less<>> histograms_;
};

/// Process resource usage.
struct ResourceSample {
    Timestamp ts{kNoTimestamp};
    /// Resident set size in bytes. Zero when unavailable.
    std::uint64_t resident_bytes = 0;
    std::uint64_t virtual_bytes = 0;
    /// Cumulative CPU seconds consumed by this process.
    double cpu_seconds = 0.0;
    /// CPU utilisation since the previous sample, as a fraction of one core.
    double cpu_fraction = 0.0;
    std::size_t open_file_descriptors = 0;
    bool available = false;

    [[nodiscard]] std::string to_json() const;
};

/// Reads process resource usage from the operating system.
///
/// QUARANTINED BEHIND AN INTERFACE. This is the only component in the codebase
/// that reads /proc, and its output is non-deterministic by nature. A test
/// substitutes a fake so that nothing which depends on resource pressure
/// becomes untestable or replay-divergent.
class IResourceReader {
public:
    IResourceReader() = default;
    virtual ~IResourceReader() = default;
    IResourceReader(const IResourceReader&) = delete;
    IResourceReader& operator=(const IResourceReader&) = delete;

protected:
    IResourceReader(IResourceReader&&) = default;
    IResourceReader& operator=(IResourceReader&&) = default;

public:
    /// \returns raw resident bytes, virtual bytes, cpu seconds, fd count.
    [[nodiscard]] virtual Result<ResourceSample> read() = 0;
};

/// Reads from /proc on Linux. Returns an unavailable sample elsewhere rather
/// than failing: an absent resource reading is not a reason to stop trading.
class ProcResourceReader final : public IResourceReader {
public:
    [[nodiscard]] Result<ResourceSample> read() override;
};

class ResourceMonitor {
public:
    ResourceMonitor(const IClock& clock, IResourceReader& reader) noexcept
        : clock_(&clock), reader_(&reader) {}

    /// Take a sample and compute CPU utilisation since the last one.
    [[nodiscard]] Result<ResourceSample> sample();
    [[nodiscard]] const ResourceSample& last() const noexcept { return last_; }
    /// Feed the current sample into a registry as gauges.
    void publish(MetricsRegistry&) const;

private:
    const IClock* clock_;
    IResourceReader* reader_;
    ResourceSample last_;
    bool have_previous_ = false;
};

/// Canonical metric names.
///
/// Constants rather than string literals at each call site: a typo in a metric
/// name produces a second metric that silently reads zero, and the dashboard
/// looks healthy because the real counter is under a name nobody queries.
namespace metric_names {
inline constexpr std::string_view kOrdersSubmitted = "orders.submitted";
inline constexpr std::string_view kOrdersFilled = "orders.filled";
inline constexpr std::string_view kOrdersRejected = "orders.rejected";
inline constexpr std::string_view kOrdersCancelled = "orders.cancelled";
inline constexpr std::string_view kFillsReceived = "fills.received";
inline constexpr std::string_view kMessagesReceived = "messages.received";
inline constexpr std::string_view kMessagesDropped = "messages.dropped";
inline constexpr std::string_view kReconnects = "connection.reconnects";
inline constexpr std::string_view kOrderLatency = "latency.order_round_trip";
inline constexpr std::string_view kDecisionLatency = "latency.decision";
inline constexpr std::string_view kEventLatency = "latency.event_processing";
inline constexpr std::string_view kQueueDepth = "queue.depth";
inline constexpr std::string_view kEquity = "portfolio.equity";
inline constexpr std::string_view kDrawdown = "portfolio.drawdown";
inline constexpr std::string_view kGrossExposure = "portfolio.gross_exposure";
inline constexpr std::string_view kNetExposure = "portfolio.net_exposure";
inline constexpr std::string_view kRealizedPnl = "portfolio.realized_pnl";
inline constexpr std::string_view kUnrealizedPnl = "portfolio.unrealized_pnl";
inline constexpr std::string_view kCpuFraction = "resource.cpu_fraction";
inline constexpr std::string_view kResidentBytes = "resource.resident_bytes";
inline constexpr std::string_view kOpenFds = "resource.open_fds";
}  // namespace metric_names

}  // namespace ptl::ops
