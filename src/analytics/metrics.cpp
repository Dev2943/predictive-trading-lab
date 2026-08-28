#include "ptl/analytics/metrics.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>
#include <sstream>

namespace ptl::analytics {
namespace {

[[nodiscard]] double mean_of(std::span<const double> v) {
    if (v.empty()) return 0.0;
    return std::accumulate(v.begin(), v.end(), 0.0) / static_cast<double>(v.size());
}

/// Sample standard deviation, computed with Welford rather than
/// sum-of-squares: the naive form loses catastrophic precision when the mean is
/// large relative to the spread, which is exactly the case for minute returns.
[[nodiscard]] double stdev_of(std::span<const double> v) {
    if (v.size() < 2) return 0.0;
    double mean = 0.0;
    double m2 = 0.0;
    std::size_t n = 0;
    for (const double x : v) {
        ++n;
        const double delta = x - mean;
        mean += delta / static_cast<double>(n);
        m2 += delta * (x - mean);
    }
    return std::sqrt(m2 / static_cast<double>(n - 1));
}

}  // namespace

std::vector<double> MetricsEngine::returns(std::span<const portfolio::EquityPoint> curve) const {
    std::vector<double> out;
    if (curve.size() < 2) return out;
    out.reserve(curve.size() - 1);
    for (std::size_t i = 1; i < curve.size(); ++i) {
        const double prev = curve[i - 1].equity.get();
        const double cur = curve[i].equity.get();
        // A non-positive equity makes a return undefined. Emitting zero keeps
        // the series finite; the run is already invalid at that point and the
        // portfolio's own identity check will have said so.
        if (!is_finite(prev) || !is_finite(cur) || prev <= 0.0 || cur <= 0.0) {
            out.push_back(0.0);
            continue;
        }
        out.push_back(cfg_.use_log_returns ? std::log(cur / prev) : (cur / prev - 1.0));
    }
    return out;
}

double MetricsEngine::max_drawdown(std::span<const double> equity) {
    double peak = -std::numeric_limits<double>::infinity();
    double worst = 0.0;
    for (const double e : equity) {
        if (!is_finite(e)) continue;
        peak = std::max(peak, e);
        if (peak > 0.0) worst = std::max(worst, 1.0 - e / peak);
    }
    return worst;
}

PerformanceMetrics MetricsEngine::compute(std::span<const portfolio::EquityPoint> curve,
                                          std::span<const accounting::Trade> trades) const {
    PerformanceMetrics m;
    if (curve.empty()) return m;

    m.periods = curve.size();
    m.initial_equity = curve.front().equity;
    m.final_equity = curve.back().equity;
    m.total_costs = curve.back().cumulative_costs;
    m.total_turnover = curve.back().turnover;

    const double e0 = m.initial_equity.get();
    const double e1 = m.final_equity.get();
    if (e0 > 0.0 && is_finite(e1)) m.cumulative_return = e1 / e0 - 1.0;

    const std::vector<double> r = returns(curve);
    if (!r.empty()) {
        const double mu = mean_of(r);
        const double sd = stdev_of(r);
        const double ann = cfg_.periods_per_year;
        const double sqrt_ann = std::sqrt(ann);

        // Log returns compound additively, so annualising the MEAN is exact.
        // Simple returns need the geometric form, which is why the basis is a
        // documented configuration rather than an assumption.
        m.annualized_return = cfg_.use_log_returns ? mu * ann : std::pow(1.0 + mu, ann) - 1.0;
        m.annualized_volatility = sd * sqrt_ann;

        const double rf_per_period = cfg_.risk_free_rate / ann;
        if (sd > 0.0) m.sharpe = (mu - rf_per_period) / sd * sqrt_ann;

        // Sortino: only returns BELOW the threshold contribute, and the
        // denominator divides by the full count -- not the count of downside
        // periods. Dividing by the smaller count is a common error that
        // inflates the ratio.
        const double mar_per_period = cfg_.sortino_threshold / ann;
        double downside_sq = 0.0;
        for (const double x : r) {
            const double d = std::min(0.0, x - mar_per_period);
            downside_sq += d * d;
        }
        const double dd = std::sqrt(downside_sq / static_cast<double>(r.size()));
        m.downside_volatility = dd * sqrt_ann;
        if (dd > 0.0) m.sortino = (mu - mar_per_period) / dd * sqrt_ann;

        auto sorted = r;
        std::sort(sorted.begin(), sorted.end());
        m.worst_period = sorted.front();
        m.best_period = sorted.back();
        const auto tail = std::max<std::size_t>(1, sorted.size() / 20);
        m.expected_shortfall_5pct = mean_of(std::span<const double>{sorted.data(), tail});

        if (sd > 0.0 && r.size() > 2) {
            double s = 0.0;
            for (const double x : r) s += std::pow((x - mu) / sd, 3.0);
            m.skewness = s / static_cast<double>(r.size());
        }

        // Elapsed time drives CAGR, not the period count: a curve with gaps
        // would otherwise report a compounding rate it never achieved.
        const Duration elapsed = curve.back().ts - curve.front().ts;
        const double years = static_cast<double>(elapsed.count()) / (365.25 * 24 * 3600 * 1e9);
        if (years > 0.0 && e0 > 0.0 && e1 > 0.0) {
            m.cagr = std::pow(e1 / e0, 1.0 / years) - 1.0;
        }
        if (years > 0.0 && e0 > 0.0) {
            m.annualized_turnover = m.total_turnover.get() / e0 / years;
        }
    }

    std::vector<double> equity;
    equity.reserve(curve.size());
    for (const auto& p : curve) equity.push_back(p.equity.get());
    m.max_drawdown = max_drawdown(equity);

    double peak = -std::numeric_limits<double>::infinity();
    std::size_t peak_idx = 0;
    double worst = 0.0;
    for (std::size_t i = 0; i < equity.size(); ++i) {
        if (equity[i] > peak) {
            peak = equity[i];
            peak_idx = i;
        }
        if (peak > 0.0) {
            const double dd = 1.0 - equity[i] / peak;
            if (dd > worst) {
                worst = dd;
                m.max_drawdown_start = curve[peak_idx].ts;
                m.max_drawdown_trough = curve[i].ts;
                m.max_drawdown_periods = i - peak_idx;
            }
        }
    }
    if (m.max_drawdown > 0.0) m.calmar = m.cagr / m.max_drawdown;

    // --- trade level ---------------------------------------------------------
    double win_sum = 0.0;
    double loss_sum = 0.0;
    for (const auto& t : trades) {
        ++m.trades;
        const double pnl = t.net_pnl().get();
        if (pnl > 0.0) {
            ++m.wins;
            win_sum += pnl;
        } else {
            ++m.losses;
            loss_sum += -pnl;
        }
    }
    if (m.trades > 0) {
        m.win_rate = static_cast<double>(m.wins) / static_cast<double>(m.trades);
        m.average_win = Notional{m.wins > 0 ? win_sum / static_cast<double>(m.wins) : 0.0};
        m.average_loss = Notional{m.losses > 0 ? loss_sum / static_cast<double>(m.losses) : 0.0};
        if (m.average_loss.get() > 0.0) {
            m.win_loss_ratio = m.average_win.get() / m.average_loss.get();
        }
        if (loss_sum > 0.0) m.profit_factor = win_sum / loss_sum;
        // Expectancy per trade. A high win rate with negative expectancy is the
        // classic trap, which is why both are reported.
        m.expectancy = Notional{(win_sum - loss_sum) / static_cast<double>(m.trades)};
    }

    const double gross = (e1 - e0) + m.total_costs.get();
    if (std::abs(gross) > 0.0) m.cost_to_gross_ratio = m.total_costs.get() / std::abs(gross);
    return m;
}

std::string PerformanceMetrics::describe() const {
    std::ostringstream ss;
    ss.precision(4);
    ss << std::fixed;
    ss << "performance over " << periods << " periods\n";
    ss << "  cumulative return    " << cumulative_return << '\n';
    ss << "  CAGR                 " << cagr << '\n';
    ss << "  annualised vol       " << annualized_volatility << '\n';
    ss << "  Sharpe               " << sharpe << '\n';
    ss << "  Sortino              " << sortino << '\n';
    ss << "  Calmar               " << calmar << '\n';
    ss << "  max drawdown         " << max_drawdown << '\n';
    ss << "  trades               " << trades << " (" << wins << "W / " << losses << "L)\n";
    ss << "  win rate             " << win_rate << '\n';
    ss << "  profit factor        " << profit_factor << '\n';
    ss << "  expectancy/trade     " << expectancy.get() << '\n';
    ss << "  annualised turnover  " << annualized_turnover << '\n';
    ss << "  cost / gross P&L     " << cost_to_gross_ratio << '\n';
    ss << "  worst period         " << worst_period << '\n';
    ss << "  ES 5%                " << expected_shortfall_5pct << '\n';
    return ss.str();
}

}  // namespace ptl::analytics
