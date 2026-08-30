#include "ptl/optimization/objective.hpp"

#include <algorithm>
#include <cmath>
#include <numeric>
#include <sstream>

namespace ptl::optimization {
namespace {

[[nodiscard]] Error bad(std::string message, std::string context = {}) {
    return make_error(ErrorCode::InvalidArgument, std::move(message), std::move(context));
}

}  // namespace

// ---------------------------------------------------------------------------
// Shared types
// ---------------------------------------------------------------------------

std::string_view to_string(OptimizationStatus s) noexcept {
    switch (s) {
        case OptimizationStatus::Optimal:
            return "optimal";
        case OptimizationStatus::ConstrainedOptimal:
            return "constrained_optimal";
        case OptimizationStatus::MaxIterations:
            return "max_iterations";
        case OptimizationStatus::Infeasible:
            return "infeasible";
        case OptimizationStatus::SingularRiskModel:
            return "singular_risk_model";
        case OptimizationStatus::Failed:
            return "failed";
    }
    return "unknown";
}

std::string_view to_string(ObjectiveKind k) noexcept {
    switch (k) {
        case ObjectiveKind::MaximizeReturn:
            return "maximize_return";
        case ObjectiveKind::MaximizeSharpe:
            return "maximize_sharpe";
        case ObjectiveKind::MinimizeVariance:
            return "minimize_variance";
        case ObjectiveKind::MinimizeTrackingError:
            return "minimize_tracking_error";
        case ObjectiveKind::RiskParity:
            return "risk_parity";
        case ObjectiveKind::Kelly:
            return "kelly";
    }
    return "unknown";
}

Result<bool> OptimizationInput::validate() const {
    if (instruments.empty()) {
        return fail(bad("optimization universe is empty"));
    }
    const std::size_t n = instruments.size();

    // Duplicates would give one instrument two weights, and the rebalance
    // engine would see a target it cannot satisfy.
    std::vector<std::uint32_t> seen;
    seen.reserve(n);
    for (const auto instrument : instruments) {
        if (instrument == kInvalidInstrument) {
            return fail(bad("universe contains an invalid instrument"));
        }
        const auto idx = index_of(instrument);
        if (std::find(seen.begin(), seen.end(), idx) != seen.end()) {
            return fail(bad("universe contains a duplicate instrument", std::to_string(idx)));
        }
        seen.push_back(idx);
    }

    const auto check = [n](const std::vector<double>& v, const char* name) -> Result<bool> {
        if (v.empty()) return true;  // absent is allowed; wrong-length is not
        if (v.size() != n) {
            return fail(bad(std::string{name} + " has " + std::to_string(v.size()) +
                            " entries for a universe of " + std::to_string(n)));
        }
        for (const double x : v) {
            if (!is_finite(x)) {
                return fail(bad(std::string{name} + " contains a non-finite value"));
            }
        }
        return true;
    };

    if (auto r = check(expected_returns, "expected_returns"); !r) return r;
    if (auto r = check(volatilities, "volatilities"); !r) return r;
    if (auto r = check(current_weights, "current_weights"); !r) return r;
    if (auto r = check(signals, "signals"); !r) return r;
    if (auto r = check(betas, "betas"); !r) return r;

    if (!sectors.empty() && sectors.size() != n) {
        return fail(bad("sectors has the wrong length for the universe"));
    }
    if (!covariance.empty()) {
        if (covariance.size() != n) {
            return fail(bad("covariance is " + std::to_string(covariance.size()) + "x" +
                            std::to_string(covariance.size()) + " for a universe of " +
                            std::to_string(n)));
        }
        if (!covariance.all_finite()) {
            return fail(bad("covariance contains non-finite entries"));
        }
    }
    return true;
}

Result<bool> validate_weights(std::span<const double> weights) {
    for (std::size_t i = 0; i < weights.size(); ++i) {
        if (!is_finite(weights[i])) {
            // A NaN weight propagates silently through rebalancing into an
            // order quantity, and the first sign of trouble is a rejected order
            // with an unreadable size.
            return fail(bad("weight " + std::to_string(i) + " is not finite"));
        }
    }
    return true;
}

void finalize_result(OptimizationResult& result, const OptimizationInput& input) {
    double gross = 0.0;
    double net = 0.0;
    for (const double w : result.weights) {
        gross += std::abs(w);
        net += w;
    }
    result.gross_exposure = gross;
    result.net_exposure = net;
    // A set of weights summing to less than one is a CASH POSITION, not an
    // error. Reporting it explicitly stops a reader assuming full investment.
    result.cash_weight = 1.0 - net;

    if (!input.expected_returns.empty() && input.expected_returns.size() == result.weights.size()) {
        double expected = 0.0;
        for (std::size_t i = 0; i < result.weights.size(); ++i) {
            expected += result.weights[i] * input.expected_returns[i];
        }
        result.expected_return = is_finite(expected) ? expected : 0.0;
    }

    if (!input.covariance.empty() && input.covariance.size() == result.weights.size()) {
        result.expected_variance = input.covariance.quadratic_form(result.weights);
        result.expected_volatility = std::sqrt(std::max(0.0, result.expected_variance));
    } else if (!input.volatilities.empty() && input.volatilities.size() == result.weights.size()) {
        // No covariance: assume independence. Stated here rather than hidden,
        // because it understates risk whenever the book is correlated.
        double variance = 0.0;
        for (std::size_t i = 0; i < result.weights.size(); ++i) {
            const double contribution = result.weights[i] * input.volatilities[i];
            variance += contribution * contribution;
        }
        result.expected_variance = variance;
        result.expected_volatility = std::sqrt(std::max(0.0, variance));
    }

    result.sharpe = result.expected_volatility > 0.0
                        ? result.expected_return / result.expected_volatility
                        : 0.0;
    if (!is_finite(result.sharpe)) result.sharpe = 0.0;

    if (input.current_weights.size() == result.weights.size()) {
        double turnover = 0.0;
        for (std::size_t i = 0; i < result.weights.size(); ++i) {
            turnover += std::abs(result.weights[i] - input.current_weights[i]);
        }
        result.turnover = turnover;
    }
}

std::string OptimizationResult::describe() const {
    std::ostringstream ss;
    ss.precision(6);
    ss << std::fixed << to_string(status) << ": gross " << gross_exposure << ", net "
       << net_exposure << ", expected return " << expected_return << ", volatility "
       << expected_volatility << ", Sharpe " << sharpe;
    if (!binding_constraints.empty()) {
        ss << "\n  binding:";
        for (const auto& c : binding_constraints) ss << ' ' << c;
    }
    if (!detail.empty()) ss << "\n  " << detail;
    return ss.str();
}

// ---------------------------------------------------------------------------
// Objective
// ---------------------------------------------------------------------------

Result<bool> Objective::check_requirements(const OptimizationInput& input) const {
    // Checked BEFORE a solve, so a failure names the missing ingredient rather
    // than surfacing later as a NaN nobody can trace.
    switch (cfg_.kind) {
        case ObjectiveKind::MaximizeReturn:
        case ObjectiveKind::MaximizeSharpe:
        case ObjectiveKind::Kelly:
            if (input.expected_returns.empty()) {
                return fail(bad(std::string{to_string(cfg_.kind)} +
                                " needs expected returns; a zero forecast is a real "
                                "opinion, not an absence of one, so none is supplied "
                                "by default"));
            }
            break;
        case ObjectiveKind::MinimizeVariance:
        case ObjectiveKind::RiskParity:
            if (input.covariance.empty() && input.volatilities.empty()) {
                return fail(bad(std::string{to_string(cfg_.kind)} +
                                " needs a covariance matrix or volatilities"));
            }
            break;
        case ObjectiveKind::MinimizeTrackingError:
            if (cfg_.benchmark_weights.empty()) {
                return fail(
                    bad("tracking-error minimisation needs benchmark weights; "
                        "without them it would silently track cash"));
            }
            if (cfg_.benchmark_weights.size() != input.size()) {
                return fail(bad("benchmark weights do not match the universe"));
            }
            if (input.covariance.empty()) {
                return fail(bad("tracking-error minimisation needs a covariance matrix"));
            }
            break;
    }
    return true;
}

Result<double> Objective::value(std::span<const double> w, const OptimizationInput& input) const {
    if (w.size() != input.size()) return fail(bad("weights do not match the universe"));

    const auto expected = [&]() {
        double total = 0.0;
        if (input.expected_returns.size() == w.size()) {
            for (std::size_t i = 0; i < w.size(); ++i) {
                total += w[i] * input.expected_returns[i];
            }
        }
        return total;
    };
    const auto variance = [&]() { return input.covariance.quadratic_form(w); };

    double result = 0.0;
    switch (cfg_.kind) {
        case ObjectiveKind::MaximizeReturn:
            // Mean-variance: return penalised by risk aversion times variance.
            result = expected() - 0.5 * cfg_.risk_aversion * variance();
            break;

        case ObjectiveKind::MaximizeSharpe: {
            const double vol = std::sqrt(std::max(0.0, variance()));
            // Zero volatility with a positive edge is a modelling artefact, not
            // an infinite Sharpe. Returning the raw excess keeps the search
            // finite and the answer honest.
            result = vol > 1e-12 ? (expected() - cfg_.risk_free_rate) / vol
                                 : expected() - cfg_.risk_free_rate;
            break;
        }

        case ObjectiveKind::MinimizeVariance:
            // Expressed as a MAXIMISATION of negative variance, like every
            // other objective here. Mixing conventions is how a sign error
            // hides for months.
            result = -variance();
            break;

        case ObjectiveKind::MinimizeTrackingError: {
            std::vector<double> active(w.size(), 0.0);
            for (std::size_t i = 0; i < w.size(); ++i) {
                active[i] = w[i] - cfg_.benchmark_weights[i];
            }
            result = -input.covariance.quadratic_form(active);
            break;
        }

        case ObjectiveKind::RiskParity: {
            // Maximise the sum of log weights subject to risk: the standard
            // convex reformulation, whose optimum equalises risk contributions.
            const double v = variance();
            double log_sum = 0.0;
            for (const double x : w) {
                if (x <= 1e-12) return -1e18;  // outside the domain
                log_sum += std::log(x);
            }
            result = log_sum - 0.5 * static_cast<double>(w.size()) * std::log(std::max(v, 1e-18));
            break;
        }

        case ObjectiveKind::Kelly: {
            // f' μ - ½ f' Σ f, scaled. Full Kelly assumes the edge is exact; it
            // never is, so the fraction is applied to the return term.
            result = cfg_.kelly_fraction * expected() - 0.5 * variance();
            break;
        }
    }
    return is_finite(result) ? result : -1e18;
}

Result<std::vector<double>> Objective::gradient(std::span<const double> w,
                                                const OptimizationInput& input) const {
    if (w.size() != input.size()) return fail(bad("weights do not match the universe"));
    const std::size_t n = w.size();
    std::vector<double> g(n, 0.0);

    // Analytic throughout. A finite-difference gradient on a near-singular
    // covariance is dominated by rounding, and the solver then wanders instead
    // of converging.
    const auto sigma_w =
        input.covariance.empty() ? std::vector<double>(n, 0.0) : input.covariance.multiply(w);
    const bool have_mu = input.expected_returns.size() == n;

    switch (cfg_.kind) {
        case ObjectiveKind::MaximizeReturn:
            for (std::size_t i = 0; i < n; ++i) {
                g[i] =
                    (have_mu ? input.expected_returns[i] : 0.0) - cfg_.risk_aversion * sigma_w[i];
            }
            break;

        case ObjectiveKind::MaximizeSharpe: {
            const double variance = input.covariance.quadratic_form(w);
            const double vol = std::sqrt(std::max(0.0, variance));
            double expected = 0.0;
            if (have_mu) {
                for (std::size_t i = 0; i < n; ++i) expected += w[i] * input.expected_returns[i];
            }
            expected -= cfg_.risk_free_rate;
            if (vol > 1e-12) {
                // d/dw (μ'w / σ) = μ/σ - (μ'w) Σw / σ³
                const double vol_cubed = vol * vol * vol;
                for (std::size_t i = 0; i < n; ++i) {
                    const double mu = have_mu ? input.expected_returns[i] : 0.0;
                    g[i] = mu / vol - expected * sigma_w[i] / vol_cubed;
                }
            } else {
                for (std::size_t i = 0; i < n; ++i) {
                    g[i] = have_mu ? input.expected_returns[i] : 0.0;
                }
            }
            break;
        }

        case ObjectiveKind::MinimizeVariance:
            for (std::size_t i = 0; i < n; ++i) g[i] = -2.0 * sigma_w[i];
            break;

        case ObjectiveKind::MinimizeTrackingError: {
            std::vector<double> active(n, 0.0);
            for (std::size_t i = 0; i < n; ++i) {
                active[i] = w[i] - cfg_.benchmark_weights[i];
            }
            const auto sigma_active = input.covariance.multiply(active);
            for (std::size_t i = 0; i < n; ++i) g[i] = -2.0 * sigma_active[i];
            break;
        }

        case ObjectiveKind::RiskParity: {
            const double variance = std::max(input.covariance.quadratic_form(w), 1e-18);
            for (std::size_t i = 0; i < n; ++i) {
                const double x = std::max(w[i], 1e-12);
                g[i] = 1.0 / x - static_cast<double>(n) * sigma_w[i] / variance;
            }
            break;
        }

        case ObjectiveKind::Kelly:
            for (std::size_t i = 0; i < n; ++i) {
                const double mu = have_mu ? input.expected_returns[i] : 0.0;
                g[i] = cfg_.kelly_fraction * mu - sigma_w[i];
            }
            break;
    }

    for (double& x : g) {
        if (!is_finite(x)) x = 0.0;
    }
    return g;
}

}  // namespace ptl::optimization
