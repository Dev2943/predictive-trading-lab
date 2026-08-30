#include "ptl/optimization/covariance.hpp"

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
// SymmetricMatrix
// ---------------------------------------------------------------------------

std::vector<double> SymmetricMatrix::diagonal() const {
    std::vector<double> out;
    out.reserve(n_);
    for (std::size_t i = 0; i < n_; ++i) out.push_back(at(i, i));
    return out;
}

bool SymmetricMatrix::all_finite() const noexcept {
    return std::all_of(data_.begin(), data_.end(), [](double v) { return is_finite(v); });
}

bool SymmetricMatrix::is_symmetric(double tolerance) const noexcept {
    for (std::size_t i = 0; i < n_; ++i) {
        for (std::size_t j = i + 1; j < n_; ++j) {
            if (std::abs(at(i, j) - at(j, i)) > tolerance) return false;
        }
    }
    return true;
}

double SymmetricMatrix::quadratic_form(std::span<const double> w) const noexcept {
    if (w.size() != n_ || n_ == 0) return 0.0;
    double total = 0.0;
    for (std::size_t i = 0; i < n_; ++i) {
        double row = 0.0;
        for (std::size_t j = 0; j < n_; ++j) row += at(i, j) * w[j];
        total += w[i] * row;
    }
    // Variance cannot be negative. A tiny negative from rounding on a
    // near-singular matrix is clamped rather than propagated into a sqrt.
    return is_finite(total) ? std::max(0.0, total) : 0.0;
}

std::vector<double> SymmetricMatrix::multiply(std::span<const double> w) const {
    std::vector<double> out(n_, 0.0);
    if (w.size() != n_) return out;
    for (std::size_t i = 0; i < n_; ++i) {
        double row = 0.0;
        for (std::size_t j = 0; j < n_; ++j) row += at(i, j) * w[j];
        out[i] = is_finite(row) ? row : 0.0;
    }
    return out;
}

// ---------------------------------------------------------------------------
// Covariance estimation
// ---------------------------------------------------------------------------

std::string_view to_string(CovarianceMethod m) noexcept {
    switch (m) {
        case CovarianceMethod::Sample:
            return "sample";
        case CovarianceMethod::Rolling:
            return "rolling";
        case CovarianceMethod::Ewma:
            return "ewma";
        case CovarianceMethod::Shrinkage:
            return "shrinkage";
        case CovarianceMethod::Identity:
            return "identity";
    }
    return "unknown";
}

std::string CovarianceDiagnostics::describe() const {
    std::ostringstream ss;
    ss.precision(6);
    ss << std::fixed << "covariance over " << observations << " observations of " << assets
       << " assets";
    if (applied_shrinkage > 0.0) ss << ", shrinkage " << applied_shrinkage;
    if (psd_repaired)
        ss << ", PSD repaired (smallest eigenvalue was " << smallest_eigenvalue_before << ')';
    if (degraded) ss << "\n  DEGRADED: " << degradation_reason;
    return ss.str();
}

