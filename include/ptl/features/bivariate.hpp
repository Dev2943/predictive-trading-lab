#pragma once

/// \file bivariate.hpp
/// Rolling covariance, correlation and beta.
///
/// These take TWO series -- typically an instrument and its market proxy -- and
/// are the machinery behind market-relative features. They live here rather than
/// inside the cross-sectional stage because they are ordinary streaming
/// estimators: they see one paired observation at a time and never look back
/// beyond their window.

#include <algorithm>
#include <cmath>
#include <cstddef>

#include "ptl/core/types.hpp"
#include "ptl/features/rolling.hpp"

namespace ptl::features {

/// Rolling sample covariance over a fixed window.
///
/// Recomputed from the retained window using two-pass means, for the same
/// reason RollingStdev is: the E[xy]-E[x]E[y] shortcut cancels catastrophically
/// when means are large relative to spreads, and can return a covariance whose
/// implied correlation exceeds 1.
class RollingCovariance {
public:
    explicit RollingCovariance(std::size_t window)
        : xs_(std::max<std::size_t>(2, window)),
          ys_(std::max<std::size_t>(2, window)),
          window_(std::max<std::size_t>(2, window)) {}

    void update(double x, double y) noexcept {
        if (!have_ref_) {
            ref_x_ = x;
            ref_y_ = y;
            have_ref_ = true;
        }
        const double dx = x - ref_x_;
        const double dy = y - ref_y_;
        const double ex = xs_.push(x);
        const double ey = ys_.push(y);

        // Subtract evicted values only once the ring has begun evicting; while
        // filling it returns 0.0 as a placeholder.
        if (evicting_) {
            const double edx = ex - ref_x_;
            const double edy = ey - ref_y_;
            sx_ -= edx;
            sy_ -= edy;
            sxx_ -= edx * edx;
            syy_ -= edy * edy;
            sxy_ -= edx * edy;
        }
        sx_ += dx;
        sy_ += dy;
        sxx_ += dx * dx;
        syy_ += dy * dy;
        sxy_ += dx * dy;
        if (xs_.full()) evicting_ = true;

        if (++since_recompute_ >= window_) recompute();
    }

    [[nodiscard]] double value() const noexcept {
        const std::size_t n = xs_.size();
        if (n < 2) return 0.0;
        const double nd = static_cast<double>(n);
        // Shifted-data form: the reference cancels exactly, so this is the true
        // covariance, computed on small numbers where cancellation is
        // negligible.
        const double c = sxy_ - sx_ * sy_ / nd;
        return c / (nd - 1.0);
    }

    [[nodiscard]] double variance_x() const noexcept { return var_from(sxx_, sx_); }
    [[nodiscard]] double variance_y() const noexcept { return var_from(syy_, sy_); }
    [[nodiscard]] bool ready() const noexcept { return xs_.full(); }
    [[nodiscard]] std::size_t warmup() const noexcept { return window_; }

    void reset() noexcept {
        xs_.reset();
        ys_.reset();
        sx_ = sy_ = sxx_ = syy_ = sxy_ = 0.0;
        ref_x_ = ref_y_ = 0.0;
        have_ref_ = false;
        evicting_ = false;
        since_recompute_ = 0;
    }

private:
    [[nodiscard]] double var_from(double sumsq, double sum) const noexcept {
        const std::size_t n = xs_.size();
        if (n < 2) return 0.0;
        const double nd = static_cast<double>(n);
        // Clamped at zero: the shifted form can land a few ulps negative for a
        // constant window, and a negative variance would produce NaN in sqrt.
        const double m2 = std::max(0.0, sumsq - sum * sum / nd);
        return m2 / (nd - 1.0);
    }

