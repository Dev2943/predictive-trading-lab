#include "ptl/optimization/optimizer.hpp"

#include <algorithm>
#include <cmath>
#include <numeric>

namespace ptl::optimization {
namespace {

[[nodiscard]] Error bad(std::string message, std::string context = {}) {
    return make_error(ErrorCode::InvalidArgument, std::move(message), std::move(context));
}

/// Per-asset volatility, from the covariance diagonal or the explicit vector.
[[nodiscard]] std::vector<double> volatilities_of(const OptimizationInput& input) {
    const std::size_t n = input.size();
    if (input.volatilities.size() == n) return input.volatilities;
    std::vector<double> out(n, 0.0);
    if (input.covariance.size() == n) {
        for (std::size_t i = 0; i < n; ++i) {
            out[i] = std::sqrt(std::max(0.0, input.covariance.at(i, i)));
        }
    }
    return out;
}

}  // namespace

// ---------------------------------------------------------------------------
// OptimizerBase
// ---------------------------------------------------------------------------

Result<OptimizationResult> OptimizerBase::complete(std::vector<double> weights,
                                                   const OptimizationInput& input,
                                                   OptimizationStatus status) const {
    OptimizationResult result;
    result.weights = std::move(weights);

    const ConstraintProjector projector{cfg_.constraints};
    if (auto projected = projector.project(result.weights, input, &result.binding_constraints);
        !projected) {
        return fail(projected.error());
    }

    // THE NaN GATE. Every optimizer ends here, so the guarantee holds for all
    // of them rather than for whichever ones remembered to check.
    if (auto ok = validate_weights(result.weights); !ok) return fail(ok.error());

    result.status =
        result.binding_constraints.empty()
            ? status
            : (status == OptimizationStatus::Optimal ? OptimizationStatus::ConstrainedOptimal
                                                     : status);
    finalize_result(result, input);
    return result;
}

std::vector<double> OptimizerBase::initial_weights(const OptimizationInput& input) const {
    const std::size_t n = input.size();
    if (n == 0) return {};
    // Equal weight, scaled to the gross budget. A feasible start matters: a
    // projected-gradient method launched from an infeasible point spends its
    // first iterations walking back to the feasible set.
    const double budget =
        cfg_.constraints.max_gross_leverage * (1.0 - cfg_.constraints.cash_reserve);
    return std::vector<double>(n, budget / static_cast<double>(n));
}

Result<OptimizationResult> OptimizerBase::ascend(const OptimizationInput& input,
                                                 const Objective& objective,
                                                 std::vector<double> start) const {
    const std::size_t n = input.size();
    if (n == 0) return fail(bad("cannot optimize an empty universe"));

    const ConstraintProjector projector{cfg_.constraints};
    std::vector<double> w = std::move(start);
    if (auto projected = projector.project(w, input, nullptr); !projected) {
        return fail(projected.error());
    }

    auto best_value = objective.value(w, input);
    if (!best_value) return fail(best_value.error());

    std::vector<double> best = w;
    double step = cfg_.step_size;
    std::size_t iterations = 0;
    bool converged = false;

    for (; iterations < cfg_.max_iterations; ++iterations) {
        auto gradient = objective.gradient(w, input);
        if (!gradient) return fail(gradient.error());

        std::vector<double> candidate(n, 0.0);
        for (std::size_t i = 0; i < n; ++i) candidate[i] = w[i] + step * (*gradient)[i];

        // PROJECT after every step. Optimising in the unconstrained space and
        // clipping at the end would give a point that is feasible but not
        // optimal among feasible points.
        if (auto projected = projector.project(candidate, input, nullptr); !projected) {
            return fail(projected.error());
        }

        auto candidate_value = objective.value(candidate, input);
        if (!candidate_value) return fail(candidate_value.error());

        if (*candidate_value > *best_value) {
            double delta = 0.0;
            for (std::size_t i = 0; i < n; ++i) delta += std::abs(candidate[i] - w[i]);

            w = candidate;
            best = candidate;
            best_value = candidate_value;

            if (delta < cfg_.tolerance) {
                converged = true;
                break;
            }
        } else {
            // Overshot: halve the step. Backtracking rather than a fixed step
            // is what keeps the search stable on the badly-scaled problems a
            // near-singular covariance produces.
            step *= 0.5;
            if (step < 1e-14) {
                converged = true;
                break;
            }
        }
    }

    auto result =
        complete(std::move(best), input,
                 converged ? OptimizationStatus::Optimal : OptimizationStatus::MaxIterations);
    if (!result) return result;
    result->iterations = iterations;
    return result;
}

// ---------------------------------------------------------------------------
// Equal weight
// ---------------------------------------------------------------------------

Result<OptimizationResult> EqualWeightOptimizer::optimize(const OptimizationInput& input) const {
    if (auto ok = input.validate(); !ok) return fail(ok.error());
    const std::size_t n = input.size();

    const double budget =
        cfg_.constraints.max_gross_leverage * (1.0 - cfg_.constraints.cash_reserve);
    // No covariance needed, no forecast needed. That is the point: this is the
    // benchmark every other optimizer must beat after costs.
    std::vector<double> w(n, budget / static_cast<double>(n));
    return complete(std::move(w), input, OptimizationStatus::Optimal);
}

std::unique_ptr<IPortfolioOptimizer> EqualWeightOptimizer::clone() const {
    return std::make_unique<EqualWeightOptimizer>(cfg_);
}

// ---------------------------------------------------------------------------
// Inverse volatility
// ---------------------------------------------------------------------------

Result<OptimizationResult> InverseVolatilityOptimizer::optimize(
    const OptimizationInput& input) const {
    if (auto ok = input.validate(); !ok) return fail(ok.error());
    const std::size_t n = input.size();
    const auto vols = volatilities_of(input);

    std::vector<double> w(n, 0.0);
    double total = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        // A zero-volatility asset would take infinite weight, so it takes none.
        // A constant price series is a data problem, not an arbitrage.
        w[i] = vols[i] > 1e-12 ? 1.0 / vols[i] : 0.0;
        total += w[i];
    }

