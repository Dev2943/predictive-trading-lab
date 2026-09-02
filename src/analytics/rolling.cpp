#include "ptl/analytics/rolling.hpp"

#include <algorithm>
#include <cmath>
#include <numeric>
#include <sstream>

namespace ptl::analytics {
namespace {

[[nodiscard]] Error bad(std::string message, std::string context = {}) {
    return make_error(ErrorCode::InvalidArgument, std::move(message), std::move(context));
}

/// Shared shape validation for every rolling method.
[[nodiscard]] Result<bool> check_shape(std::span<const Timestamp> ts,
                                       std::span<const double> values, std::size_t window) {
    if (window < 2) return fail(bad("rolling window must be at least two"));
    if (values.empty()) return fail(bad("cannot compute a rolling series of nothing"));
    if (ts.size() != values.size()) {
        return fail(bad("timestamp series has " + std::to_string(ts.size()) + " entries for " +
                        std::to_string(values.size()) + " values"));
    }
    for (const double v : values) {
        if (!is_finite(v)) return fail(bad("series contains a non-finite value"));
    }
    for (std::size_t i = 1; i < ts.size(); ++i) {
        if (ts[i] < ts[i - 1]) {
            return fail(bad("timestamps are not non-decreasing at index " + std::to_string(i)));
        }
    }
    return true;
}

}  // namespace

std::size_t RollingSeries::observed() const noexcept {
    return static_cast<std::size_t>(
        std::count_if(values.begin(), values.end(),
                      [](const std::optional<double>& v) { return v.has_value(); }));
}

std::optional<double> RollingSeries::latest() const noexcept {
    for (auto it = values.rbegin(); it != values.rend(); ++it) {
        if (it->has_value()) return *it;
    }
    return std::nullopt;
}

std::string RollingSeries::describe() const {
    std::ostringstream ss;
    ss.precision(6);
    ss << std::fixed << name << " (window " << window << "): " << observed() << " of " << size()
       << " observed";
    if (const auto last = latest()) ss << ", latest " << *last;
    return ss.str();
}

Result<RollingSeries> RollingAnalyzer::volatility(std::span<const Timestamp> ts,
                                                  std::span<const double> returns) const {
    if (auto ok = check_shape(ts, returns, cfg_.window); !ok) return fail(ok.error());

    RollingSeries out;
    out.name = "rolling_volatility";
    out.window = cfg_.window;
    out.timestamps.assign(ts.begin(), ts.end());
    out.values.assign(returns.size(), std::nullopt);

    // INCREMENTAL: one add and one remove per step, so the whole series is
    // O(n) rather than O(n*w). Sum and sum-of-squares are kept rather than a
    // Welford accumulator because a rolling window must also REMOVE, which
    // Welford cannot do exactly.
    double sum = 0.0;
    double sum_sq = 0.0;
    const double annualization = std::sqrt(std::max(1.0, cfg_.periods_per_year));

    for (std::size_t i = 0; i < returns.size(); ++i) {
        sum += returns[i];
        sum_sq += returns[i] * returns[i];
        if (i >= cfg_.window) {
            const double leaving = returns[i - cfg_.window];
            sum -= leaving;
            sum_sq -= leaving * leaving;
        }
        if (i + 1 < cfg_.window) continue;  // window not yet full

        const auto n = static_cast<double>(cfg_.window);
        const double mean = sum / n;
        // Clamped at zero: with a large mean relative to the spread the
        // subtraction can go slightly negative through rounding, and a
        // negative variance must never reach a sqrt.
        const double variance = std::max(0.0, (sum_sq - n * mean * mean) / (n - 1.0));
        const double vol = std::sqrt(variance) * annualization;
        if (is_finite(vol)) out.values[i] = vol;
    }
    return out;
}

Result<RollingSeries> RollingAnalyzer::sharpe(std::span<const Timestamp> ts,
                                              std::span<const double> returns) const {
    if (auto ok = check_shape(ts, returns, cfg_.window); !ok) return fail(ok.error());

    RollingSeries out;
    out.name = "rolling_sharpe";
    out.window = cfg_.window;
    out.timestamps.assign(ts.begin(), ts.end());
    out.values.assign(returns.size(), std::nullopt);

    double sum = 0.0;
    double sum_sq = 0.0;
    const double annualization = std::sqrt(std::max(1.0, cfg_.periods_per_year));
    const double rf_per_period = cfg_.risk_free_rate / std::max(1.0, cfg_.periods_per_year);

    for (std::size_t i = 0; i < returns.size(); ++i) {
        sum += returns[i];
        sum_sq += returns[i] * returns[i];
        if (i >= cfg_.window) {
            const double leaving = returns[i - cfg_.window];
            sum -= leaving;
            sum_sq -= leaving * leaving;
        }
        if (i + 1 < cfg_.window) continue;

        const auto n = static_cast<double>(cfg_.window);
        const double mean = sum / n;
        const double variance = std::max(0.0, (sum_sq - n * mean * mean) / (n - 1.0));
        const double sd = std::sqrt(variance);
        // Zero dispersion means the Sharpe is undefined, not infinite.
        if (sd > 1e-15) {
            const double sharpe = (mean - rf_per_period) / sd * annualization;
            if (is_finite(sharpe)) out.values[i] = sharpe;
        }
    }
    return out;
}

Result<RollingSeries> RollingAnalyzer::beta(std::span<const Timestamp> ts,
                                            std::span<const double> returns,
                                            std::span<const double> benchmark) const {
    if (auto ok = check_shape(ts, returns, cfg_.window); !ok) return fail(ok.error());
    if (benchmark.size() != returns.size()) {
        return fail(bad("benchmark length does not match the return series"));
    }

    RollingSeries out;
    out.name = "rolling_beta";
    out.window = cfg_.window;
    out.timestamps.assign(ts.begin(), ts.end());
    out.values.assign(returns.size(), std::nullopt);

    // Incremental co-moments: sum(x), sum(y), sum(xy), sum(y^2).
    double sx = 0.0;
    double sy = 0.0;
    double sxy = 0.0;
    double syy = 0.0;

    for (std::size_t i = 0; i < returns.size(); ++i) {
        sx += returns[i];
        sy += benchmark[i];
        sxy += returns[i] * benchmark[i];
        syy += benchmark[i] * benchmark[i];
        if (i >= cfg_.window) {
            const std::size_t j = i - cfg_.window;
            sx -= returns[j];
            sy -= benchmark[j];
            sxy -= returns[j] * benchmark[j];
            syy -= benchmark[j] * benchmark[j];
        }
        if (i + 1 < cfg_.window) continue;

        const auto n = static_cast<double>(cfg_.window);
        const double covariance = sxy - sx * sy / n;
        const double variance_b = syy - sy * sy / n;
        // A benchmark with no variance in this window explains nothing, so
        // beta is undefined rather than infinite.
        if (variance_b > 1e-18) {
            const double b = covariance / variance_b;
            if (is_finite(b)) out.values[i] = b;
        }
    }
    return out;
}

Result<RollingSeries> RollingAnalyzer::alpha(std::span<const Timestamp> ts,
                                             std::span<const double> returns,
                                             std::span<const double> benchmark) const {
    auto betas = beta(ts, returns, benchmark);
    if (!betas) return fail(betas.error());

    RollingSeries out;
    out.name = "rolling_alpha";
    out.window = cfg_.window;
    out.timestamps.assign(ts.begin(), ts.end());
    out.values.assign(returns.size(), std::nullopt);

    const double annualization = std::max(1.0, cfg_.periods_per_year);
    const double rf_per_period = cfg_.risk_free_rate / annualization;

    double sx = 0.0;
    double sy = 0.0;
    for (std::size_t i = 0; i < returns.size(); ++i) {
        sx += returns[i];
        sy += benchmark[i];
        if (i >= cfg_.window) {
            sx -= returns[i - cfg_.window];
            sy -= benchmark[i - cfg_.window];
        }
        if (i + 1 < cfg_.window) continue;
        if (!betas->values[i].has_value()) continue;

        const auto n = static_cast<double>(cfg_.window);
        const double mean_p = sx / n;
        const double mean_b = sy / n;
        // Jensen's alpha, annualised: the return the benchmark does not explain.
        const double a = (mean_p - rf_per_period) * annualization -
                         *betas->values[i] * (mean_b - rf_per_period) * annualization;
        if (is_finite(a)) out.values[i] = a;
    }
    return out;
}

Result<RollingSeries> RollingAnalyzer::value_at_risk(std::span<const Timestamp> ts,
                                                     std::span<const double> returns) const {
    if (auto ok = check_shape(ts, returns, cfg_.window); !ok) return fail(ok.error());
    if (cfg_.var_confidence <= 0.0 || cfg_.var_confidence >= 1.0) {
        return fail(bad("VaR confidence must lie in (0, 1)"));
    }

    RollingSeries out;
    out.name = "rolling_var";
    out.window = cfg_.window;
    out.timestamps.assign(ts.begin(), ts.end());
    out.values.assign(returns.size(), std::nullopt);

    // A quantile genuinely needs order statistics, so this is O(n log w) via a
    // sorted window rather than the O(n) the moment-based statistics achieve.
    // That cost is unavoidable and is why VaR is a separate method rather than
    // folded into the others.
    std::vector<double> window;
    window.reserve(cfg_.window);

    for (std::size_t i = 0; i < returns.size(); ++i) {
        const auto position = std::lower_bound(window.begin(), window.end(), returns[i]);
        window.insert(position, returns[i]);

        if (i >= cfg_.window) {
            const double leaving = returns[i - cfg_.window];
            const auto it = std::lower_bound(window.begin(), window.end(), leaving);
            if (it != window.end() && *it == leaving) window.erase(it);
        }
        if (i + 1 < cfg_.window) continue;

        // NEAREST-RANK: ceil(q*n) - 1, not floor(q*n).
        //
        // floor overshoots past the tail into the body of the distribution and
        // reports a LESS SEVERE loss than the confidence level asks for -- it
        // understates risk, which is the one direction a risk statistic must
        // never err in. With 20 observations at 10%, floor selects the third
        // worst return; the tenth percentile is the second worst.
        const auto rank = static_cast<std::size_t>(
            std::ceil(cfg_.var_confidence * static_cast<double>(window.size())));
        const std::size_t index = rank > 0 ? rank - 1 : 0;
        const double quantile = window[std::min(index, window.size() - 1)];
        // Reported POSITIVE for a loss, which is what a risk report expects.
        out.values[i] = -quantile;
    }
    return out;
}

Result<RollingSeries> RollingAnalyzer::conditional_var(std::span<const Timestamp> ts,
                                                       std::span<const double> returns) const {
    if (auto ok = check_shape(ts, returns, cfg_.window); !ok) return fail(ok.error());

    RollingSeries out;
    out.name = "rolling_cvar";
    out.window = cfg_.window;
    out.timestamps.assign(ts.begin(), ts.end());
    out.values.assign(returns.size(), std::nullopt);

    std::vector<double> window;
    window.reserve(cfg_.window);

    for (std::size_t i = 0; i < returns.size(); ++i) {
        const auto position = std::lower_bound(window.begin(), window.end(), returns[i]);
        window.insert(position, returns[i]);

        if (i >= cfg_.window) {
            const double leaving = returns[i - cfg_.window];
            const auto it = std::lower_bound(window.begin(), window.end(), leaving);
            if (it != window.end() && *it == leaving) window.erase(it);
        }
        if (i + 1 < cfg_.window) continue;

        // Mean of the tail. At least one observation always, so a small window
        // with a tight confidence still produces a number rather than a
        // division by zero.
        const auto tail = std::max<std::size_t>(
            1, static_cast<std::size_t>(cfg_.var_confidence * static_cast<double>(window.size())));
        const double sum = std::accumulate(window.begin(),
                                           window.begin() + static_cast<std::ptrdiff_t>(tail), 0.0);
        out.values[i] = -(sum / static_cast<double>(tail));
    }
    return out;
}

double RollingAnalyzer::omega_ratio(std::span<const double> returns, double threshold) noexcept {
    if (returns.empty()) return 0.0;

    double gains = 0.0;
    double losses = 0.0;
    for (const double r : returns) {
        if (!is_finite(r)) continue;
        const double excess = r - threshold;
        if (excess > 0.0)
            gains += excess;
        else
            losses -= excess;
    }
    // No losses at all means Omega is unbounded, which is a property of the
    // sample rather than of the strategy. Zero is the honest sentinel; an
    // infinity here would propagate into every report that touched it.
    if (losses <= 0.0) return 0.0;
    const double omega = gains / losses;
    return is_finite(omega) ? omega : 0.0;
}

Result<std::vector<Duration>> RollingAnalyzer::drawdown_durations(std::span<const Timestamp> ts,
                                                                  std::span<const double> equity) {
    if (ts.size() != equity.size()) {
        return fail(bad("timestamp and equity series lengths differ"));
    }
    if (equity.empty()) return std::vector<Duration>{};

    std::vector<Duration> out;
    double peak = equity.front();
    Timestamp peak_time = ts.front();
    bool underwater = false;

    for (std::size_t i = 0; i < equity.size(); ++i) {
        if (!is_finite(equity[i])) continue;
        if (equity[i] >= peak) {
            if (underwater) {
                // Recovered: the episode ran from its peak to this instant.
                out.push_back(ts[i] - peak_time);
                underwater = false;
            }
            peak = equity[i];
            peak_time = ts[i];
        } else {
            underwater = true;
        }
    }
    // An unrecovered drawdown is deliberately NOT appended. Its duration is
    // still running, and recording it as though it had ended would understate
    // exactly the episode a reader most needs to see.
    return out;
}

}  // namespace ptl::analytics
