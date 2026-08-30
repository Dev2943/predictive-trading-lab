#pragma once

/// \file optimizer_types.hpp
/// Vocabulary for portfolio optimization.
///
/// An optimizer answers one question: given forecasts and a risk model, WHAT
/// SHOULD THE PORTFOLIO OWN? It returns weights, never orders. Turning weights
/// into trades is the rebalance engine's job (Phase 7), and turning trades into
/// child orders is the execution layer's (Phase 9). Keeping those three
/// separate is what stops position sizing happening twice.
///
/// NO OPTIMIZER MAY PRODUCE NaN WEIGHTS. Every result is validated before it
/// leaves the optimizer, because a NaN weight propagates silently through
/// rebalancing into an order quantity, and the first sign of trouble is a
/// rejected order with an unreadable size.

#include <cstdint>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "ptl/core/result.hpp"
#include "ptl/core/types.hpp"

namespace ptl::optimization {

/// A small dense symmetric matrix.
///
/// Deliberately NOT ptl::models::Matrix: that type lives behind the Eigen gate
/// (Phase 6), and depending on it would make the entire optimizer unavailable
/// wherever Eigen is not installed. Optimization universes are tens of names,
/// so a plain row-major vector is both sufficient and honest about the scale.
class SymmetricMatrix {
public:
    SymmetricMatrix() = default;
    explicit SymmetricMatrix(std::size_t n) : n_(n), data_(n * n, 0.0) {}

    [[nodiscard]] std::size_t size() const noexcept { return n_; }
    [[nodiscard]] bool empty() const noexcept { return n_ == 0; }

    [[nodiscard]] double& at(std::size_t i, std::size_t j) noexcept { return data_[i * n_ + j]; }
    [[nodiscard]] double at(std::size_t i, std::size_t j) const noexcept {
        return data_[i * n_ + j];
    }

    /// Write both triangles at once, so the matrix cannot drift out of symmetry
    /// through a one-sided assignment.
    void set_symmetric(std::size_t i, std::size_t j, double value) noexcept {
        data_[i * n_ + j] = value;
        data_[j * n_ + i] = value;
    }

    [[nodiscard]] std::vector<double> diagonal() const;
    [[nodiscard]] bool all_finite() const noexcept;
    [[nodiscard]] bool is_symmetric(double tolerance = 1e-9) const noexcept;

    /// w' Σ w. The portfolio variance, and the only quadratic form the
    /// optimizers need.
    [[nodiscard]] double quadratic_form(std::span<const double> w) const noexcept;

    /// Σ w. Marginal contributions before scaling.
    [[nodiscard]] std::vector<double> multiply(std::span<const double> w) const;

    [[nodiscard]] const std::vector<double>& raw() const noexcept { return data_; }

private:
    std::size_t n_ = 0;
    std::vector<double> data_;
};

/// What the optimizer is given.
///
/// Every series is HISTORICAL. There is no field here that could hold a future
/// return, and the leakage tests assert that the covariance estimators consume
/// only observations at or before the decision instant.
struct OptimizationInput {
    Timestamp as_of{kNoTimestamp};
    /// Universe, in a fixed order. Weights come back in the same order.
    std::vector<InstrumentId> instruments;

    /// Expected returns per instrument, over the forecast horizon. Empty means
    /// no view, and the optimizers that need one refuse rather than assuming
    /// zero -- a zero forecast is a real opinion, not an absence of one.
    std::vector<double> expected_returns;

    /// Risk model. Empty means no covariance, which several optimizers can
    /// still work without (equal weight, inverse volatility).
    SymmetricMatrix covariance;

    /// Per-instrument volatility, when a full covariance is unavailable.
    std::vector<double> volatilities;

    /// Current weights, for turnover constraints. Empty means a fresh book.
    std::vector<double> current_weights;

    /// Signal strengths, for the signal-weighted optimizer.
    std::vector<double> signals;

    /// Sector id per instrument; -1 is ungrouped.
    std::vector<std::int32_t> sectors;

    /// Beta to the benchmark per instrument, for beta neutrality.
    std::vector<double> betas;

    [[nodiscard]] std::size_t size() const noexcept { return instruments.size(); }

    /// Structural validation: sizes agree, values are finite, universe is
    /// non-empty and free of duplicates.
    [[nodiscard]] Result<bool> validate() const;
};

/// Why an optimization ended as it did.
enum class OptimizationStatus : std::uint8_t {
    Optimal,
    /// A constraint bound the solution; the weights are feasible but the
    /// objective is not at its unconstrained optimum.
    ConstrainedOptimal,
    /// The iterative solver hit its cap. The weights are the best found, and
    /// saying so is better than presenting them as optimal.
    MaxIterations,
    /// The problem had no feasible point; the fallback was used.
    Infeasible,
    /// The risk model was unusable (singular, non-PSD beyond repair).
    SingularRiskModel,
    Failed,
};

[[nodiscard]] std::string_view to_string(OptimizationStatus) noexcept;

/// The optimizer's answer.
struct OptimizationResult {
    OptimizationStatus status{OptimizationStatus::Failed};
    /// Weights as fractions of equity, in universe order. Signed: negative is
    /// short.
    std::vector<double> weights;

    double expected_return = 0.0;
    double expected_variance = 0.0;
    double expected_volatility = 0.0;
    double sharpe = 0.0;
    double gross_exposure = 0.0;
    double net_exposure = 0.0;
    /// Fraction of equity left uninvested. Explicit, because a set of weights
    /// summing to less than one is a cash position and not an error.
    double cash_weight = 0.0;
    double turnover = 0.0;

    std::size_t iterations = 0;
    /// Constraints that bound the solution, in the order they were applied.
    std::vector<std::string> binding_constraints;
    std::string detail;

    [[nodiscard]] bool usable() const noexcept {
        return status == OptimizationStatus::Optimal ||
               status == OptimizationStatus::ConstrainedOptimal ||
               status == OptimizationStatus::MaxIterations;
    }
    [[nodiscard]] std::string describe() const;
};

/// Populate the derived statistics on a result from its weights.
///
/// One place, so gross, net, cash and variance can never disagree with the
/// weights they describe.
void finalize_result(OptimizationResult&, const OptimizationInput&);

/// Every weight finite, and no NaN anywhere. Called before any result leaves an
/// optimizer.
[[nodiscard]] Result<bool> validate_weights(std::span<const double> weights);

}  // namespace ptl::optimization