Result<double> CovarianceEstimator::smallest_eigenvalue(const SymmetricMatrix& m) {
    if (m.empty()) return fail(bad("cannot take eigenvalues of an empty matrix"));
    if (!m.all_finite()) return fail(bad("matrix contains non-finite entries"));

    // Cyclic Jacobi. Chosen over a power iteration because it yields ALL
    // eigenvalues, which enforce_psd needs anyway, and because it is stable on
    // the small symmetric matrices this module deals in.
    const std::size_t n = m.size();
    SymmetricMatrix a = m;

    for (std::size_t sweep = 0; sweep < 100; ++sweep) {
        double off = 0.0;
        for (std::size_t i = 0; i < n; ++i) {
            for (std::size_t j = i + 1; j < n; ++j) off += a.at(i, j) * a.at(i, j);
        }
        if (off < 1e-22) break;  // converged

        for (std::size_t p = 0; p < n; ++p) {
            for (std::size_t q = p + 1; q < n; ++q) {
                const double apq = a.at(p, q);
                if (std::abs(apq) < 1e-18) continue;
                const double app = a.at(p, p);
                const double aqq = a.at(q, q);

                const double theta = (aqq - app) / (2.0 * apq);
                const double t =
                    std::copysign(1.0, theta) / (std::abs(theta) + std::sqrt(theta * theta + 1.0));
                const double c = 1.0 / std::sqrt(t * t + 1.0);
                const double s = t * c;

                for (std::size_t k = 0; k < n; ++k) {
                    const double akp = a.at(k, p);
                    const double akq = a.at(k, q);
                    a.at(k, p) = c * akp - s * akq;
                    a.at(k, q) = s * akp + c * akq;
                }
                for (std::size_t k = 0; k < n; ++k) {
                    const double apk = a.at(p, k);
                    const double aqk = a.at(q, k);
                    a.at(p, k) = c * apk - s * aqk;
                    a.at(q, k) = s * apk + c * aqk;
                }
            }
        }
    }

    double smallest = a.at(0, 0);
    for (std::size_t i = 1; i < n; ++i) smallest = std::min(smallest, a.at(i, i));
    return is_finite(smallest) ? smallest : 0.0;
}

Result<SymmetricMatrix> CovarianceEstimator::enforce_psd(const SymmetricMatrix& m,
                                                         double min_eigenvalue, bool* repaired,
                                                         double* smallest_out) {
    if (m.empty()) return fail(bad("cannot repair an empty matrix"));
    if (!m.all_finite()) return fail(bad("matrix contains non-finite entries"));

    const std::size_t n = m.size();
    SymmetricMatrix a = m;
    // Eigenvector accumulator, initialised to the identity.
    SymmetricMatrix v{n};
    for (std::size_t i = 0; i < n; ++i) v.at(i, i) = 1.0;

    for (std::size_t sweep = 0; sweep < 100; ++sweep) {
        double off = 0.0;
        for (std::size_t i = 0; i < n; ++i) {
            for (std::size_t j = i + 1; j < n; ++j) off += a.at(i, j) * a.at(i, j);
        }
        if (off < 1e-22) break;

        for (std::size_t p = 0; p < n; ++p) {
            for (std::size_t q = p + 1; q < n; ++q) {
                const double apq = a.at(p, q);
                if (std::abs(apq) < 1e-18) continue;
                const double theta = (a.at(q, q) - a.at(p, p)) / (2.0 * apq);
                const double t =
                    std::copysign(1.0, theta) / (std::abs(theta) + std::sqrt(theta * theta + 1.0));
                const double c = 1.0 / std::sqrt(t * t + 1.0);
                const double s = t * c;

                for (std::size_t k = 0; k < n; ++k) {
                    const double akp = a.at(k, p);
                    const double akq = a.at(k, q);
                    a.at(k, p) = c * akp - s * akq;
                    a.at(k, q) = s * akp + c * akq;
                }
                for (std::size_t k = 0; k < n; ++k) {
                    const double apk = a.at(p, k);
                    const double aqk = a.at(q, k);
                    a.at(p, k) = c * apk - s * aqk;
                    a.at(q, k) = s * apk + c * aqk;
                }
                for (std::size_t k = 0; k < n; ++k) {
                    const double vkp = v.at(k, p);
                    const double vkq = v.at(k, q);
                    v.at(k, p) = c * vkp - s * vkq;
                    v.at(k, q) = s * vkp + c * vkq;
                }
            }
        }
    }

    std::vector<double> eigenvalues(n, 0.0);
    double smallest = a.at(0, 0);
    for (std::size_t i = 0; i < n; ++i) {
        eigenvalues[i] = a.at(i, i);
        smallest = std::min(smallest, eigenvalues[i]);
    }
    if (smallest_out != nullptr) *smallest_out = smallest;

    const bool needs_repair = smallest < min_eigenvalue;
    if (repaired != nullptr) *repaired = needs_repair;
    if (!needs_repair) return m;

    // Floor the eigenvalues and rebuild V diag(λ) V'. An optimizer inverting a
    // singular matrix produces enormous offsetting weights that look like a
    // brilliant hedge and are numerical noise; this is what prevents that.
    for (auto& lambda : eigenvalues) lambda = std::max(lambda, min_eigenvalue);

    SymmetricMatrix out{n};
    for (std::size_t i = 0; i < n; ++i) {
        for (std::size_t j = i; j < n; ++j) {
            double sum = 0.0;
            for (std::size_t k = 0; k < n; ++k) {
                sum += v.at(i, k) * eigenvalues[k] * v.at(j, k);
            }
            out.set_symmetric(i, j, is_finite(sum) ? sum : (i == j ? min_eigenvalue : 0.0));
        }
    }
    return out;
}

