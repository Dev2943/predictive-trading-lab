#pragma once

/// \file momentum.hpp
/// Momentum, reversal, trend-state and volatility estimators.
///
/// The initial signal library from the research, deliberately unglamorous:
/// lagged log returns, short-term reversal, moving-average deviation and
/// realised volatility. Every one is trailing-only by construction.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <initializer_list>

#include "ptl/core/types.hpp"
#include "ptl/features/rolling.hpp"

namespace ptl::features {

/// Log return over `lag` observations: log(P_t / P_{t-lag}).
///
/// LOG, not simple. Log returns are additive across time, which makes a
/// multi-period return the sum of its parts and keeps the annualisation
/// arithmetic exact. They are also symmetric: a +50% then -33% round trip sums
/// to zero, whereas simple returns do not.
class LaggedReturn {
public:
    explicit LaggedReturn(std::size_t lag)
        : buf_(std::max<std::size_t>(1, lag) + 1), lag_(std::max<std::size_t>(1, lag)) {}

    void update(double price) noexcept {
        if (price > 0.0 && is_finite(price)) (void)buf_.push(price);
    }

    [[nodiscard]] double value() const noexcept {
        if (buf_.size() <= lag_) return 0.0;
        const double now = buf_.at_lag(0);
        const double then = buf_.at_lag(lag_);
        if (now <= 0.0 || then <= 0.0) return 0.0;
        const double r = std::log(now / then);
        return is_finite(r) ? r : 0.0;
    }
    [[nodiscard]] bool ready() const noexcept { return buf_.size() > lag_; }
    [[nodiscard]] std::size_t warmup() const noexcept { return lag_ + 1; }
    [[nodiscard]] std::size_t lag() const noexcept { return lag_; }
    void reset() noexcept { buf_.reset(); }

private:
    RingBuffer buf_;
    std::size_t lag_;
};

/// Short-term reversal: the negation of a recent return.
///
/// A separate type rather than a sign flip at the call site, because the SIGN
/// CONVENTION is the whole content of the signal. Reversal predicts that recent
/// losers bounce; writing it as `-r` inline invites someone to "simplify" it
/// back into momentum during a refactor.
class ShortTermReversal {
public:
    explicit ShortTermReversal(std::size_t lag) : ret_(lag) {}

    void update(double price) noexcept { ret_.update(price); }

    [[nodiscard]] double value() const noexcept { return -ret_.value(); }
    [[nodiscard]] bool ready() const noexcept { return ret_.ready(); }
    [[nodiscard]] std::size_t warmup() const noexcept { return ret_.warmup(); }
    void reset() noexcept { ret_.reset(); }

private:
    LaggedReturn ret_;
};

/// Rolling z-score of a value against its own trailing window.
class RollingZScore {
public:
    explicit RollingZScore(std::size_t window) : sd_(window) {}

    void update(double x) noexcept {
        if (is_finite(x)) {
            sd_.update(x);
            last_ = x;
        }
    }

    [[nodiscard]] double value() const noexcept {
        const double s = sd_.value();
        // A constant window has zero dispersion; the z-score is undefined and
        // zero is the honest answer. Dividing would produce inf.
        if (s <= 0.0) return 0.0;
        const double z = (last_ - sd_.mean()) / s;
        return is_finite(z) ? z : 0.0;
    }
    [[nodiscard]] bool ready() const noexcept { return sd_.ready(); }
    [[nodiscard]] std::size_t warmup() const noexcept { return sd_.warmup(); }
    void reset() noexcept {
        sd_.reset();
        last_ = 0.0;
    }

private:
    RollingStdev sd_;
    double last_ = 0.0;
};

/// Relative deviation from a simple moving average: (P - SMA) / SMA.
///
/// A trend-state descriptor, not an alpha in itself. Relative rather than
/// absolute so it is comparable across instruments trading at 40 and at 550.
class MaDeviation {
public:
    explicit MaDeviation(std::size_t window) : ma_(window) {}

