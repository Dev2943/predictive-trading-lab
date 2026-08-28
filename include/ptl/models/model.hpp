#pragma once

/// \file model.hpp
/// The model interface, and the linear and logistic implementations.
///
/// Every model exposes the same five operations -- fit, predict,
/// predict_proba, save, load -- so the walk-forward runner is written once and
/// works for all of them. A model that cannot produce a calibrated probability
/// returns an error from predict_proba rather than fabricating one.

#include <cstdint>
#include <istream>
#include <memory>
#include <ostream>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "ptl/core/result.hpp"
#include "ptl/models/matrix.hpp"

namespace ptl::models {

/// Fit diagnostics. Reported alongside every result, because a coefficient
/// vector without a condition number is a coefficient vector nobody can judge.
struct ModelDiagnostics {
    std::size_t n_train = 0;
    std::size_t n_features = 0;
    double intercept = 0.0;
    std::vector<double> coefficients;
    std::vector<double> standard_errors;
    std::vector<double> t_statistics;
    /// Variance inflation per feature. The diagnostic that justifies choosing
    /// ridge over OLS: lagged returns are strongly collinear, and a VIF above
    /// ~10 means the OLS coefficients are unstable rather than informative.
    std::vector<double> variance_inflation;
    double r_squared = 0.0;
    double adjusted_r_squared = 0.0;
    double in_sample_ic = 0.0;
    /// Ratio of largest to smallest singular value of X'X. Large means the
    /// normal equations are ill-conditioned and the solution is sensitive to
    /// rounding.
    double condition_number = 0.0;
    double residual_stdev = 0.0;

    [[nodiscard]] std::string describe() const;
};

class IModel {
public:
    IModel() = default;
    virtual ~IModel() = default;
    IModel(const IModel&) = delete;
    IModel& operator=(const IModel&) = delete;

protected:
    IModel(IModel&&) = default;
    IModel& operator=(IModel&&) = default;

public:
    [[nodiscard]] virtual std::string_view name() const noexcept = 0;

    /// Fit on the supplied data and nothing else.
    [[nodiscard]] virtual Result<bool> fit(const TrainingData&) = 0;

    /// One observation. The row must already be standardised by the same
    /// scaler the model was fitted with; Pipeline is what guarantees that.
    [[nodiscard]] virtual Result<double> predict(std::span<const double> features) const = 0;

    [[nodiscard]] virtual Result<std::vector<double>> predict_batch(const DesignMatrix&) const = 0;

    /// Calibrated probability, for models that produce one.
    ///
    /// \returns Unsupported for regressors. Returning a squashed regression
    ///          output would look like a probability and be badly calibrated,
    ///          which matters because position sizing depends on it.
    [[nodiscard]] virtual Result<double> predict_proba(std::span<const double> features) const;

    [[nodiscard]] virtual bool fitted() const noexcept = 0;
    [[nodiscard]] virtual const ModelDiagnostics& diagnostics() const noexcept = 0;

    /// Deterministic binary form: same fit, same bytes, on any machine.
    [[nodiscard]] virtual Result<bool> save(std::ostream&) const = 0;
    [[nodiscard]] virtual Result<bool> load(std::istream&) = 0;

    /// Hash of the fitted parameters. Two models with the same hash produce
    /// identical predictions, which is what the determinism test compares.
    [[nodiscard]] virtual std::uint64_t parameter_hash() const noexcept = 0;

    /// A fresh, unfitted instance with the same hyperparameters. The
    /// walk-forward runner uses this to guarantee each fold starts clean --
    /// reusing a fitted model across folds would carry the previous fold's
    /// parameters into the next one's initialisation.
    [[nodiscard]] virtual std::unique_ptr<IModel> clone_unfitted() const = 0;

