#pragma once

/// \file constraints.hpp
/// Portfolio constraints.
///
/// Constraints are applied as a PROJECTION onto the feasible set, not as a
/// penalty in the objective. A penalty produces weights that violate a limit by
/// "only a little", and a limit that can be violated a little is not a limit --
/// the risk manager downstream will reject the resulting orders and the
/// optimizer will have wasted the turnover budget deciding on them.
///
/// Order matters and is fixed: normalisation, then bounds, then group
/// exposures, then leverage. Each step can undo part of the previous one, so
/// the sequence is iterated to a fixed point rather than applied once.

#include <cstdint>
#include <map>
#include <span>
#include <string>
#include <vector>

#include "ptl/core/result.hpp"
#include "ptl/optimization/optimizer_types.hpp"

namespace ptl::optimization {

/// What the portfolio may hold.
enum class DirectionMode : std::uint8_t {
    LongOnly,
    LongShort,
    /// Long and short legs must have equal notional.
    DollarNeutral,
};

[[nodiscard]] std::string_view to_string(DirectionMode) noexcept;

struct ConstraintSet {
    DirectionMode direction{DirectionMode::LongShort};

    /// Absolute weight cap per instrument.
    double max_position = 0.10;
    /// Signed floor. For long-only this is zero; for long-short, typically
    /// -max_position.
    double min_position = -0.10;

    /// Sum of |w|.
    double max_gross_leverage = 1.0;
    /// |sum of w|.
    double max_net_leverage = 1.0;

    /// Sum of |w| within a sector.
    double max_sector_exposure = 0.30;

    /// Portfolio beta must lie within +/- this of zero. Only applied when betas
    /// are supplied.
    double max_abs_beta = 1e9;
    bool beta_neutral = false;

    /// Sum of |w - w_prev|. Zero disables the constraint.
    double max_turnover = 0.0;

    /// Weights below this magnitude are set to zero. A one-basis-point position
    /// costs a full round trip in commission and contributes nothing.
    double min_trade_size = 0.0;

    /// Fraction of equity held back. Gross exposure is capped at
    /// (1 - cash_reserve) * max_gross_leverage.
    double cash_reserve = 0.0;

    /// Largest share of gross exposure any one position may take. Distinct from
    /// max_position, which is a fraction of EQUITY: at low leverage a position
    /// can be small against equity and still be the entire book.
    double max_concentration = 1.0;

    [[nodiscard]] Result<bool> validate() const;
    [[nodiscard]] std::string describe() const;

    /// Long-only defaults.
    [[nodiscard]] static ConstraintSet long_only(double max_position = 0.10);
    /// Market-neutral defaults.
    [[nodiscard]] static ConstraintSet market_neutral(double max_position = 0.05);
};

/// Applies a constraint set to a weight vector.
///
/// Records which constraints bound, so a result can report why it is not the
/// unconstrained optimum.
class ConstraintProjector {
public:
    explicit ConstraintProjector(ConstraintSet constraints)
        : constraints_(std::move(constraints)) {}

    /// Project onto the feasible set.
    ///
    /// \param weights modified in place.
    /// \param binding receives the names of constraints that bound.
    [[nodiscard]] Result<bool> project(std::vector<double>& weights, const OptimizationInput&,
                                       std::vector<std::string>* binding = nullptr) const;

    /// Whether a weight vector already satisfies every constraint.
    [[nodiscard]] bool feasible(std::span<const double> weights, const OptimizationInput&,
                                double tolerance = 1e-6) const;

    [[nodiscard]] const ConstraintSet& constraints() const noexcept { return constraints_; }

private:
    ConstraintSet constraints_;
};

}  // namespace ptl::optimization