    void update(double price) noexcept {
        if (price > 0.0 && is_finite(price)) {
            ma_.update(price);
            last_ = price;
        }
    }

    [[nodiscard]] double value() const noexcept {
        const double m = ma_.value();
        if (m <= 0.0) return 0.0;
        const double d = (last_ - m) / m;
        return is_finite(d) ? d : 0.0;
    }
    [[nodiscard]] bool ready() const noexcept { return ma_.ready(); }
    [[nodiscard]] std::size_t warmup() const noexcept { return ma_.warmup(); }
    void reset() noexcept {
        ma_.reset();
        last_ = 0.0;
    }

private:
    RollingMean ma_;
    double last_ = 0.0;
};

/// Realised volatility: sqrt(sum r^2 / n), optionally annualised.
///
/// Sum of SQUARED returns without demeaning, which is the standard realised
/// variance estimator. Demeaning a high-frequency return series subtracts a
/// quantity that is statistically indistinguishable from zero while adding
/// estimation noise.
class RealizedVolatility {
public:
    /// \param annualization_periods periods per year; 1 leaves it per-period.
    RealizedVolatility(std::size_t window, double annualization_periods = 1.0)
        : sq_(std::max<std::size_t>(1, window)),
          window_(std::max<std::size_t>(1, window)),
          scale_(std::sqrt(std::max(1.0, annualization_periods))) {}

    /// \param log_return one period's log return
    void update(double log_return) noexcept {
        if (!is_finite(log_return)) return;
        sum_ += log_return * log_return - sq_.push(log_return * log_return);
        ++n_;
    }

    [[nodiscard]] double value() const noexcept {
        if (sq_.size() == 0) return 0.0;
        const double mean_sq = std::max(0.0, sum_) / static_cast<double>(sq_.size());
        const double v = std::sqrt(mean_sq) * scale_;
        return is_finite(v) ? v : 0.0;
    }
    [[nodiscard]] bool ready() const noexcept { return sq_.full(); }
    [[nodiscard]] std::size_t warmup() const noexcept { return window_; }
    void reset() noexcept {
        sq_.reset();
        sum_ = 0.0;
        n_ = 0;
    }

private:
    RingBuffer sq_;
    std::size_t window_;
    double scale_;
    double sum_ = 0.0;
    std::size_t n_ = 0;
};

/// Average true range, Wilder-smoothed.
///
/// Wilder smoothing (alpha = 1/N), NOT a simple moving average. The distinction
/// is not cosmetic: every published ATR level and every comparison against one
/// assumes Wilder, so an SMA-based ATR is a different indicator wearing the
/// same name.
class AverageTrueRange {
public:
    explicit AverageTrueRange(std::size_t period)
        : period_(std::max<std::size_t>(1, period)),
          alpha_(1.0 / static_cast<double>(std::max<std::size_t>(1, period))) {}

    void update(double high, double low, double close) noexcept {
        double tr = high - low;
        if (n_ > 0) {
            // True range includes the gap from the previous close, which is
            // what makes it a volatility measure rather than a range measure.
            tr = std::max({tr, std::abs(high - prev_close_), std::abs(low - prev_close_)});
        }
        if (!is_finite(tr)) return;
        atr_ = n_ == 0 ? tr : atr_ + alpha_ * (tr - atr_);
        prev_close_ = close;
        ++n_;
    }

    [[nodiscard]] double value() const noexcept { return atr_; }
    [[nodiscard]] bool ready() const noexcept { return n_ >= period_; }
    [[nodiscard]] std::size_t warmup() const noexcept { return period_; }
    void reset() noexcept {
        n_ = 0;
        atr_ = 0.0;
        prev_close_ = 0.0;
    }

private:
    std::size_t period_;
    double alpha_;
    std::size_t n_ = 0;
    double atr_ = 0.0;
    double prev_close_ = 0.0;
};

}  // namespace ptl::features