Result<SymmetricMatrix> CovarianceEstimator::to_correlation(const SymmetricMatrix& cov) {
    if (cov.empty()) return fail(bad("cannot correlate an empty matrix"));
    const std::size_t n = cov.size();
    SymmetricMatrix out{n};

    std::vector<double> sd(n, 0.0);
    for (std::size_t i = 0; i < n; ++i) sd[i] = std::sqrt(std::max(0.0, cov.at(i, i)));

    for (std::size_t i = 0; i < n; ++i) {
        for (std::size_t j = i; j < n; ++j) {
            if (i == j) {
                // The diagonal of a correlation matrix is 1 by definition,
                // including for a zero-variance asset: a constant series is
                // perfectly correlated with itself, and writing 1 here stops
                // a division by zero poisoning the whole matrix.
                out.set_symmetric(i, j, 1.0);
                continue;
            }
            const double denominator = sd[i] * sd[j];
            const double rho = denominator > 0.0 ? cov.at(i, j) / denominator : 0.0;
            out.set_symmetric(i, j, is_finite(rho) ? std::clamp(rho, -1.0, 1.0) : 0.0);
        }
    }
    return out;
}

Result<SymmetricMatrix> CovarianceEstimator::estimate(std::span<const double> observations,
                                                      std::size_t rows, std::size_t assets) {
    diagnostics_ = CovarianceDiagnostics{};
    diagnostics_.assets = assets;

    if (assets == 0) return fail(bad("covariance needs at least one asset"));
    if (rows == 0) return fail(bad("covariance needs at least one observation"));
    if (observations.size() != rows * assets) {
        return fail(bad("observation buffer is " + std::to_string(observations.size()) + " but " +
                        std::to_string(rows) + "x" + std::to_string(assets) + " was declared"));
    }

    // Trailing window. Taking the LAST `window` rows is what makes this an
    // estimate as of the final observation rather than a centred one.
    std::size_t first_row = 0;
    if (cfg_.window > 0 && rows > cfg_.window) first_row = rows - cfg_.window;
    const std::size_t used = rows - first_row;
    diagnostics_.observations = used;

    if (used < 2) {
        return fail(bad("covariance needs at least two observations in the window"));
    }

    CovarianceMethod method = cfg_.method;
    // Too few observations for a full covariance? Degrade to a diagonal and SAY
    // SO. A 50x50 estimated from 20 rows is not an estimate, and silently
    // returning noise is the worse failure.
    const double ratio = static_cast<double>(used) / static_cast<double>(assets);
    if (method != CovarianceMethod::Identity && assets > 1 && ratio < cfg_.min_observations_ratio) {
        diagnostics_.degraded = true;
        diagnostics_.degradation_reason =
            std::to_string(used) + " observations for " + std::to_string(assets) +
            " assets is below the " + std::to_string(cfg_.min_observations_ratio) +
            "x minimum; using a diagonal risk model instead of estimating correlations";
        method = CovarianceMethod::Identity;
    }

    // --- means -------------------------------------------------------------
    std::vector<double> mean(assets, 0.0);
    std::vector<double> weight(used, 0.0);
    double weight_sum = 0.0;

    if (method == CovarianceMethod::Ewma) {
        // Most recent row gets the largest weight.
        for (std::size_t r = 0; r < used; ++r) {
            const auto age = static_cast<double>(used - 1 - r);
            weight[r] = std::pow(cfg_.ewma_lambda, age);
            weight_sum += weight[r];
        }
    } else {
        for (std::size_t r = 0; r < used; ++r) weight[r] = 1.0;
        weight_sum = static_cast<double>(used);
    }
    if (weight_sum <= 0.0) return fail(bad("observation weights sum to zero"));

    for (std::size_t r = 0; r < used; ++r) {
        for (std::size_t c = 0; c < assets; ++c) {
            const double v = observations[(first_row + r) * assets + c];
            if (!is_finite(v)) {
                return fail(bad("observation at row " + std::to_string(first_row + r) + " column " +
                                std::to_string(c) + " is not finite"));
            }
            mean[c] += weight[r] * v;
        }
    }
    for (auto& m : mean) m /= weight_sum;

    // --- covariance --------------------------------------------------------
    SymmetricMatrix cov{assets};
    // Bessel-style correction for the equal-weighted cases; the EWMA form is
    // already a weighted mean and uses its own normaliser.
    const double denominator = method == CovarianceMethod::Ewma ? weight_sum : weight_sum - 1.0;
    if (denominator <= 0.0) return fail(bad("covariance denominator is not positive"));

    for (std::size_t i = 0; i < assets; ++i) {
        const std::size_t j_end = method == CovarianceMethod::Identity ? i + 1 : assets;
        for (std::size_t j = i; j < j_end; ++j) {
            double acc = 0.0;
            for (std::size_t r = 0; r < used; ++r) {
                const double a = observations[(first_row + r) * assets + i] - mean[i];
                const double b = observations[(first_row + r) * assets + j] - mean[j];
                acc += weight[r] * a * b;
            }
            const double value = acc / denominator;
            cov.set_symmetric(i, j, is_finite(value) ? value : 0.0);
        }
    }

    // --- shrinkage ---------------------------------------------------------
    if (method == CovarianceMethod::Shrinkage && assets > 1) {
        const auto diag = cov.diagonal();
        const double mean_variance =
            std::accumulate(diag.begin(), diag.end(), 0.0) / static_cast<double>(assets);

        double intensity = cfg_.shrinkage_intensity;
        if (intensity < 0.0) {
            // Analytic intensity: more shrinkage when observations are few
            // relative to assets, which is exactly when the sample estimate is
            // least trustworthy.
            intensity = std::clamp(static_cast<double>(assets) /
                                       (static_cast<double>(used) + static_cast<double>(assets)),
                                   0.0, 1.0);
        }
        intensity = std::clamp(intensity, 0.0, 1.0);
        diagnostics_.applied_shrinkage = intensity;

        for (std::size_t i = 0; i < assets; ++i) {
            for (std::size_t j = i; j < assets; ++j) {
                // Target is a scaled identity: mean variance on the diagonal,
                // zero correlation off it.
                const double target = i == j ? mean_variance : 0.0;
                const double blended = (1.0 - intensity) * cov.at(i, j) + intensity * target;
                cov.set_symmetric(i, j, blended);
            }
        }
    }

    // --- annualisation -----------------------------------------------------
    if (cfg_.annualization != 1.0 && cfg_.annualization > 0.0) {
        for (std::size_t i = 0; i < assets; ++i) {
            for (std::size_t j = i; j < assets; ++j) {
                cov.set_symmetric(i, j, cov.at(i, j) * cfg_.annualization);
            }
        }
    }

    // --- PSD ---------------------------------------------------------------
    const auto diag = cov.diagonal();
    const double mean_variance =
        std::accumulate(diag.begin(), diag.end(), 0.0) / static_cast<double>(assets);
    const double floor = std::max(1e-14, mean_variance * cfg_.min_eigenvalue_ratio);

    auto repaired = enforce_psd(cov, floor, &diagnostics_.psd_repaired,
                                &diagnostics_.smallest_eigenvalue_before);
    if (!repaired) return fail(repaired.error());
    return repaired;
}

}  // namespace ptl::optimization
