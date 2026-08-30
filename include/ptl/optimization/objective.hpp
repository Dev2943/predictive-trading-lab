#pragma once

/// \file objective.hpp
/// Optimization objectives.
///
/// An objective scores a weight vector. Separating it from the optimizer means
/// the same projected-gradient solver drives every objective, so a bug in the
/// search is fixed once rather than in nine places.
///
/// EVERY OBJECTIVE IS A MAXIMISATION. Minimum-variance is expressed as
/// maximising negative variance. Mixing conventions is how a sign error hides
/// for months, producing a portfolio that reliably picks the worst available
/// allocation.

#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

#include "ptl/core/result.hpp"
#include "ptl/optimization/optimizer_types.hpp"

namespace ptl::optimization {

enum class ObjectiveKind : std::uint8_t {
    MaximizeReturn,
    MaximizeSharpe,
    MinimizeVariance,
    MinimizeTrackingError,
    RiskParity,
    Kelly,
};

[[nodiscard]] std::string_view to_string(ObjectiveKind) noexcept;

struct ObjectiveConfig {
    ObjectiveKind kind{ObjectiveKind::MaximizeSharpe};
    /// Risk aversion for the mean-variance trade-off. Higher means more
    /// weight on variance.
    double risk_aversion = 1.0;
    /// Risk-free rate per period, for Sharpe.
    double risk_free_rate = 0.0;
    /// Benchmark weights for tracking error. Empty means the objective is
    /// unavailable and the optimizer refuses rather than tracking cash.
    std::vector<double> benchmark_weights;
    /// Fraction of full Kelly. Full Kelly assumes the edge is exact; it never
    /// is (the same reasoning as Phase 7 sizing).
    double kelly_fraction = 0.25;
};

/// Scores weights and supplies a gradient.
///
/// The gradient is ANALYTIC rather than numerical. A finite-difference gradient
/// on a near-singular covariance is dominated by rounding, and the solver then
/// wanders instead of converging.
class Objective {
public:
    explicit Objective(ObjectiveConfig cfg = {}) : cfg_(cfg) {}

    /// Higher is better, always.
    [[nodiscard]] Result<double> value(std::span<const double> weights,
                                       const OptimizationInput&) const;

    /// d(value)/dw.
    [[nodiscard]] Result<std::vector<double>> gradient(std::span<const double> weights,
                                                       const OptimizationInput&) const;

    /// What this objective requires of its input, checked before a solve so the
    /// failure names the missing ingredient rather than surfacing as a NaN.
    [[nodiscard]] Result<bool> check_requirements(const OptimizationInput&) const;

    [[nodiscard]] const ObjectiveConfig& config() const noexcept { return cfg_; }

private:
    ObjectiveConfig cfg_;
};

}  // namespace ptl::optimization