    virtual void reset() noexcept = 0;
};

// ---------------------------------------------------------------------------
// Linear models
// ---------------------------------------------------------------------------

struct LinearConfig {
    /// L2 penalty. Zero is OLS.
    double l2_penalty = 0.0;
    bool fit_intercept = true;
    /// Compute standard errors, t-statistics and VIF. Off in a sweep, where
    /// only the predictions matter and the extra solve is wasted.
    bool compute_diagnostics = true;
    /// Ridge added to the normal equations purely for numerical conditioning
    /// when the penalty is zero and X'X is near-singular. Distinct from
    /// l2_penalty: this is a solver detail, not a modelling choice, and it is
    /// reported when it fires.
    double conditioning_epsilon = 1e-10;

    [[nodiscard]] std::string signature() const;
    [[nodiscard]] std::uint64_t hash() const;
};

/// Ordinary least squares and ridge, sharing one implementation.
///
/// Solved through the normal equations with an LDLT (Cholesky) factorisation:
/// X'X is symmetric positive semi-definite, so LDLT is both the fastest correct
/// choice and the one that fails loudly on a singular system rather than
/// returning arbitrary numbers.
///
/// THE INTERCEPT IS NEVER PENALISED. Shrinking it toward zero would bias every
/// prediction toward zero in proportion to the penalty, which is not what a
/// regularisation parameter is supposed to control.
class LinearRegression final : public IModel {
public:
    explicit LinearRegression(LinearConfig cfg = {}) : cfg_(cfg) {}

    [[nodiscard]] std::string_view name() const noexcept override {
        return cfg_.l2_penalty > 0.0 ? "ridge" : "ols";
    }

    [[nodiscard]] Result<bool> fit(const TrainingData&) override;
    [[nodiscard]] Result<double> predict(std::span<const double>) const override;
    [[nodiscard]] Result<std::vector<double>> predict_batch(const DesignMatrix&) const override;

    [[nodiscard]] bool fitted() const noexcept override { return fitted_; }
    [[nodiscard]] const ModelDiagnostics& diagnostics() const noexcept override { return diag_; }
    [[nodiscard]] Result<bool> save(std::ostream&) const override;
    [[nodiscard]] Result<bool> load(std::istream&) override;
    [[nodiscard]] std::uint64_t parameter_hash() const noexcept override;
    [[nodiscard]] std::unique_ptr<IModel> clone_unfitted() const override;
    void reset() noexcept override;

    [[nodiscard]] const LinearConfig& config() const noexcept { return cfg_; }
    [[nodiscard]] std::span<const double> coefficients() const noexcept { return coef_; }
    [[nodiscard]] double intercept() const noexcept { return intercept_; }

private:
    LinearConfig cfg_;
    bool fitted_ = false;
    double intercept_ = 0.0;
    std::vector<double> coef_;
    ModelDiagnostics diag_;
};

// ---------------------------------------------------------------------------
// Logistic regression
// ---------------------------------------------------------------------------

struct LogisticConfig {
    double l2_penalty = 1e-4;
    bool fit_intercept = true;
    std::size_t max_iterations = 100;
    /// Convergence threshold on the maximum coefficient change.
    double tolerance = 1e-8;
    /// Ridge added to the Hessian each IRLS step, for conditioning.
    double conditioning_epsilon = 1e-10;

    [[nodiscard]] std::string signature() const;
    [[nodiscard]] std::uint64_t hash() const;
};

/// Binary logistic regression by iteratively reweighted least squares.
///
/// IRLS rather than gradient descent: it converges in a handful of Newton steps
/// with no learning rate to tune, and a learning rate would be one more
/// hyperparameter to declare in the trial registry. Deterministic -- the
/// iteration is a pure function of the data, with no random initialisation.
///
/// The research treats this as a SECONDARY DIAGNOSTIC, not the primary model.
/// Direction accuracy is not P&L: a 54% hit rate loses money when the 46% are
/// larger.
class LogisticRegression final : public IModel {
public:
    explicit LogisticRegression(LogisticConfig cfg = {}) : cfg_(cfg) {}

