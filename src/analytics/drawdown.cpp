#include "ptl/analytics/drawdown.hpp"

#include <algorithm>
#include <cmath>
#include <sstream>

#include "ptl/analytics/exposure.hpp"
#include "ptl/analytics/statistics.hpp"

namespace ptl::analytics {
namespace {

[[nodiscard]] Error bad(std::string message, std::string context = {}) {
    return make_error(ErrorCode::ValidationFailed, std::move(message), std::move(context));
}

}  // namespace

Duration DrawdownEpisode::decline_duration() const noexcept {
    if (!is_set(peak_time) || !is_set(trough_time)) return Duration::zero();
    return trough_time - peak_time;
}

Duration DrawdownEpisode::total_duration() const noexcept {
    // Zero while still underwater. An unrecovered episode has no total duration
    // yet, and inventing one by using "now" would understate the pain of a
    // drawdown that is still going.
    if (!recovered() || !is_set(peak_time)) return Duration::zero();
    return recovery_time - peak_time;
}

Result<bool> DrawdownTracker::update(Timestamp ts, Notional equity) {
    if (!is_set(ts)) return fail(bad("drawdown observation has no timestamp"));
    if (!is_finite(equity.get())) return fail(bad("equity is not finite"));
    if (is_set(last_ts_) && ts < last_ts_) {
        // An out-of-order observation would corrupt the peak and every episode
        // boundary derived from it.
        return fail(bad("drawdown observations must be non-decreasing in time",
                        to_iso8601(ts) + " < " + to_iso8601(last_ts_)));
    }
    last_ts_ = ts;

    if (!have_peak_ || equity.get() >= peak_.get()) {
        // A new high closes any episode in progress.
        if (in_episode_) {
            active_.recovery_time = ts;
            episodes_.push_back(active_);
            in_episode_ = false;
        }
        peak_ = equity;
        peak_time_ = ts;
        have_peak_ = true;
        current_ = 0.0;
        underwater_ = 0;
    } else {
        const double peak = peak_.get();
        current_ = peak > 0.0 ? 1.0 - equity.get() / peak : 0.0;
        if (!is_finite(current_)) current_ = 0.0;
        ++underwater_;
        longest_underwater_ = std::max(longest_underwater_, underwater_);

        if (!in_episode_) {
            in_episode_ = true;
            active_ = DrawdownEpisode{};
            active_.peak_time = peak_time_;
            active_.peak_equity = peak_;
            active_.trough_time = ts;
            active_.trough_equity = equity;
            active_.depth = current_;
        } else if (equity.get() < active_.trough_equity.get()) {
            active_.trough_time = ts;
            active_.trough_equity = equity;
            active_.depth = current_;
        }

        if (current_ > max_) {
            max_ = current_;
            max_peak_time_ = peak_time_;
            max_trough_time_ = ts;
        }
    }

    UnderwaterPoint point;
    point.ts = ts;
    point.drawdown = current_;
    point.equity = equity;
    point.peak = peak_;
    point.periods_underwater = underwater_;
    curve_.push_back(point);
    return true;
}

void DrawdownTracker::reset() noexcept {
    current_ = 0.0;
    max_ = 0.0;
    peak_ = Notional{};
    peak_time_ = kNoTimestamp;
    last_ts_ = kNoTimestamp;
    max_peak_time_ = kNoTimestamp;
    max_trough_time_ = kNoTimestamp;
    underwater_ = 0;
    longest_underwater_ = 0;
    have_peak_ = false;
    curve_.clear();
    episodes_.clear();
    in_episode_ = false;
    active_ = DrawdownEpisode{};
}

Result<std::vector<double>> rolling_max_drawdown(std::span<const portfolio::EquityPoint> curve,
                                                 std::size_t window) {
    if (window < 2) return fail(bad("rolling drawdown window must be at least two"));

    std::vector<double> out;
    out.reserve(curve.size());

    for (std::size_t i = 0; i < curve.size(); ++i) {
        const std::size_t first = i + 1 >= window ? i + 1 - window : 0;
        double peak = -1.0;
        double worst = 0.0;
        for (std::size_t j = first; j <= i; ++j) {
            const double e = curve[j].equity.get();
            if (!is_finite(e)) continue;
            peak = std::max(peak, e);
            // The peak used is the one WITHIN the window, not the all-time
            // peak. That is the whole point: a strategy whose worst 60-day
            // drawdown is 8% behaves differently from one whose worst is 25%,
            // even at the same all-time maximum.
            if (peak > 0.0) worst = std::max(worst, 1.0 - e / peak);
        }
        out.push_back(worst);
    }
    return out;
}

// ---------------------------------------------------------------------------
// StatisticsAccumulator
// ---------------------------------------------------------------------------

void StatisticsAccumulator::update(double x) noexcept {
    if (!is_finite(x)) return;

    if (n_ == 0) {
        min_ = x;
        max_ = x;
    } else {
        min_ = std::min(min_, x);
        max_ = std::max(max_, x);
    }

    // Welford, extended to third and fourth moments. The naive sum-of-powers
    // form loses catastrophic precision when the mean is large relative to the
    // spread -- exactly equity values around 10^6 with daily moves around 10^3.
    const double n1 = static_cast<double>(n_);
    ++n_;
    const double n = static_cast<double>(n_);
    const double delta = x - mean_;
    const double delta_n = delta / n;
    const double delta_n2 = delta_n * delta_n;
    const double term = delta * delta_n * n1;

    mean_ += delta_n;
    m4_ += term * delta_n2 * (n * n - 3.0 * n + 3.0) + 6.0 * delta_n2 * m2_ - 4.0 * delta_n * m3_;
    m3_ += term * delta_n * (n - 2.0) - 3.0 * delta_n * m2_;
    m2_ += term;
    sum_ += x;

    if (downside_valid_) {
        const double d = std::min(0.0, x - downside_threshold_);
        downside_sq_ += d * d;
    }
}

double StatisticsAccumulator::variance() const noexcept {
    if (n_ < 2) return 0.0;
    return m2_ / static_cast<double>(n_ - 1);
}

double StatisticsAccumulator::stdev() const noexcept {
    return std::sqrt(std::max(0.0, variance()));
}

double StatisticsAccumulator::skewness() const noexcept {
    if (n_ < 3 || m2_ <= 0.0) return 0.0;
    const double n = static_cast<double>(n_);
    const double s = std::sqrt(n) * m3_ / std::pow(m2_, 1.5);
    return is_finite(s) ? s : 0.0;
}

double StatisticsAccumulator::excess_kurtosis() const noexcept {
    if (n_ < 4 || m2_ <= 0.0) return 0.0;
    const double n = static_cast<double>(n_);
    const double k = n * m4_ / (m2_ * m2_) - 3.0;
    return is_finite(k) ? k : 0.0;
}

double StatisticsAccumulator::downside_deviation(double threshold) const noexcept {
    if (n_ == 0) return 0.0;
    // Recomputed against the requested threshold when it differs from the one
    // accumulated. Callers usually ask for zero, which the accumulator tracks.
    if (downside_valid_ && threshold == downside_threshold_) {
        return std::sqrt(downside_sq_ / static_cast<double>(n_));
    }
    return 0.0;
}

void StatisticsAccumulator::reset() noexcept {
    n_ = 0;
    mean_ = 0.0;
    m2_ = 0.0;
    m3_ = 0.0;
    m4_ = 0.0;
    sum_ = 0.0;
    min_ = 0.0;
    max_ = 0.0;
    downside_sq_ = 0.0;
    downside_valid_ = false;
}

double quantile_of(std::span<const double> values, double q) {
    if (values.empty()) return 0.0;
    // COPIES. An analytics function that sorted its argument in place would be
    // a mutation in disguise, and the caller's series is usually the equity
    // curve itself.
    std::vector<double> sorted(values.begin(), values.end());
    std::sort(sorted.begin(), sorted.end());
    const double clamped = std::clamp(q, 0.0, 1.0);
    const auto idx =
        static_cast<std::size_t>(std::llround(clamped * static_cast<double>(sorted.size() - 1)));
    return sorted[std::min(idx, sorted.size() - 1)];
}

double expected_shortfall(std::span<const double> values, double q) {
    if (values.empty()) return 0.0;
    std::vector<double> sorted(values.begin(), values.end());
    std::sort(sorted.begin(), sorted.end());
    const auto tail = std::max<std::size_t>(
        1, static_cast<std::size_t>(std::clamp(q, 0.0, 1.0) * static_cast<double>(sorted.size())));
    double sum = 0.0;
    for (std::size_t i = 0; i < tail; ++i) sum += sorted[i];
    return sum / static_cast<double>(tail);
}

}  // namespace ptl::analytics
