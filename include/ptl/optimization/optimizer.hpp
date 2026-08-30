#pragma once

/// \file optimizer.hpp
/// Portfolio optimizers.
///
/// ONE INTERFACE, NINE OPTIMIZERS, NO SWITCH. Selection is holding a different
/// pointer, exactly as with the Phase 9 execution algorithms. Adding a tenth
/// means adding a class.
///
/// EVERY OPTIMIZER RETURNS WEIGHTS, NEVER ORDERS. The rebalance engine turns
/// weights into trades and the execution layer turns trades into child orders.
/// Keeping those separate is what stops position sizing happening twice --
/// which is precisely what would occur if an optimizer returned quantities and
/// the Phase 7 sizer then scaled them again.

#include <map>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "ptl/core/result.hpp"
#include "ptl/optimization/constraints.hpp"
#include "ptl/optimization/covariance.hpp"
#include "ptl/optimization/objective.hpp"
#include "ptl/optimization/optimizer_types.hpp"

namespace ptl::optimization {

struct OptimizerConfig {
    ConstraintSet constraints;
    ObjectiveConfig objective;

    /// Iteration cap for the gradient solvers.
    std::size_t max_iterations = 500;
    /// Convergence threshold on the weight change per iteration.
    double tolerance = 1e-10;
    /// Initial step size for projected gradient ascent.
    double step_size = 0.05;

    /// Target annualised volatility, for the volatility-targeting optimizer.
    double target_volatility = 0.10;

    /// Risk-free rate per period.
    double risk_free_rate = 0.0;
};

class IPortfolioOptimizer {
public:
    IPortfolioOptimizer() = default;
    virtual ~IPortfolioOptimizer() = default;
    IPortfolioOptimizer(const IPortfolioOptimizer&) = delete;
    IPortfolioOptimizer& operator=(const IPortfolioOptimizer&) = delete;

protected:
    IPortfolioOptimizer(IPortfolioOptimizer&&) = default;
    IPortfolioOptimizer& operator=(IPortfolioOptimizer&&) = default;

public:
    [[nodiscard]] virtual std::string_view name() const noexcept = 0;

    /// \returns weights in universe order. Never NaN: every result is validated
    ///          before it is returned.
    [[nodiscard]] virtual Result<OptimizationResult> optimize(const OptimizationInput&) const = 0;

    [[nodiscard]] virtual std::unique_ptr<IPortfolioOptimizer> clone() const = 0;
};

/// Shared validation, projection and finalisation.
class OptimizerBase : public IPortfolioOptimizer {
public:
    explicit OptimizerBase(OptimizerConfig cfg = {}) : cfg_(std::move(cfg)) {}

    [[nodiscard]] const OptimizerConfig& config() const noexcept { return cfg_; }

protected:
    /// Validate the input, project the weights, finalise the statistics and
    /// check for NaN. Every optimizer ends here, so the guarantees hold for all
    /// of them rather than for whichever ones remembered.
    [[nodiscard]] Result<OptimizationResult> complete(std::vector<double> weights,
                                                      const OptimizationInput&,
                                                      OptimizationStatus) const;

    /// Projected gradient ascent, shared by the objective-driven optimizers.
    [[nodiscard]] Result<OptimizationResult> ascend(const OptimizationInput&, const Objective&,
                                                    std::vector<double> start) const;

    /// A feasible starting point: equal weight, projected.
    [[nodiscard]] std::vector<double> initial_weights(const OptimizationInput&) const;

    OptimizerConfig cfg_;
};

/// 1/N. The benchmark every other optimizer must beat after costs.
class EqualWeightOptimizer final : public OptimizerBase {
public:
    using OptimizerBase::OptimizerBase;
    [[nodiscard]] std::string_view name() const noexcept override { return "equal_weight"; }
    [[nodiscard]] Result<OptimizationResult> optimize(const OptimizationInput&) const override;
    [[nodiscard]] std::unique_ptr<IPortfolioOptimizer> clone() const override;
};

/// Weight proportional to 1/volatility.
class InverseVolatilityOptimizer final : public OptimizerBase {
public:
    using OptimizerBase::OptimizerBase;
    [[nodiscard]] std::string_view name() const noexcept override { return "inverse_volatility"; }
    [[nodiscard]] Result<OptimizationResult> optimize(const OptimizationInput&) const override;
    [[nodiscard]] std::unique_ptr<IPortfolioOptimizer> clone() const override;
};

/// Equal RISK contribution, which differs from inverse volatility whenever the
/// assets are correlated.
class RiskParityOptimizer final : public OptimizerBase {
public:
    using OptimizerBase::OptimizerBase;
    [[nodiscard]] std::string_view name() const noexcept override { return "risk_parity"; }
    [[nodiscard]] Result<OptimizationResult> optimize(const OptimizationInput&) const override;
    [[nodiscard]] std::unique_ptr<IPortfolioOptimizer> clone() const override;