    [[nodiscard]] std::string_view name() const noexcept override { return "logistic"; }

    [[nodiscard]] Result<bool> fit(const TrainingData&) override;
    /// Returns the probability of the positive class, same as predict_proba.
    [[nodiscard]] Result<double> predict(std::span<const double>) const override;
    [[nodiscard]] Result<double> predict_proba(std::span<const double>) const override;
    [[nodiscard]] Result<std::vector<double>> predict_batch(const DesignMatrix&) const override;

    [[nodiscard]] bool fitted() const noexcept override { return fitted_; }
    [[nodiscard]] const ModelDiagnostics& diagnostics() const noexcept override { return diag_; }
    [[nodiscard]] Result<bool> save(std::ostream&) const override;
    [[nodiscard]] Result<bool> load(std::istream&) override;
    [[nodiscard]] std::uint64_t parameter_hash() const noexcept override;
    [[nodiscard]] std::unique_ptr<IModel> clone_unfitted() const override;
    void reset() noexcept override;

    [[nodiscard]] const LogisticConfig& config() const noexcept { return cfg_; }
    [[nodiscard]] std::size_t iterations_used() const noexcept { return iterations_; }
    [[nodiscard]] bool converged() const noexcept { return converged_; }
    [[nodiscard]] std::span<const double> coefficients() const noexcept { return coef_; }
    [[nodiscard]] double intercept() const noexcept { return intercept_; }

private:
    LogisticConfig cfg_;
    bool fitted_ = false;
    bool converged_ = false;
    std::size_t iterations_ = 0;
    double intercept_ = 0.0;
    std::vector<double> coef_;
    ModelDiagnostics diag_;
};

// ---------------------------------------------------------------------------
// Rule baseline
// ---------------------------------------------------------------------------

struct RuleBaselineConfig {
    /// Column index of the signal feature.
    std::size_t signal_column = 0;
    /// Sign applied to it: -1 for a reversal rule, +1 for momentum.
    double direction = 1.0;
    /// Column index of a volatility feature used for scaling; npos disables it.
    std::size_t volatility_column = static_cast<std::size_t>(-1);
    double target_volatility = 0.01;

    [[nodiscard]] std::string signature() const;
    [[nodiscard]] std::uint64_t hash() const;
};

/// A no-model baseline: a volatility-targeted momentum or reversal rule.
///
/// THE MOST USEFUL ENTRY IN THE MODEL PROGRESSION, and the one most often
/// skipped. If ridge does not beat this after costs, the machine learning adds
/// nothing -- and that is the finding, not a failure. It stays a permanent
/// benchmark row in every report.
///
/// fit() computes only a scaling constant from the training rows, so it is a
/// legitimate IModel and can be run through the same walk-forward machinery.
class RuleBaseline final : public IModel {
public:
    explicit RuleBaseline(RuleBaselineConfig cfg = {}) : cfg_(cfg) {}

    [[nodiscard]] std::string_view name() const noexcept override { return "rule_baseline"; }

    [[nodiscard]] Result<bool> fit(const TrainingData&) override;
    [[nodiscard]] Result<double> predict(std::span<const double>) const override;
    [[nodiscard]] Result<std::vector<double>> predict_batch(const DesignMatrix&) const override;

    [[nodiscard]] bool fitted() const noexcept override { return fitted_; }
    [[nodiscard]] const ModelDiagnostics& diagnostics() const noexcept override { return diag_; }
    [[nodiscard]] Result<bool> save(std::ostream&) const override;
    [[nodiscard]] Result<bool> load(std::istream&) override;
    [[nodiscard]] std::uint64_t parameter_hash() const noexcept override;
    [[nodiscard]] std::unique_ptr<IModel> clone_unfitted() const override;
    void reset() noexcept override;

private:
    RuleBaselineConfig cfg_;
    bool fitted_ = false;
    double scale_ = 1.0;
    ModelDiagnostics diag_;
};

}  // namespace ptl::models
