#include "ptl/ops/metrics.hpp"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>

namespace ptl::ops {
namespace {

[[nodiscard]] std::string num(double v, int precision = 6) {
    // JSON has no NaN literal; null is the honest representation of a metric
    // that could not be computed.
    if (!is_finite(v)) return "null";
    std::ostringstream ss;
    ss << std::fixed << std::setprecision(precision) << v;
    return ss.str();
}

}  // namespace

std::string_view to_string(MetricKind k) noexcept {
    switch (k) {
        case MetricKind::Counter:
            return "counter";
        case MetricKind::Gauge:
            return "gauge";
        case MetricKind::Histogram:
            return "histogram";
    }
    return "unknown";
}

// ---------------------------------------------------------------------------
// LatencyHistogram
// ---------------------------------------------------------------------------

LatencyHistogram::LatencyHistogram() {
    // POWERS OF TEN, in microseconds: 1us .. ~100s. Linear buckets would spend
    // all their resolution on the tail and have none where the median lives.
    double bound = 1.0;
    for (std::size_t i = 0; i < kBuckets; ++i) {
        bounds_[i] = bound;
        bound *= 3.1622776601683795;  // sqrt(10): two buckets per decade
    }
    min_micros_ = std::numeric_limits<double>::infinity();
}

void LatencyHistogram::record(Duration d) noexcept {
    record_micros(static_cast<double>(d.count()) / 1000.0);
}

void LatencyHistogram::record_micros(double micros) noexcept {
    if (!is_finite(micros) || micros < 0.0) return;  // a negative latency is a bug upstream

    ++count_;
    sum_micros_ += micros;
    max_micros_ = std::max(max_micros_, micros);
    min_micros_ = std::min(min_micros_, micros);

    for (std::size_t i = 0; i < kBuckets; ++i) {
        if (micros <= bounds_[i]) {
            ++counts_[i];
            return;
        }
    }
    // Beyond the last bound: the overflow bucket, so an outlier still counts
    // toward the total rather than vanishing.
    ++counts_[kBuckets - 1];
}

double LatencyHistogram::mean_micros() const noexcept {
    if (count_ == 0) return 0.0;
    return sum_micros_ / static_cast<double>(count_);
}

double LatencyHistogram::min_micros() const noexcept {
    return count_ == 0 ? 0.0 : min_micros_;
}

double LatencyHistogram::quantile_micros(double q) const noexcept {
    if (count_ == 0 || !is_finite(q)) return 0.0;
    const double target = std::clamp(q, 0.0, 1.0) * static_cast<double>(count_);

    std::uint64_t cumulative = 0;
    for (std::size_t i = 0; i < kBuckets; ++i) {
        cumulative += counts_[i];
        if (static_cast<double>(cumulative) >= target) {
            // Interpolated within the bucket. Approximate by construction, and
            // documented so nobody builds an SLA on the third digit.
            const double lower = i == 0 ? 0.0 : bounds_[i - 1];
            const double upper = bounds_[i];
            const double in_bucket = static_cast<double>(counts_[i]);
            if (in_bucket <= 0.0) return upper;
            const double before = static_cast<double>(cumulative) - in_bucket;
            const double fraction = (target - before) / in_bucket;
            return lower + (upper - lower) * std::clamp(fraction, 0.0, 1.0);
        }
    }
    return max_micros_;
}

void LatencyHistogram::reset() noexcept {
    counts_.fill(0);
    count_ = 0;
    sum_micros_ = 0.0;
    max_micros_ = 0.0;
    min_micros_ = std::numeric_limits<double>::infinity();
}

std::string LatencyHistogram::to_json() const {
    std::ostringstream ss;
    ss << "{\"count\": " << count_ << ", \"mean_us\": " << num(mean_micros())
       << ", \"min_us\": " << num(min_micros()) << ", \"max_us\": " << num(max_micros_)
       << ", \"p50_us\": " << num(p50()) << ", \"p95_us\": " << num(p95())
       << ", \"p99_us\": " << num(p99()) << '}';
    return ss.str();
}

// ---------------------------------------------------------------------------
// MetricsRegistry
// ---------------------------------------------------------------------------

void MetricsRegistry::increment(std::string_view name, std::uint64_t by) {
    const auto it = counters_.find(name);
    if (it == counters_.end()) {
        counters_.emplace(std::string{name}, by);
        return;
    }
    it->second += by;
}

void MetricsRegistry::set_gauge(std::string_view name, double value) {
    // A non-finite gauge is dropped rather than stored: it would propagate into
    // every aggregate that reads it, and a missing sample is easier to notice
    // than a NaN.
    if (!is_finite(value)) return;
    const auto it = gauges_.find(name);
    if (it == gauges_.end()) {
        gauges_.emplace(std::string{name}, value);
        return;
    }
    it->second = value;
}

void MetricsRegistry::observe(std::string_view name, Duration latency) {
    observe_micros(name, static_cast<double>(latency.count()) / 1000.0);
}

void MetricsRegistry::observe_micros(std::string_view name, double micros) {
    const auto it = histograms_.find(name);
    if (it == histograms_.end()) {
        LatencyHistogram histogram;
        histogram.record_micros(micros);
        histograms_.emplace(std::string{name}, histogram);
        return;
    }
    it->second.record_micros(micros);
}

std::uint64_t MetricsRegistry::counter(std::string_view name) const noexcept {
    const auto it = counters_.find(name);
    return it == counters_.end() ? 0 : it->second;
}

double MetricsRegistry::gauge(std::string_view name) const noexcept {
    const auto it = gauges_.find(name);
    return it == gauges_.end() ? 0.0 : it->second;
}

const LatencyHistogram* MetricsRegistry::histogram(std::string_view name) const noexcept {
    const auto it = histograms_.find(name);
    return it == histograms_.end() ? nullptr : &it->second;
}

double MetricsRegistry::ratio(std::string_view numerator,
                              std::string_view denominator) const noexcept {
    const auto bottom = counter(denominator);
    // Zero denominator is ZERO, not NaN. A reject rate on the first scrape has
    // no samples, and a NaN there poisons every alert threshold downstream.
    if (bottom == 0) return 0.0;
    return static_cast<double>(counter(numerator)) / static_cast<double>(bottom);
}

std::size_t MetricsRegistry::size() const noexcept {
    return counters_.size() + gauges_.size() + histograms_.size();
}

std::vector<MetricSnapshot> MetricsRegistry::snapshot() const {
    std::vector<MetricSnapshot> out;
    out.reserve(size());

    // std::map iteration: ordered by name, so two scrapes are diffable and a
    // regression test can compare them byte for byte.
    for (const auto& [name, value] : counters_) {
        MetricSnapshot snapshot;
        snapshot.name = name;
        snapshot.kind = MetricKind::Counter;
        snapshot.value = static_cast<double>(value);
        out.push_back(std::move(snapshot));
    }
    for (const auto& [name, value] : gauges_) {
        MetricSnapshot snapshot;
        snapshot.name = name;
        snapshot.kind = MetricKind::Gauge;
        snapshot.value = value;
        out.push_back(std::move(snapshot));
    }
    for (const auto& [name, histogram] : histograms_) {
        MetricSnapshot snapshot;
        snapshot.name = name;
        snapshot.kind = MetricKind::Histogram;
        snapshot.value = histogram.mean_micros();
        snapshot.p50 = histogram.p50();
        snapshot.p95 = histogram.p95();
        snapshot.p99 = histogram.p99();
        snapshot.samples = histogram.count();
        out.push_back(std::move(snapshot));
    }
    std::sort(out.begin(), out.end(),
              [](const MetricSnapshot& a, const MetricSnapshot& b) { return a.name < b.name; });
    return out;
}

std::string MetricsRegistry::to_json() const {
    std::ostringstream ss;
    ss << "{\"counters\": {";
    bool first = true;
    for (const auto& [name, value] : counters_) {
        if (!first) ss << ", ";
        first = false;
        ss << '"' << name << "\": " << value;
    }
    ss << "}, \"gauges\": {";
    first = true;
    for (const auto& [name, value] : gauges_) {
        if (!first) ss << ", ";
        first = false;
        ss << '"' << name << "\": " << num(value);
    }
    ss << "}, \"histograms\": {";
    first = true;
    for (const auto& [name, histogram] : histograms_) {
        if (!first) ss << ", ";
        first = false;
        ss << '"' << name << "\": " << histogram.to_json();
    }
    ss << "}}";
    return ss.str();
}

void MetricsRegistry::reset() noexcept {
    counters_.clear();
    gauges_.clear();
    histograms_.clear();
}

// ---------------------------------------------------------------------------
// Resources
// ---------------------------------------------------------------------------

std::string ResourceSample::to_json() const {
    std::ostringstream ss;
    ss << "{\"available\": " << (available ? "true" : "false")
       << ", \"resident_bytes\": " << resident_bytes << ", \"virtual_bytes\": " << virtual_bytes
       << ", \"cpu_seconds\": " << num(cpu_seconds) << ", \"cpu_fraction\": " << num(cpu_fraction)
       << ", \"open_fds\": " << open_file_descriptors << '}';
    return ss.str();
}

Result<ResourceSample> ProcResourceReader::read() {
    ResourceSample sample;

    // /proc/self/stat fields 23 and 24 are vsize and rss (in pages).
    std::ifstream stat{"/proc/self/stat"};
    if (!stat) {
        // An absent resource reading is NOT a reason to stop trading, so this
        // reports unavailability rather than failing.
        sample.available = false;
        return sample;
    }

    std::string field;
    std::vector<std::string> fields;
    while (stat >> field) fields.push_back(field);

    if (fields.size() >= 24) {
        try {
            const double utime = std::stod(fields[13]);
            const double stime = std::stod(fields[14]);
            // Clock ticks to seconds. 100 Hz is the near-universal Linux
            // default; a wrong divisor scales CPU but never breaks trading.
            sample.cpu_seconds = (utime + stime) / 100.0;
            sample.virtual_bytes = static_cast<std::uint64_t>(std::stoull(fields[22]));
            const auto rss_pages = std::stoull(fields[23]);
            sample.resident_bytes = static_cast<std::uint64_t>(rss_pages) * 4096ULL;
            sample.available = true;
        } catch (...) {
            sample.available = false;
        }
    }

    std::error_code ec;
    const std::filesystem::path fd_dir{"/proc/self/fd"};
    if (std::filesystem::exists(fd_dir, ec)) {
        std::size_t count = 0;
        for (const auto& entry : std::filesystem::directory_iterator{fd_dir, ec}) {
            if (ec) break;
            (void)entry;
            ++count;
        }
        sample.open_file_descriptors = count;
    }
    return sample;
}

Result<ResourceSample> ResourceMonitor::sample() {
    auto reading = reader_->read();
    if (!reading) return fail(reading.error());

    reading->ts = clock_->now();

    // CPU utilisation is a DIFFERENCE, so the first sample has none. Reporting
    // a fraction computed against process start would show a meaningless
    // lifetime average as though it were current load.
    if (have_previous_ && reading->available && last_.available) {
        const auto elapsed_ns = (reading->ts - last_.ts).count();
        if (elapsed_ns > 0) {
            const double elapsed_seconds = static_cast<double>(elapsed_ns) / 1e9;
            const double cpu_delta = reading->cpu_seconds - last_.cpu_seconds;
            const double fraction = cpu_delta / elapsed_seconds;
            reading->cpu_fraction = is_finite(fraction) ? std::max(0.0, fraction) : 0.0;
        }
    }

    last_ = *reading;
    have_previous_ = true;
    return last_;
}

void ResourceMonitor::publish(MetricsRegistry& registry) const {
    if (!last_.available) return;
    registry.set_gauge(metric_names::kCpuFraction, last_.cpu_fraction);
    registry.set_gauge(metric_names::kResidentBytes, static_cast<double>(last_.resident_bytes));
    registry.set_gauge(metric_names::kOpenFds, static_cast<double>(last_.open_file_descriptors));
}

}  // namespace ptl::ops
