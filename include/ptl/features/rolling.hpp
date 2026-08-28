#pragma once

/// \file rolling.hpp
/// Fixed-window and exponentially weighted estimators.
///
/// Every one is O(1) per update with a preallocated ring buffer -- no
/// reallocation, no recomputation of the window, no allocation after
/// construction. A naive implementation that re-sums the window on each update
/// is O(N) per bar and O(N*T) per pass, which at 9 instruments x 390 minutes x
/// 3 years is the difference between seconds and minutes for every sweep.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <numbers>
#include <string>
#include <utility>
#include <vector>

#include "ptl/core/types.hpp"

namespace ptl::features {

/// Preallocated circular buffer. The building block for every windowed
/// estimator here.
class RingBuffer {
public:
    explicit RingBuffer(std::size_t capacity) : data_(capacity, 0.0) {}

    /// Push a value; returns the evicted one (0.0 while still filling).
    double push(double x) noexcept {
        const double evicted = full_ ? data_[head_] : 0.0;
        data_[head_] = x;
        head_ = (head_ + 1) % data_.size();
        if (!full_) {
            ++size_;
            if (size_ == data_.size()) full_ = true;
        }
        return evicted;
    }

    [[nodiscard]] std::size_t size() const noexcept { return size_; }
    [[nodiscard]] std::size_t capacity() const noexcept { return data_.size(); }
    [[nodiscard]] bool full() const noexcept { return full_; }

    /// Oldest retained value.
    [[nodiscard]] double oldest() const noexcept {
        if (size_ == 0) return 0.0;
        return full_ ? data_[head_] : data_[0];
    }
    /// Most recently pushed value.
    [[nodiscard]] double newest() const noexcept {
        if (size_ == 0) return 0.0;
        return data_[(head_ + data_.size() - 1) % data_.size()];
    }
    /// k steps back; k == 0 is newest.
    [[nodiscard]] double at_lag(std::size_t k) const noexcept {
        if (k >= size_) return 0.0;
        return data_[(head_ + data_.size() - 1 - k) % data_.size()];
    }

    void reset() noexcept {
        std::fill(data_.begin(), data_.end(), 0.0);
        head_ = 0;
        size_ = 0;
        full_ = false;
    }

private:
    std::vector<double> data_;
    std::size_t head_ = 0;
    std::size_t size_ = 0;
    bool full_ = false;
};

/// Rolling arithmetic mean.
///
/// Maintains a running sum with a periodic exact recomputation. Pure
/// add-and-subtract accumulates floating-point drift without bound over
/// millions of updates; recomputing every `capacity` pushes caps the error at
/// one window's worth while keeping the amortised cost O(1).
class RollingMean {
public:
    explicit RollingMean(std::size_t window)
        : buf_(std::max<std::size_t>(1, window)), window_(std::max<std::size_t>(1, window)) {}

    void update(double x) noexcept {
        const double evicted = buf_.push(x);
        sum_ += x - evicted;
        if (++since_recompute_ >= window_) recompute();
    }

    [[nodiscard]] double value() const noexcept {
        if (buf_.size() == 0) return 0.0;
        return sum_ / static_cast<double>(buf_.size());
    }
    [[nodiscard]] bool ready() const noexcept { return buf_.full(); }
    [[nodiscard]] std::size_t warmup() const noexcept { return window_; }
    [[nodiscard]] const RingBuffer& buffer() const noexcept { return buf_; }

    void reset() noexcept {
        buf_.reset();
        sum_ = 0.0;
        since_recompute_ = 0;
    }

private:
    void recompute() noexcept {
        double s = 0.0;
        for (std::size_t i = 0; i < buf_.size(); ++i) s += buf_.at_lag(i);
        sum_ = s;
        since_recompute_ = 0;
    }

    RingBuffer buf_;
    std::size_t window_;
    double sum_ = 0.0;
    std::size_t since_recompute_ = 0;
};

/// Rolling sample standard deviation over a fixed window.
///
/// O(1) per update via SHIFTED-DATA accumulation, with a periodic exact
/// recompute. Two problems are being solved at once:
///
///  - The naive running form E[x^2] - E[x]^2 cancels catastrophically when the
///    mean is large relative to the spread -- exactly minute prices around 500
///    with a spread of 0.05 -- and can return a NEGATIVE variance.
///  - Recomputing the whole window on every query is O(N) and, at a 390-bar
///    window across millions of bars, dominates the entire feature engine.
///
/// The fix is to accumulate (x - K) for a reference K close to the data. The
/// shifted values are small, so cancellation is negligible, while the sums
/// remain incremental. K is re-centred on the current mean at each exact
/// recompute, which bounds drift as the series trends away from its origin.
class RollingStdev {
public:
    explicit RollingStdev(std::size_t window)
        : buf_(std::max<std::size_t>(2, window)), window_(std::max<std::size_t>(2, window)) {}

