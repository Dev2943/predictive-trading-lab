#pragma once

/// \file intraday.hpp
/// Session-aware and intraday estimators.
///
/// These are the features that a naive daily-bar implementation gets wrong. The
/// intraday volume U-curve in particular means a plain trailing volume z-score
/// mostly encodes "it is near the open" rather than genuine volume surprise
/// (reconciliation row F3), so relative volume is measured against a
/// same-minute-of-day baseline.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <numbers>
#include <vector>

#include "ptl/core/types.hpp"
#include "ptl/features/rolling.hpp"
#include "ptl/market/calendar.hpp"

namespace ptl::features {

/// Minute index within a session, derived from the session open.
///
/// Session-relative rather than clock-relative: a half day and a regular day
/// both start at minute 0, so a minute-of-day statistic remains aligned on the
/// ~6 sessions a year that close early (ADR-0001 Addendum A2).
[[nodiscard]] std::int32_t minute_of_session(Timestamp ts, const market::Session& session) noexcept;

/// Rolling per-minute volume baseline.
///
/// Holds one RollingMean per minute slot, each updated only by bars landing in
/// that slot. Relative volume is then current / baseline for the SAME minute --
/// so 09:31 is compared with previous 09:31s, not with 14:00.
///
/// Memory is one mean per slot, allocated once; there is no per-bar allocation.
class MinuteOfDayVolumeProfile {
public:
    /// \param slots   minutes per session (390 regular, 210 half day)
    /// \param lookback sessions of history per slot
    MinuteOfDayVolumeProfile(std::size_t slots = 390, std::size_t lookback = 20);

    void update(std::int32_t minute, double volume) noexcept;

    /// Baseline volume for `minute`, or 0 when that slot has no history.
    [[nodiscard]] double baseline(std::int32_t minute) const noexcept;

    /// current / baseline. Returns 1.0 (neutral) rather than infinity when the
    /// baseline is zero: a zero-volume slot is uninformative, not infinitely
    /// surprising, and an inf here would poison every downstream aggregate.
    [[nodiscard]] double relative_volume(std::int32_t minute, double volume) const noexcept;

    [[nodiscard]] bool ready(std::int32_t minute) const noexcept;
    [[nodiscard]] std::size_t slots() const noexcept { return means_.size(); }
    void reset() noexcept;

private:
    std::vector<RollingMean> means_;
    std::size_t lookback_;
};

/// Volume-weighted average price accumulated over a window.
///
/// Zero total volume yields the last price rather than a division by zero:
/// zero-volume minutes are real for XLE and TLT, and VWAP over nothing is
/// simply the prevailing price.
class RollingVwap {
public:
    explicit RollingVwap(std::size_t window)
        : pv_(std::max<std::size_t>(1, window)),
          vol_(std::max<std::size_t>(1, window)),
          window_(std::max<std::size_t>(1, window)) {}

    void update(double price, double volume) noexcept {
        last_price_ = price;
        pv_sum_ += price * volume - pv_.push(price * volume);
        vol_sum_ += volume - vol_.push(volume);
    }

    [[nodiscard]] double value() const noexcept {
        if (vol_sum_ <= 0.0) return last_price_;
        const double v = pv_sum_ / vol_sum_;
        return is_finite(v) ? v : last_price_;
    }
    [[nodiscard]] double window_volume() const noexcept { return vol_sum_; }
    [[nodiscard]] bool ready() const noexcept { return pv_.full(); }
    [[nodiscard]] std::size_t warmup() const noexcept { return window_; }

    void reset() noexcept {
        pv_.reset();
        vol_.reset();
        pv_sum_ = 0.0;
        vol_sum_ = 0.0;
        last_price_ = 0.0;
    }

private:
    RingBuffer pv_;
    RingBuffer vol_;
    std::size_t window_;
    double pv_sum_ = 0.0;
    double vol_sum_ = 0.0;
    double last_price_ = 0.0;
};

/// Session-cumulative VWAP, reset at each session open.
///
/// Distinct from RollingVwap: the trading benchmark is the whole session, not a
/// trailing window, and resetting is what makes it comparable to the venue's
/// own published figure.
class SessionVwap {
public:
    void update(double price, double volume) noexcept {
        last_price_ = price;
        pv_ += price * volume;
        vol_ += volume;
        ++n_;
    }
    void on_session_open() noexcept { reset(); }

    [[nodiscard]] double value() const noexcept {
        if (vol_ <= 0.0) return last_price_;
        const double v = pv_ / vol_;
        return is_finite(v) ? v : last_price_;
    }
    [[nodiscard]] double session_volume() const noexcept { return vol_; }
    [[nodiscard]] bool ready() const noexcept { return n_ > 0; }