    /// Each position's share of total portfolio risk. Exposed because it is the
    /// property the optimizer exists to equalise and deserves a direct test.
    [[nodiscard]] static std::vector<double> risk_contributions(std::span<const double> weights,
                                                                const SymmetricMatrix&);
};

/// Minimum variance, ignoring expected returns entirely.
class MinimumVarianceOptimizer final : public OptimizerBase {
public:
    using OptimizerBase::OptimizerBase;
    [[nodiscard]] std::string_view name() const noexcept override { return "minimum_variance"; }
    [[nodiscard]] Result<OptimizationResult> optimize(const OptimizationInput&) const override;
    [[nodiscard]] std::unique_ptr<IPortfolioOptimizer> clone() const override;
};

/// Markowitz mean-variance.
class MeanVarianceOptimizer final : public OptimizerBase {
public:
    using OptimizerBase::OptimizerBase;
    [[nodiscard]] std::string_view name() const noexcept override { return "mean_variance"; }
    [[nodiscard]] Result<OptimizationResult> optimize(const OptimizationInput&) const override;
    [[nodiscard]] std::unique_ptr<IPortfolioOptimizer> clone() const override;
};

/// Maximum Sharpe, the tangency portfolio.
class MaximumSharpeOptimizer final : public OptimizerBase {
public:
    using OptimizerBase::OptimizerBase;
    [[nodiscard]] std::string_view name() const noexcept override { return "max_sharpe"; }
    [[nodiscard]] Result<OptimizationResult> optimize(const OptimizationInput&) const override;
    [[nodiscard]] std::unique_ptr<IPortfolioOptimizer> clone() const override;
};

/// Fractional Kelly.
class KellyOptimizer final : public OptimizerBase {
public:
    using OptimizerBase::OptimizerBase;
    [[nodiscard]] std::string_view name() const noexcept override { return "kelly"; }
    [[nodiscard]] Result<OptimizationResult> optimize(const OptimizationInput&) const override;
    [[nodiscard]] std::unique_ptr<IPortfolioOptimizer> clone() const override;
};

/// Scales any base allocation to hit a target volatility.
class TargetVolatilityOptimizer final : public OptimizerBase {
public:
    using OptimizerBase::OptimizerBase;
    [[nodiscard]] std::string_view name() const noexcept override { return "target_volatility"; }
    [[nodiscard]] Result<OptimizationResult> optimize(const OptimizationInput&) const override;
    [[nodiscard]] std::unique_ptr<IPortfolioOptimizer> clone() const override;
};

/// Weights proportional to signal strength.
///
/// The direct bridge from Phase 7: signals in, weights out, with no covariance
/// required. Useful when the risk model is untrusted but the ranking is not.
class SignalWeightedOptimizer final : public OptimizerBase {
public:
    using OptimizerBase::OptimizerBase;
    [[nodiscard]] std::string_view name() const noexcept override { return "signal_weighted"; }
    [[nodiscard]] Result<OptimizationResult> optimize(const OptimizationInput&) const override;
    [[nodiscard]] std::unique_ptr<IPortfolioOptimizer> clone() const override;
};

/// Name-to-optimizer registry. Explicit, like the Phase 9 algorithm registry.
class OptimizerRegistry {
public:
    [[nodiscard]] Result<bool> register_optimizer(std::string name,
                                                  std::unique_ptr<IPortfolioOptimizer>);
    [[nodiscard]] Result<std::unique_ptr<IPortfolioOptimizer>> create(std::string_view) const;
    [[nodiscard]] bool contains(std::string_view) const noexcept;
    [[nodiscard]] std::vector<std::string_view> names() const;
    [[nodiscard]] std::size_t size() const noexcept { return optimizers_.size(); }

    /// All nine built-ins.
    [[nodiscard]] static Result<OptimizerRegistry> with_defaults(OptimizerConfig = {});

private:
    std::map<std::string, std::unique_ptr<IPortfolioOptimizer>, std::less<>> optimizers_;
};

}  // namespace ptl::optimization