    void update(double x) noexcept {
        if (!have_ref_) {
            ref_ = x;
            have_ref_ = true;
        }
        const double shifted = x - ref_;
        const double evicted = buf_.push(x);
        // Subtract the evicted value only once the ring has actually begun
        // evicting; while filling it returns 0.0 as a placeholder, and
        // subtracting that would corrupt the sums.
        if (evicting_) {
            const double evicted_shift = evicted - ref_;
            sum_ -= evicted_shift;
            sumsq_ -= evicted_shift * evicted_shift;
        }
        sum_ += shifted;
        sumsq_ += shifted * shifted;
        if (buf_.full()) evicting_ = true;

        if (++since_recompute_ >= window_) recompute();
    }

    [[nodiscard]] double value() const noexcept { return std::sqrt(variance()); }

    [[nodiscard]] double variance() const noexcept {
        const std::size_t n = buf_.size();
        if (n < 2) return 0.0;
        const double nd = static_cast<double>(n);
        // max(0, .) because the shifted form can still land a few ulps below
        // zero for a constant window; a negative variance would produce NaN in
        // sqrt and poison every downstream aggregate.
        const double m2 = std::max(0.0, sumsq_ - sum_ * sum_ / nd);
        return m2 / (nd - 1.0);
    }

    [[nodiscard]] double mean() const noexcept {
        const std::size_t n = buf_.size();
        if (n == 0) return 0.0;
        return ref_ + sum_ / static_cast<double>(n);
    }
    [[nodiscard]] bool ready() const noexcept { return buf_.full(); }
    [[nodiscard]] std::size_t warmup() const noexcept { return window_; }

    void reset() noexcept {
        buf_.reset();
        sum_ = 0.0;
        sumsq_ = 0.0;
        ref_ = 0.0;
        have_ref_ = false;
        evicting_ = false;
        since_recompute_ = 0;
    }

private:
    /// Exact two-pass recompute, re-centring the reference on the current mean.
    /// Bounds both accumulated drift and the distance of the data from K.
    void recompute() noexcept {
        const std::size_t n = buf_.size();
        since_recompute_ = 0;
        if (n == 0) return;
        double m = 0.0;
        for (std::size_t i = 0; i < n; ++i) m += buf_.at_lag(i);
        m /= static_cast<double>(n);

        ref_ = m;
        sum_ = 0.0;
        sumsq_ = 0.0;
        for (std::size_t i = 0; i < n; ++i) {
            const double d = buf_.at_lag(i) - ref_;
            sum_ += d;
            sumsq_ += d * d;
        }
    }

    RingBuffer buf_;
    std::size_t window_;
    double sum_ = 0.0;
    double sumsq_ = 0.0;
    double ref_ = 0.0;
    bool have_ref_ = false;
    bool evicting_ = false;
    std::size_t since_recompute_ = 0;
};

/// Welford's online variance over ALL observations seen.
///
/// Unlike RollingStdev this never forgets, which is what makes it the right
/// choice for a session-cumulative or full-sample statistic. Numerically stable
/// by construction.
class WelfordVariance {
public:
    void update(double x) noexcept {
        ++n_;
        const double delta = x - mean_;
        mean_ += delta / static_cast<double>(n_);
        m2_ += delta * (x - mean_);
    }

    [[nodiscard]] double mean() const noexcept { return mean_; }
    [[nodiscard]] double variance() const noexcept {
        return n_ < 2 ? 0.0 : m2_ / static_cast<double>(n_ - 1);
    }
    [[nodiscard]] double stdev() const noexcept { return std::sqrt(variance()); }
    [[nodiscard]] std::size_t count() const noexcept { return n_; }
    [[nodiscard]] bool ready() const noexcept { return n_ >= 2; }