    void reset() noexcept {
        pv_ = 0.0;
        vol_ = 0.0;
        n_ = 0;
        last_price_ = 0.0;
    }

private:
    double pv_ = 0.0;
    double vol_ = 0.0;
    std::size_t n_ = 0;
    double last_price_ = 0.0;
};

/// Time-weighted average price: the unweighted mean of interval prices.
///
/// Deliberately separate from VWAP rather than "VWAP with unit weights". The
/// two answer different questions -- TWAP is the schedule benchmark, VWAP the
/// liquidity benchmark -- and their difference is itself a signal about where
/// volume clustered in the interval.
class RollingTwap {
public:
    explicit RollingTwap(std::size_t window) : mean_(window) {}

    void update(double price) noexcept { mean_.update(price); }

    [[nodiscard]] double value() const noexcept { return mean_.value(); }
    [[nodiscard]] bool ready() const noexcept { return mean_.ready(); }
    [[nodiscard]] std::size_t warmup() const noexcept { return mean_.warmup(); }
    void reset() noexcept { mean_.reset(); }

private:
    RollingMean mean_;
};

/// Quoted spread statistics in basis points.
class SpreadStatistics {
public:
    explicit SpreadStatistics(std::size_t window) : mean_(window), sd_(window) {}

    void update(double bid, double ask) noexcept {
        const double mid = (bid + ask) * 0.5;
        if (mid <= 0.0 || !is_finite(mid)) return;
        last_bps_ = (ask - bid) / mid * 1e4;
        mean_.update(last_bps_);
        sd_.update(last_bps_);
    }

    [[nodiscard]] double current_bps() const noexcept { return last_bps_; }
    [[nodiscard]] double mean_bps() const noexcept { return mean_.value(); }
    /// How unusual the current spread is. Zero dispersion yields zero rather
    /// than infinity.
    [[nodiscard]] double zscore() const noexcept {
        const double s = sd_.value();
        if (s <= 0.0) return 0.0;
        const double z = (last_bps_ - mean_.value()) / s;
        return is_finite(z) ? z : 0.0;
    }
    [[nodiscard]] bool ready() const noexcept { return mean_.ready(); }
    [[nodiscard]] std::size_t warmup() const noexcept { return mean_.warmup(); }

    void reset() noexcept {
        mean_.reset();
        sd_.reset();
        last_bps_ = 0.0;
    }

private:
    RollingMean mean_;
    RollingStdev sd_;
    double last_bps_ = 0.0;
};

/// Amihud-style illiquidity: |return| per unit of dollar volume, scaled.
///
/// A capacity proxy. Higher means price moves more for the same traded value,
/// which is what makes a name expensive to trade in size.
class AmihudIlliquidity {
public:
    explicit AmihudIlliquidity(std::size_t window) : mean_(window) {}

    void update(double abs_return, double dollar_volume) noexcept {
        if (dollar_volume <= 0.0 || !is_finite(dollar_volume)) return;
        const double ratio = abs_return / dollar_volume * 1e6;
        if (is_finite(ratio)) mean_.update(ratio);
    }

    [[nodiscard]] double value() const noexcept { return mean_.value(); }
    [[nodiscard]] bool ready() const noexcept { return mean_.ready(); }
    [[nodiscard]] std::size_t warmup() const noexcept { return mean_.warmup(); }
    void reset() noexcept { mean_.reset(); }

private:
    RollingMean mean_;
};

/// Discretises realised volatility into regimes.
///
/// Regime conditioning is half of the signal-decay story: it distinguishes a
/// model that stopped working from one merely operating in an unfamiliar
/// regime, which the research names as a required distinction.
class VolatilityBucket {
public:
    enum class Regime : std::uint8_t { Low, Normal, High, Extreme };

    explicit VolatilityBucket(std::size_t window) : sd_(window) {}

    void update(double realized_vol) noexcept {
        sd_.update(realized_vol);
        last_ = realized_vol;
    }

    [[nodiscard]] Regime regime() const noexcept {
        const double s = sd_.value();
        const double m = sd_.mean();
        if (s <= 0.0) return Regime::Normal;
        const double z = (last_ - m) / s;
        if (z < -1.0) return Regime::Low;
        if (z < 1.0) return Regime::Normal;
        if (z < 2.0) return Regime::High;
        return Regime::Extreme;
    }
    [[nodiscard]] double value() const noexcept {
        return static_cast<double>(static_cast<std::uint8_t>(regime()));
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

/// Intraday seasonal controls: sine and cosine of session progress.
///
/// A model given raw minute-of-day would have to learn the U-shape from
/// scratch and would extrapolate nonsensically past the session edge. A
/// sin/cos pair is smooth, bounded and periodic, so the model spends its
/// capacity on the signal rather than on rediscovering the clock.
struct SeasonalControls {
    double sin_component = 0.0;
    double cos_component = 0.0;

    /// \param progress fraction of the session elapsed, in [0, 1]
    void update(double progress) noexcept {
        const double theta = 2.0 * std::numbers::pi * std::clamp(progress, 0.0, 1.0);
        sin_component = std::sin(theta);
        cos_component = std::cos(theta);
    }
};

}  // namespace ptl::features
