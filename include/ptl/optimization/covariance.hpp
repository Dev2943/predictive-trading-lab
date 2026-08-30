#pragma once

/// \file covariance.hpp
/// Covariance estimation.
///
/// THE LEAKAGE SURFACE OF THE WHOLE PHASE. A covariance estimated over a window
/// that includes the period being traded is the most respectable-looking
/// lookahead there is: the numbers are plausible, the code is short, and the
/// resulting backtest is wrong in a way no summary statistic reveals.
///
/// Every estimator here consumes a matrix of HISTORICAL observations and is
/// given no access to anything else. The leakage tests assert that an estimator
/// fed observations up to time T produces an identical matrix whether or not
/// later observations exist in the caller's buffer.
///
/// PSD ENFORCEMENT. A sample covariance from fewer observations than assets is
/// singular by construction, and a shrunk or repaired matrix is not a
/// nice-to-have -- an optimizer inverting a singular matrix produces enormous
/// offsetting weights that look like a brilliant hedge and are numerical noise.

#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

#include "ptl/core/result.hpp"
#include "ptl/optimization/optimizer_types.hpp"

namespace ptl::optimization {

enum class CovarianceMethod : std::uint8_t {
    /// Equal-weighted sample covariance over the whole window.
    Sample,
    /// Sample covariance over a trailing window of fixed length.
    Rolling,
    /// Exponentially weighted, so recent observations dominate.
    Ewma,
    /// Ledoit-Wolf style shrinkage toward a scaled identity.
    Shrinkage,
    /// Diagonal only: variances on the diagonal, zero elsewhere. The honest
    /// choice when there is too little data to estimate correlations.
    Identity,
};

[[nodiscard]] std::string_view to_string(CovarianceMethod) noexcept;

struct CovarianceConfig {
    CovarianceMethod method{CovarianceMethod::Shrinkage};

    /// Observations in the trailing window. Zero uses everything supplied.
    std::size_t window = 0;

    /// EWMA decay. 0.94 is the RiskMetrics daily convention.
    double ewma_lambda = 0.94;

    /// Shrinkage intensity toward the target, in [0, 1]. Negative selects the
    /// analytic Ledoit-Wolf estimate instead of a fixed value.
    double shrinkage_intensity = -1.0;

    /// Minimum eigenvalue after repair, as a fraction of the mean variance.
    /// Zero eigenvalues make a matrix singular; nudging them to a small
    /// positive floor is what makes inversion stable.
    double min_eigenvalue_ratio = 1e-8;

    /// Annualisation factor applied to the result. One leaves it per-period.
    double annualization = 1.0;

    /// Refuse when observations are fewer than this multiple of the asset
    /// count. Estimating a 50x50 covariance from 20 observations is not an
    /// estimate, and the caller should know rather than receive noise.
    double min_observations_ratio = 1.5;
};

struct CovarianceDiagnostics {
    std::size_t observations = 0;
    std::size_t assets = 0;
    /// True when observations were too few for the requested method and a
    /// simpler one was substituted. Recorded, never silent.
    bool degraded = false;
    std::string degradation_reason;
    /// Shrinkage actually applied.
    double applied_shrinkage = 0.0;
    /// Whether PSD repair changed the matrix.
    bool psd_repaired = false;
    double smallest_eigenvalue_before = 0.0;

    [[nodiscard]] std::string describe() const;
};

/// Estimates covariance from historical observations.
///
/// \param observations row-major, one ROW PER TIME STEP and one column per
///        asset. Rows must be in chronological order; the estimator uses them
///        all and never looks beyond what it is given.
class CovarianceEstimator {
public:
    explicit CovarianceEstimator(CovarianceConfig cfg = {}) : cfg_(cfg) {}

    [[nodiscard]] Result<SymmetricMatrix> estimate(std::span<const double> observations,
                                                   std::size_t rows, std::size_t assets);

    [[nodiscard]] const CovarianceDiagnostics& diagnostics() const noexcept { return diagnostics_; }
    [[nodiscard]] const CovarianceConfig& config() const noexcept { return cfg_; }

    /// Force a symmetric matrix to be positive semi-definite.
    ///
    /// Jacobi eigendecomposition, floor the eigenvalues, reconstruct. Exposed
    /// because it deserves a direct test rather than only being reachable
    /// through a full estimation.
    [[nodiscard]] static Result<SymmetricMatrix> enforce_psd(const SymmetricMatrix&,
                                                             double min_eigenvalue,
                                                             bool* repaired = nullptr,
                                                             double* smallest = nullptr);

    /// Smallest eigenvalue, by Jacobi rotation. Negative means not PSD.
    [[nodiscard]] static Result<double> smallest_eigenvalue(const SymmetricMatrix&);

    /// Correlation matrix implied by a covariance. Zero-variance assets get a
    /// unit diagonal and zero correlations rather than a division by zero.
    [[nodiscard]] static Result<SymmetricMatrix> to_correlation(const SymmetricMatrix&);

private:
    CovarianceConfig cfg_;
    CovarianceDiagnostics diagnostics_;
};

}  // namespace ptl::optimization