    /// Exact recompute, re-centring both references on their current means.
    void recompute() noexcept {
        const std::size_t n = xs_.size();
        since_recompute_ = 0;
        if (n == 0) return;
        double mx = 0.0;
        double my = 0.0;
        for (std::size_t i = 0; i < n; ++i) {
            mx += xs_.at_lag(i);
            my += ys_.at_lag(i);
        }
        mx /= static_cast<double>(n);
        my /= static_cast<double>(n);

        ref_x_ = mx;
        ref_y_ = my;
        sx_ = sy_ = sxx_ = syy_ = sxy_ = 0.0;
        for (std::size_t i = 0; i < n; ++i) {
            const double dx = xs_.at_lag(i) - ref_x_;
            const double dy = ys_.at_lag(i) - ref_y_;
            sx_ += dx;
            sy_ += dy;
            sxx_ += dx * dx;
            syy_ += dy * dy;
            sxy_ += dx * dy;
        }
    }

    RingBuffer xs_;
    RingBuffer ys_;
    std::size_t window_;
    double sx_ = 0.0;
    double sy_ = 0.0;
    double sxx_ = 0.0;
    double syy_ = 0.0;
    double sxy_ = 0.0;
    double ref_x_ = 0.0;
    double ref_y_ = 0.0;
    bool have_ref_ = false;
    bool evicting_ = false;
    std::size_t since_recompute_ = 0;
};

/// Pearson correlation. Clamped to [-1, 1]: floating-point error can push a
/// legitimate result a few ulps outside the range, and a correlation of
/// 1.0000000002 flowing into a downstream sqrt(1 - rho^2) produces a NaN far
/// from its cause.
class RollingCorrelation {
public:
    explicit RollingCorrelation(std::size_t window) : cov_(window) {}

    void update(double x, double y) noexcept { cov_.update(x, y); }

    [[nodiscard]] double value() const noexcept {
        const double vx = cov_.variance_x();
        const double vy = cov_.variance_y();
        // A constant series has zero variance and undefined correlation.
        // Returning zero is the honest answer; dividing would produce inf.
        if (vx <= 0.0 || vy <= 0.0) return 0.0;
        const double r = cov_.value() / std::sqrt(vx * vy);
        if (!is_finite(r)) return 0.0;
        return std::clamp(r, -1.0, 1.0);
    }
    [[nodiscard]] bool ready() const noexcept { return cov_.ready(); }
    [[nodiscard]] std::size_t warmup() const noexcept { return cov_.warmup(); }
    void reset() noexcept { cov_.reset(); }

private:
    RollingCovariance cov_;
};

/// Rolling OLS beta of `y` on `x`: cov(y,x) / var(x).
///
/// The convention matters and is easy to invert. Call update(market, asset):
/// x is the EXPLANATORY series (the market), y is the dependent one (the
/// asset). Beta measures how the asset moves per unit of market move.
class RollingBeta {
public:
    explicit RollingBeta(std::size_t window) : cov_(window) {}

    /// \param market the explanatory return
    /// \param asset  the dependent return
    void update(double market, double asset) noexcept { cov_.update(market, asset); }

    [[nodiscard]] double value() const noexcept {
        const double vm = cov_.variance_x();
        // A market with no variance over the window explains nothing. Beta is
        // undefined; zero is the honest answer and keeps the value finite.
        if (vm <= 0.0) return 0.0;
        const double b = cov_.value() / vm;
        return is_finite(b) ? b : 0.0;
    }

    /// Residual (idiosyncratic) return: asset - beta * market. This is the
    /// market-relative return the research asks for, computed with a beta the
    /// estimator has actually measured rather than an assumed 1.0.
    [[nodiscard]] double residual(double market, double asset) const noexcept {
        return asset - value() * market;
    }

    [[nodiscard]] bool ready() const noexcept { return cov_.ready(); }
    [[nodiscard]] std::size_t warmup() const noexcept { return cov_.warmup(); }
    void reset() noexcept { cov_.reset(); }

private:
    RollingCovariance cov_;
};

}  // namespace ptl::features