    if (total <= 0.0) {
        // Every asset had zero or missing volatility: fall back to equal weight
        // and say so, rather than returning zeros that look like a deliberate
        // flat position.
        auto result = EqualWeightOptimizer{cfg_}.optimize(input);
        if (result) {
            result->detail = "all volatilities were zero or missing; fell back to equal weight";
        }
        return result;
    }

    const double budget =
        cfg_.constraints.max_gross_leverage * (1.0 - cfg_.constraints.cash_reserve);
    for (double& x : w) x = x / total * budget;
    return complete(std::move(w), input, OptimizationStatus::Optimal);
}

std::unique_ptr<IPortfolioOptimizer> InverseVolatilityOptimizer::clone() const {
    return std::make_unique<InverseVolatilityOptimizer>(cfg_);
}

// ---------------------------------------------------------------------------
// Risk parity
// ---------------------------------------------------------------------------

std::vector<double> RiskParityOptimizer::risk_contributions(std::span<const double> w,
                                                            const SymmetricMatrix& cov) {
    const std::size_t n = w.size();
    std::vector<double> out(n, 0.0);
    if (cov.size() != n || n == 0) return out;

    const double variance = cov.quadratic_form(w);
    if (variance <= 0.0) return out;
    const double vol = std::sqrt(variance);

    // RC_i = w_i (Σw)_i / σ. These sum to σ, so dividing by σ gives shares that
    // sum to one -- which is what "equal risk contribution" equalises.
    const auto sigma_w = cov.multiply(w);
    for (std::size_t i = 0; i < n; ++i) {
        const double rc = w[i] * sigma_w[i] / vol;
        out[i] = is_finite(rc) ? rc / vol : 0.0;
    }
    return out;
}