    void reset() noexcept {
        n_ = 0;
        mean_ = 0.0;
        m2_ = 0.0;
    }

private:
    std::size_t n_ = 0;
    double mean_ = 0.0;
    double m2_ = 0.0;
};

/// Exponentially weighted mean.
///
/// `halflife` is expressed in observations, not as a raw alpha: a half-life is
/// interpretable and comparable across sampling frequencies, whereas an alpha
/// silently means something different at one-minute versus daily bars.
class Ewma {
public:
    explicit Ewma(double halflife, std::size_t warmup = 0)
        : alpha_(halflife > 0.0 ? 1.0 - std::exp(-std::numbers::ln2 / halflife) : 1.0),
          warmup_(warmup > 0 ? warmup : static_cast<std::size_t>(std::ceil(halflife * 3.0))) {}

    void update(double x) noexcept {
        if (n_ == 0) {
            value_ = x;
        } else {
            value_ += alpha_ * (x - value_);
        }
        ++n_;
    }

    [[nodiscard]] double value() const noexcept { return value_; }
    [[nodiscard]] double alpha() const noexcept { return alpha_; }
    [[nodiscard]] bool ready() const noexcept { return n_ >= warmup_; }
    [[nodiscard]] std::size_t warmup() const noexcept { return warmup_; }

    void reset() noexcept {
        n_ = 0;
        value_ = 0.0;
    }

private:
    double alpha_;
    std::size_t warmup_;
    std::size_t n_ = 0;
    double value_ = 0.0;
};

/// Exponentially weighted variance, tracking the EW mean simultaneously.
class EwVariance {
public:
    explicit EwVariance(double halflife, std::size_t warmup = 0)
        : alpha_(halflife > 0.0 ? 1.0 - std::exp(-std::numbers::ln2 / halflife) : 1.0),
          warmup_(warmup > 0 ? warmup : static_cast<std::size_t>(std::ceil(halflife * 3.0))) {}

    void update(double x) noexcept {
        if (n_ == 0) {
            mean_ = x;
        } else {
            const double delta = x - mean_;
            mean_ += alpha_ * delta;
            // West's incremental EW variance: uses the PRE-update delta and the
            // post-update mean, which keeps the estimate non-negative.
            var_ = (1.0 - alpha_) * (var_ + alpha_ * delta * delta);
        }
        ++n_;
    }

    [[nodiscard]] double mean() const noexcept { return mean_; }
    [[nodiscard]] double variance() const noexcept { return var_; }
    [[nodiscard]] double stdev() const noexcept { return std::sqrt(std::max(0.0, var_)); }
    [[nodiscard]] bool ready() const noexcept { return n_ >= warmup_; }
    [[nodiscard]] std::size_t warmup() const noexcept { return warmup_; }

    void reset() noexcept {
        n_ = 0;
        mean_ = 0.0;
        var_ = 0.0;
    }

private:
    double alpha_;
    std::size_t warmup_;
    std::size_t n_ = 0;
    double mean_ = 0.0;
    double var_ = 0.0;
};

/// Rolling minimum and maximum via monotonic deques: O(1) amortised, not
/// O(window) per query.
class RollingExtrema {
public:
    explicit RollingExtrema(std::size_t window) : window_(std::max<std::size_t>(1, window)) {
        min_.reserve(window_);
        max_.reserve(window_);
    }

    void update(double x) noexcept {
        ++index_;
        while (!min_.empty() && min_.back().second >= x) min_.pop_back();
        min_.emplace_back(index_, x);
        while (!max_.empty() && max_.back().second <= x) max_.pop_back();
        max_.emplace_back(index_, x);

        const std::size_t cutoff = index_ >= window_ ? index_ - window_ : 0;
        while (!min_.empty() && min_.front().first <= cutoff) min_.erase(min_.begin());
        while (!max_.empty() && max_.front().first <= cutoff) max_.erase(max_.begin());
        if (count_ < window_) ++count_;
    }

    [[nodiscard]] double min() const noexcept { return min_.empty() ? 0.0 : min_.front().second; }
    [[nodiscard]] double max() const noexcept { return max_.empty() ? 0.0 : max_.front().second; }
    [[nodiscard]] bool ready() const noexcept { return count_ >= window_; }
    [[nodiscard]] std::size_t warmup() const noexcept { return window_; }

    void reset() noexcept {
        min_.clear();
        max_.clear();
        index_ = 0;
        count_ = 0;
    }

private:
    std::size_t window_;
    std::size_t index_ = 0;
    std::size_t count_ = 0;
    std::vector<std::pair<std::size_t, double>> min_;
    std::vector<std::pair<std::size_t, double>> max_;
};

}  // namespace ptl::features
