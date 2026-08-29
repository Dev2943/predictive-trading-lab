#include <algorithm>
#include <cmath>
#include <sstream>
#include <vector>

#include "ptl/analytics/risk_analyzer.hpp"

namespace ptl::analytics {
namespace {

[[nodiscard]] Error bad(std::string message, std::string context = {}) {
    return make_error(ErrorCode::InvalidArgument, std::move(message), std::move(context));
}

[[nodiscard]] double mean_of(std::span<const double> v) {
    if (v.empty()) return 0.0;
    double sum = 0.0;
    for (const double x : v) sum += x;
    return sum / static_cast<double>(v.size());
}

}  // namespace

double RiskAnalyzer::beta_of(std::span<const double> returns, std::span<const double> benchmark) {
    const std::size_t n = std::min(returns.size(), benchmark.size());
    if (n < 2) return 0.0;

    const double mr = mean_of(returns.subspan(0, n));
    const double mb = mean_of(benchmark.subspan(0, n));

    double cov = 0.0;
    double var_b = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        const double db = benchmark[i] - mb;
        cov += (returns[i] - mr) * db;
        var_b += db * db;
    }
    // A benchmark with no variance explains nothing, so beta is undefined.
    // Zero is the honest answer; dividing would give infinity.
    if (var_b <= 0.0) return 0.0;
    const double b = cov / var_b;
    return is_finite(b) ? b : 0.0;
}

double RiskAnalyzer::tracking_error_of(std::span<const double> returns,
                                       std::span<const double> benchmark, double periods_per_year) {
    const std::size_t n = std::min(returns.size(), benchmark.size());
    if (n < 2) return 0.0;

    // Standard deviation of the DIFFERENCE series, annualised.
    StatisticsAccumulator active;
    for (std::size_t i = 0; i < n; ++i) active.update(returns[i] - benchmark[i]);
    return active.stdev() * std::sqrt(std::max(1.0, periods_per_year));
}

Result<RiskMetrics> RiskAnalyzer::analyze(std::span<const double> returns,
                                          std::span<const double> benchmark,
                                          double max_drawdown) const {
    if (returns.empty()) return fail(bad("cannot analyse an empty return series"));
    if (!benchmark.empty() && benchmark.size() != returns.size()) {
        // A length mismatch is an error, not something to truncate silently:
        // truncating would silently pair each return with the wrong benchmark
        // observation and produce a beta that means nothing.
        return fail(bad("benchmark series length (" + std::to_string(benchmark.size()) +
                        ") does not match the return series (" + std::to_string(returns.size()) +
                        ")"));
    }

    RiskMetrics m;
    m.periods = returns.size();

    StatisticsAccumulator stats{cfg_.sortino_threshold / cfg_.periods_per_year};
    for (const double r : returns) {
        if (!is_finite(r)) return fail(bad("return series contains a non-finite value"));
        stats.update(r);
    }

    const double ann = cfg_.periods_per_year;
    const double sqrt_ann = std::sqrt(std::max(1.0, ann));

    m.annualized_return = stats.mean() * ann;
    m.annualized_volatility = stats.stdev() * sqrt_ann;
    m.downside_deviation = stats.downside_deviation(cfg_.sortino_threshold / ann) * sqrt_ann;
    m.skewness = stats.skewness();
    m.excess_kurtosis = stats.excess_kurtosis();
    m.best_period = stats.max();
    m.worst_period = stats.min();

    const double rf_per_period = cfg_.risk_free_rate / ann;
    if (stats.stdev() > 0.0) {
        m.sharpe = (stats.mean() - rf_per_period) / stats.stdev() * sqrt_ann;
    }
    if (m.downside_deviation > 0.0) {
        m.sortino = (m.annualized_return - cfg_.sortino_threshold) / m.downside_deviation;
    }
    if (max_drawdown > 0.0) {
        m.calmar = m.annualized_return / max_drawdown;
    }

    m.value_at_risk_95 = quantile_of(returns, 0.05);
    m.expected_shortfall_95 = expected_shortfall(returns, 0.05);

    if (!benchmark.empty()) {
        m.beta = beta_of(returns, benchmark);
        m.tracking_error = tracking_error_of(returns, benchmark, ann);

        const double mr = mean_of(returns);
        const double mb = mean_of(benchmark);

        // Jensen's alpha: the annualised return the benchmark does NOT explain.
        m.alpha = (mr - rf_per_period) * ann - m.beta * ((mb - rf_per_period) * ann);

        if (m.tracking_error > 0.0) {
            // Active return over tracking error. Distinct from Sharpe, which
            // measures against cash rather than against the benchmark.
            m.information_ratio = (mr - mb) * ann / m.tracking_error;
        }
        if (std::abs(m.beta) > 1e-12) {
            // Excess return per unit of BETA. Undefined at zero beta, where a
            // market-neutral book legitimately sits, so it stays zero there.
            m.treynor_ratio = (mr - rf_per_period) * ann / m.beta;
        }

        StatisticsAccumulator bench_stats;
        for (const double b : benchmark) bench_stats.update(b);

        double cov = 0.0;
        for (std::size_t i = 0; i < returns.size(); ++i) {
            cov += (returns[i] - mr) * (benchmark[i] - mb);
        }
        const auto n = static_cast<double>(returns.size());
        if (n > 1.0 && stats.stdev() > 0.0 && bench_stats.stdev() > 0.0) {
            cov /= (n - 1.0);
            m.correlation_to_benchmark =
                std::clamp(cov / (stats.stdev() * bench_stats.stdev()), -1.0, 1.0);
            m.r_squared = m.correlation_to_benchmark * m.correlation_to_benchmark;
        }
    }
    return m;
}

std::string RiskMetrics::describe() const {
    std::ostringstream ss;
    ss.precision(6);
    ss << std::fixed;
    ss << "risk over " << periods << " periods\n";
    ss << "  annualised return    " << annualized_return << '\n';
    ss << "  annualised vol       " << annualized_volatility << '\n';
    ss << "  downside deviation   " << downside_deviation << '\n';
    ss << "  Sharpe               " << sharpe << '\n';
    ss << "  Sortino              " << sortino << '\n';
    ss << "  Calmar               " << calmar << '\n';
    ss << "  beta                 " << beta << '\n';
    ss << "  alpha                " << alpha << '\n';
    ss << "  tracking error       " << tracking_error << '\n';
    ss << "  information ratio    " << information_ratio << '\n';
    ss << "  Treynor              " << treynor_ratio << '\n';
    ss << "  R^2 to benchmark     " << r_squared << '\n';
    ss << "  skewness             " << skewness << '\n';
    ss << "  excess kurtosis      " << excess_kurtosis << '\n';
    ss << "  VaR 95               " << value_at_risk_95 << '\n';
    ss << "  ES 95                " << expected_shortfall_95 << '\n';
    return ss.str();
}

}  // namespace ptl::analytics