Result<OptimizationResult> RiskParityOptimizer::optimize(const OptimizationInput& input) const {
    if (auto ok = input.validate(); !ok) return fail(ok.error());
    const std::size_t n = input.size();

    if (input.covariance.empty()) {
        // Without correlations, equal risk contribution IS inverse volatility.
        // Saying so beats silently producing a different portfolio under the
        // risk-parity name.
        auto result = InverseVolatilityOptimizer{cfg_}.optimize(input);
        if (result) {
            result->detail =
                "no covariance supplied; equal risk contribution reduces to inverse "
                "volatility when correlations are unknown";
        }
        return result;
    }

    // Cyclical coordinate descent on w_i <- w_i * target / RC_i. Converges
    // reliably for long-only risk parity and avoids the log-domain issues the
    // gradient form has at the boundary.
    const auto vols = volatilities_of(input);
    std::vector<double> w(n, 0.0);
    double seed_total = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        w[i] = vols[i] > 1e-12 ? 1.0 / vols[i] : 1.0;
        seed_total += w[i];
    }
    for (double& x : w) x /= seed_total;

    const double target = 1.0 / static_cast<double>(n);
    std::size_t iterations = 0;
    bool converged = false;

    for (; iterations < cfg_.max_iterations; ++iterations) {
        const auto contributions = risk_contributions(w, input.covariance);

        double worst = 0.0;
        for (std::size_t i = 0; i < n; ++i) {
            worst = std::max(worst, std::abs(contributions[i] - target));
        }
        if (worst < 1e-10) {
            converged = true;
            break;
        }

        double total = 0.0;
        for (std::size_t i = 0; i < n; ++i) {
            if (contributions[i] > 1e-15) {
                // Damped update: the raw ratio oscillates on strongly
                // correlated books.
                w[i] *= std::pow(target / contributions[i], 0.5);
            }
            w[i] = std::max(w[i], 1e-12);
            total += w[i];
        }
        if (total <= 0.0 || !is_finite(total)) break;
        for (double& x : w) x /= total;
    }

    const double budget =
        cfg_.constraints.max_gross_leverage * (1.0 - cfg_.constraints.cash_reserve);
    for (double& x : w) x *= budget;

    auto result =
        complete(std::move(w), input,
                 converged ? OptimizationStatus::Optimal : OptimizationStatus::MaxIterations);
    if (result) result->iterations = iterations;
    return result;
}

std::unique_ptr<IPortfolioOptimizer> RiskParityOptimizer::clone() const {
    return std::make_unique<RiskParityOptimizer>(cfg_);
}

// ---------------------------------------------------------------------------
// Minimum variance
// ---------------------------------------------------------------------------

Result<OptimizationResult> MinimumVarianceOptimizer::optimize(
    const OptimizationInput& input) const {
    if (auto ok = input.validate(); !ok) return fail(ok.error());
    if (input.covariance.empty()) {
        return fail(bad("minimum variance needs a covariance matrix"));
    }

    ObjectiveConfig objective_cfg = cfg_.objective;
    objective_cfg.kind = ObjectiveKind::MinimizeVariance;
    const Objective objective{objective_cfg};
    if (auto ok = objective.check_requirements(input); !ok) return fail(ok.error());

    return ascend(input, objective, initial_weights(input));
}

std::unique_ptr<IPortfolioOptimizer> MinimumVarianceOptimizer::clone() const {
    return std::make_unique<MinimumVarianceOptimizer>(cfg_);
}

// ---------------------------------------------------------------------------
// Mean variance
// ---------------------------------------------------------------------------

Result<OptimizationResult> MeanVarianceOptimizer::optimize(const OptimizationInput& input) const {
    if (auto ok = input.validate(); !ok) return fail(ok.error());

    ObjectiveConfig objective_cfg = cfg_.objective;
    objective_cfg.kind = ObjectiveKind::MaximizeReturn;
    const Objective objective{objective_cfg};
    if (auto ok = objective.check_requirements(input); !ok) return fail(ok.error());

    return ascend(input, objective, initial_weights(input));
}

std::unique_ptr<IPortfolioOptimizer> MeanVarianceOptimizer::clone() const {
    return std::make_unique<MeanVarianceOptimizer>(cfg_);
}

// ---------------------------------------------------------------------------
// Maximum Sharpe
// ---------------------------------------------------------------------------

Result<OptimizationResult> MaximumSharpeOptimizer::optimize(const OptimizationInput& input) const {
    if (auto ok = input.validate(); !ok) return fail(ok.error());

    ObjectiveConfig objective_cfg = cfg_.objective;
    objective_cfg.kind = ObjectiveKind::MaximizeSharpe;
    objective_cfg.risk_free_rate = cfg_.risk_free_rate;
    const Objective objective{objective_cfg};
    if (auto ok = objective.check_requirements(input); !ok) return fail(ok.error());

    return ascend(input, objective, initial_weights(input));
}

std::unique_ptr<IPortfolioOptimizer> MaximumSharpeOptimizer::clone() const {
    return std::make_unique<MaximumSharpeOptimizer>(cfg_);
}

// ---------------------------------------------------------------------------
// Kelly
// ---------------------------------------------------------------------------

Result<OptimizationResult> KellyOptimizer::optimize(const OptimizationInput& input) const {
    if (auto ok = input.validate(); !ok) return fail(ok.error());

    ObjectiveConfig objective_cfg = cfg_.objective;
    objective_cfg.kind = ObjectiveKind::Kelly;
    const Objective objective{objective_cfg};
    if (auto ok = objective.check_requirements(input); !ok) return fail(ok.error());

    auto result = ascend(input, objective, initial_weights(input));
    if (result) {
        result->detail = "fractional Kelly at " + std::to_string(objective_cfg.kelly_fraction) +
                         " of full; full Kelly assumes the edge is exact, which it "
                         "never is";
    }
    return result;
}

std::unique_ptr<IPortfolioOptimizer> KellyOptimizer::clone() const {
    return std::make_unique<KellyOptimizer>(cfg_);
}

// ---------------------------------------------------------------------------
// Target volatility
// ---------------------------------------------------------------------------

Result<OptimizationResult> TargetVolatilityOptimizer::optimize(
    const OptimizationInput& input) const {
    if (auto ok = input.validate(); !ok) return fail(ok.error());
    if (input.covariance.empty() && input.volatilities.empty()) {
        return fail(bad("volatility targeting needs a covariance matrix or volatilities"));
    }
    if (!(cfg_.target_volatility > 0.0)) {
        return fail(bad("target volatility must be positive"));
    }

    // Start from risk parity, then SCALE to hit the target. Two separable
    // decisions -- what to hold and how much of it -- and keeping them separate
    // means the composition does not change when the target does.
    auto base = RiskParityOptimizer{cfg_}.optimize(input);
    if (!base) return base;

    const double achieved = base->expected_volatility;
    if (achieved <= 1e-12) {
        base->detail =
            "base portfolio has no estimated volatility; leaving it unscaled rather "
            "than dividing by zero";
        return base;
    }

    const double scale = cfg_.target_volatility / achieved;
    std::vector<double> w = base->weights;
    for (double& x : w) x *= scale;

    auto result = complete(std::move(w), input, OptimizationStatus::Optimal);
    if (result) {
        result->detail = "scaled a risk-parity base by " + std::to_string(scale) + " to target " +
                         std::to_string(cfg_.target_volatility) + " volatility";
    }
    return result;
}

std::unique_ptr<IPortfolioOptimizer> TargetVolatilityOptimizer::clone() const {
    return std::make_unique<TargetVolatilityOptimizer>(cfg_);
}

// ---------------------------------------------------------------------------
// Signal weighted
// ---------------------------------------------------------------------------

Result<OptimizationResult> SignalWeightedOptimizer::optimize(const OptimizationInput& input) const {
    if (auto ok = input.validate(); !ok) return fail(ok.error());
    const std::size_t n = input.size();

    if (input.signals.size() != n) {
        return fail(bad("signal-weighted optimization needs one signal per instrument"));
    }

    double total_magnitude = 0.0;
    for (const double s : input.signals) total_magnitude += std::abs(s);

    if (total_magnitude <= 0.0) {
        // Every signal is zero: that is a genuine flat view, and holding
        // nothing is the correct expression of it.
        std::vector<double> flat(n, 0.0);
        auto result = complete(std::move(flat), input, OptimizationStatus::Optimal);
        if (result) {
            result->detail =
                "all signals were zero; holding no position is the correct "
                "expression of a flat view";
        }
        return result;
    }

    const double budget =
        cfg_.constraints.max_gross_leverage * (1.0 - cfg_.constraints.cash_reserve);
    std::vector<double> w(n, 0.0);
    for (std::size_t i = 0; i < n; ++i) {
        // Signed and proportional: the sign carries direction, the magnitude
        // carries conviction.
        w[i] = input.signals[i] / total_magnitude * budget;
    }
    return complete(std::move(w), input, OptimizationStatus::Optimal);
}

std::unique_ptr<IPortfolioOptimizer> SignalWeightedOptimizer::clone() const {
    return std::make_unique<SignalWeightedOptimizer>(cfg_);
}

// ---------------------------------------------------------------------------
// Registry
// ---------------------------------------------------------------------------

Result<bool> OptimizerRegistry::register_optimizer(std::string name,
                                                   std::unique_ptr<IPortfolioOptimizer> optimizer) {
    if (name.empty()) return fail(bad("optimizer name cannot be empty"));
    if (optimizer == nullptr) return fail(bad("optimizer is null"));
    if (optimizers_.contains(name)) {
        return fail(bad("optimizer already registered: " + name));
    }
    optimizers_.emplace(std::move(name), std::move(optimizer));
    return true;
}

Result<std::unique_ptr<IPortfolioOptimizer>> OptimizerRegistry::create(
    std::string_view name) const {
    const auto it = optimizers_.find(name);
    if (it == optimizers_.end()) {
        std::string msg = "unknown optimizer '" + std::string{name} + "'. Registered: ";
        bool first = true;
        for (const auto& [k, v] : optimizers_) {
            if (!first) msg += ", ";
            first = false;
            msg += k;
        }
        if (optimizers_.empty()) msg += "(none)";
        return fail(make_error(ErrorCode::NotFound, std::move(msg)));
    }
    return it->second->clone();
}

bool OptimizerRegistry::contains(std::string_view name) const noexcept {
    return optimizers_.find(name) != optimizers_.end();
}

std::vector<std::string_view> OptimizerRegistry::names() const {
    std::vector<std::string_view> out;
    out.reserve(optimizers_.size());
    for (const auto& [k, v] : optimizers_) out.emplace_back(k);
    return out;
}

Result<OptimizerRegistry> OptimizerRegistry::with_defaults(OptimizerConfig cfg) {
    OptimizerRegistry reg;
    const auto add = [&reg](std::string name,
                            std::unique_ptr<IPortfolioOptimizer> o) -> Result<bool> {
        return reg.register_optimizer(std::move(name), std::move(o));
    };

    if (auto r = add("equal_weight", std::make_unique<EqualWeightOptimizer>(cfg)); !r) {
        return fail(r.error());
    }
    if (auto r = add("inverse_volatility", std::make_unique<InverseVolatilityOptimizer>(cfg)); !r) {
        return fail(r.error());
    }
    if (auto r = add("risk_parity", std::make_unique<RiskParityOptimizer>(cfg)); !r) {
        return fail(r.error());
    }
    if (auto r = add("minimum_variance", std::make_unique<MinimumVarianceOptimizer>(cfg)); !r) {
        return fail(r.error());
    }
    if (auto r = add("mean_variance", std::make_unique<MeanVarianceOptimizer>(cfg)); !r) {
        return fail(r.error());
    }
    if (auto r = add("max_sharpe", std::make_unique<MaximumSharpeOptimizer>(cfg)); !r) {
        return fail(r.error());
    }
    if (auto r = add("kelly", std::make_unique<KellyOptimizer>(cfg)); !r) {
        return fail(r.error());
    }
    if (auto r = add("target_volatility", std::make_unique<TargetVolatilityOptimizer>(cfg)); !r) {
        return fail(r.error());
    }
    if (auto r = add("signal_weighted", std::make_unique<SignalWeightedOptimizer>(cfg)); !r) {
        return fail(r.error());
    }
    return reg;
}

}  // namespace ptl::optimization
