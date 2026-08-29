#include <Eigen/Cholesky>
#include <Eigen/Dense>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <sstream>

#include "ptl/models/model.hpp"

namespace ptl::models {
namespace {

constexpr std::uint32_t kLinearMagic = 0x50544C4D;  // "PTLM"
constexpr std::uint32_t kLinearVersion = 1;

[[nodiscard]] Error bad(std::string message, std::string context = {}) {
    return make_error(ErrorCode::ValidationFailed, std::move(message), std::move(context));
}

template <class T>
void write_pod(std::ostream& os, const T& v) {
    os.write(reinterpret_cast<const char*>(&v), sizeof(T));
}
template <class T>
[[nodiscard]] bool read_pod(std::istream& is, T& v) {
    is.read(reinterpret_cast<char*>(&v), sizeof(T));
    return is.gcount() == static_cast<std::streamsize>(sizeof(T));
}

void hash_bytes(std::uint64_t& h, const void* data, std::size_t len) {
    const auto* p = static_cast<const unsigned char*>(data);
    for (std::size_t i = 0; i < len; ++i) {
        h ^= static_cast<std::uint64_t>(p[i]);
        h *= 0x100000001b3ULL;
    }
}

/// Copy a DesignMatrix into Eigen, optionally appending an intercept column.
[[nodiscard]] Eigen::MatrixXd to_eigen(const DesignMatrix& d, bool intercept) {
    const auto n = static_cast<Eigen::Index>(d.rows());
    const auto p = static_cast<Eigen::Index>(d.cols());
    Eigen::MatrixXd X(n, intercept ? p + 1 : p);
    for (Eigen::Index r = 0; r < n; ++r) {
        for (Eigen::Index c = 0; c < p; ++c) {
            X(r, c) = d.at(static_cast<std::size_t>(r), static_cast<std::size_t>(c));
        }
        if (intercept) X(r, p) = 1.0;
    }
    return X;
}

[[nodiscard]] Eigen::VectorXd to_eigen(std::span<const double> v) {
    Eigen::VectorXd out(static_cast<Eigen::Index>(v.size()));
    for (Eigen::Index i = 0; i < out.size(); ++i) {
        out(i) = v[static_cast<std::size_t>(i)];
    }
    return out;
}

}  // namespace

std::string ModelDiagnostics::describe() const {
    std::ostringstream ss;
    ss.precision(6);
    ss << std::fixed;
    ss << "fit on " << n_train << " observations, " << n_features << " features\n";
    ss << "  intercept        " << intercept << '\n';
    ss << "  R^2              " << r_squared << '\n';
    ss << "  adjusted R^2     " << adjusted_r_squared << '\n';
    ss << "  in-sample IC     " << in_sample_ic << '\n';
    ss << "  residual stdev   " << residual_stdev << '\n';
    ss << "  condition number " << condition_number;
    if (condition_number > 1e8) {
        // Worth saying out loud: at this conditioning the coefficients are
        // dominated by rounding, and ridge is the answer rather than a bigger
        // sample.
        ss << "  [ill-conditioned; prefer ridge]";
    }
    ss << '\n';
    if (!variance_inflation.empty()) {
        ss << "  max VIF          "
           << *std::max_element(variance_inflation.begin(), variance_inflation.end()) << '\n';
    }
    return ss.str();
}

std::string LinearConfig::signature() const {
    std::ostringstream ss;
    ss.precision(17);
    ss << "linear|l2=" << l2_penalty << "|intercept=" << (fit_intercept ? 1 : 0)
       << "|eps=" << conditioning_epsilon;
    return ss.str();
}
std::uint64_t LinearConfig::hash() const {
    return fnv1a64(signature());
}

Result<double> IModel::predict_proba(std::span<const double>) const {
    // A regressor squashed through a sigmoid LOOKS like a probability and is
    // badly calibrated. Position sizing depends on calibration, so refusing is
    // safer than obliging.
    return fail(
        make_error(ErrorCode::Unsupported, "this model does not produce calibrated probabilities"));
}

Result<bool> LinearRegression::fit(const TrainingData& data) {
    if (auto ok = data.validate(); !ok) return fail(ok.error());
    if (cfg_.l2_penalty < 0.0) return fail(bad("l2 penalty cannot be negative"));

    const Eigen::MatrixXd X = to_eigen(data.features, cfg_.fit_intercept);
    const Eigen::VectorXd y = to_eigen(data.targets);
    const Eigen::Index n = X.rows();
    const Eigen::Index k = X.cols();
    const auto p = static_cast<Eigen::Index>(data.features.cols());

    Eigen::MatrixXd Xw = X;
    Eigen::VectorXd yw = y;
    if (!data.weights.empty()) {
        // Weighted least squares by scaling rows with sqrt(w): the ordinary
        // normal equations then solve the weighted problem exactly.
        for (Eigen::Index i = 0; i < n; ++i) {
            const double sw = std::sqrt(data.weights[static_cast<std::size_t>(i)]);
            Xw.row(i) *= sw;
            yw(i) *= sw;
        }
    }

    Eigen::MatrixXd xtx = Xw.transpose() * Xw;
    const Eigen::VectorXd xty = Xw.transpose() * yw;

    // THE INTERCEPT IS NEVER PENALISED. Shrinking it toward zero would bias
    // every prediction toward zero in proportion to the penalty, which is not
    // what a regularisation parameter is meant to control.
    for (Eigen::Index i = 0; i < p; ++i) {
        xtx(i, i) += cfg_.l2_penalty + cfg_.conditioning_epsilon;
    }
    if (cfg_.fit_intercept) {
        xtx(k - 1, k - 1) += cfg_.conditioning_epsilon;
    }

    // LDLT: X'X is symmetric positive semi-definite, so this is both the
    // fastest correct factorisation and one that reports failure rather than
    // returning arbitrary numbers on a singular system.
    const Eigen::LDLT<Eigen::MatrixXd> ldlt(xtx);
    if (ldlt.info() != Eigen::Success) {
        return fail(bad("normal equations are singular; increase the ridge penalty"));
    }
    const Eigen::VectorXd beta = ldlt.solve(xty);
    if (!beta.allFinite()) {
        return fail(bad("solver produced non-finite coefficients"));
    }

    coef_.assign(static_cast<std::size_t>(p), 0.0);
    for (Eigen::Index i = 0; i < p; ++i) coef_[static_cast<std::size_t>(i)] = beta(i);
    intercept_ = cfg_.fit_intercept ? beta(k - 1) : 0.0;

    diag_ = ModelDiagnostics{};
    diag_.n_train = static_cast<std::size_t>(n);
    diag_.n_features = static_cast<std::size_t>(p);
    diag_.intercept = intercept_;
    diag_.coefficients = coef_;

    const Eigen::VectorXd fitted = X * beta;
    const Eigen::VectorXd resid = y - fitted;
    const double sse = resid.squaredNorm();
    const double mean_y = y.mean();
    const double sst = (y.array() - mean_y).square().sum();
    diag_.r_squared = sst > 0.0 ? 1.0 - sse / sst : 0.0;
    const auto dof = static_cast<double>(n - k);
    if (dof > 0.0 && sst > 0.0) {
        diag_.adjusted_r_squared =
            1.0 - (1.0 - diag_.r_squared) * (static_cast<double>(n) - 1.0) / dof;
        diag_.residual_stdev = std::sqrt(sse / dof);
    }

    // In-sample IC. Reported so it can be compared against the out-of-sample
    // figure: a large gap is the clearest signal of overfitting available.
    const double fm = fitted.mean();
    const double num = ((fitted.array() - fm) * (y.array() - mean_y)).sum();
    const double den = std::sqrt((fitted.array() - fm).square().sum() * sst);
    diag_.in_sample_ic = den > 0.0 ? num / den : 0.0;

    if (cfg_.compute_diagnostics) {
        const Eigen::SelfAdjointEigenSolver<Eigen::MatrixXd> es(xtx);
        if (es.info() == Eigen::Success) {
            const double lo = es.eigenvalues().minCoeff();
            const double hi = es.eigenvalues().maxCoeff();
            diag_.condition_number = lo > 0.0 ? hi / lo : 0.0;
        }

        if (dof > 0.0) {
            const Eigen::MatrixXd cov = ldlt.solve(Eigen::MatrixXd::Identity(k, k)) * (sse / dof);
            diag_.standard_errors.assign(static_cast<std::size_t>(p), 0.0);
            diag_.t_statistics.assign(static_cast<std::size_t>(p), 0.0);
            for (Eigen::Index i = 0; i < p; ++i) {
                const double se = std::sqrt(std::max(0.0, cov(i, i)));
                diag_.standard_errors[static_cast<std::size_t>(i)] = se;
                diag_.t_statistics[static_cast<std::size_t>(i)] = se > 0.0 ? beta(i) / se : 0.0;
            }
        }

        // VIF: the diagnostic that justifies ridge over OLS. Lagged returns are
        // strongly collinear, and a VIF above ~10 means the OLS coefficients
        // are unstable rather than informative.
        diag_.variance_inflation.assign(static_cast<std::size_t>(p), 1.0);
        if (p > 1) {
            Eigen::MatrixXd corr = xtx.topLeftCorner(p, p);
            const Eigen::LDLT<Eigen::MatrixXd> cl(corr);
            if (cl.info() == Eigen::Success) {
                const Eigen::MatrixXd inv = cl.solve(Eigen::MatrixXd::Identity(p, p));
                for (Eigen::Index i = 0; i < p; ++i) {
                    const double v = inv(i, i) * corr(i, i);
                    diag_.variance_inflation[static_cast<std::size_t>(i)] =
                        is_finite(v) ? std::abs(v) : 1.0;
                }
            }
        }
    }

    fitted_ = true;
    return true;
}

Result<double> LinearRegression::predict(std::span<const double> features) const {
    if (!fitted_) return fail(bad("model has not been fitted"));
    if (features.size() != coef_.size()) {
        return fail(bad("expected " + std::to_string(coef_.size()) + " features, got " +
                        std::to_string(features.size())));
    }
    double y = intercept_;
    for (std::size_t i = 0; i < coef_.size(); ++i) y += coef_[i] * features[i];
    if (!is_finite(y)) return fail(bad("prediction is not finite"));
    return y;
}

Result<std::vector<double>> LinearRegression::predict_batch(const DesignMatrix& X) const {
    if (!fitted_) return fail(bad("model has not been fitted"));
    if (X.cols() != coef_.size()) return fail(bad("feature width does not match the model"));

    std::vector<double> out;
    out.reserve(X.rows());
    for (std::size_t r = 0; r < X.rows(); ++r) {
        double y = intercept_;
        const auto row = X.row(r);
        for (std::size_t c = 0; c < coef_.size(); ++c) y += coef_[c] * row[c];
        out.push_back(is_finite(y) ? y : 0.0);
    }
    return out;
}

Result<bool> LinearRegression::save(std::ostream& os) const {
    if (!fitted_) return fail(bad("cannot serialise an unfitted model"));
    write_pod(os, kLinearMagic);
    write_pod(os, kLinearVersion);
    write_pod(os, cfg_.l2_penalty);
    write_pod(os, cfg_.fit_intercept);
    write_pod(os, cfg_.conditioning_epsilon);
    write_pod(os, intercept_);
    const std::uint64_t p = coef_.size();
    write_pod(os, p);
    os.write(reinterpret_cast<const char*>(coef_.data()),
             static_cast<std::streamsize>(p * sizeof(double)));
    // Hash last, so a truncated file fails the check rather than loading
    // silently with fewer coefficients than it claims.
    const std::uint64_t h = parameter_hash();
    write_pod(os, h);
    return os.good() ? Result<bool>{true} : fail(bad("write failed"));
}

Result<bool> LinearRegression::load(std::istream& is) {
    std::uint32_t magic = 0;
    std::uint32_t version = 0;
    if (!read_pod(is, magic) || magic != kLinearMagic) {
        return fail(bad("not a serialised linear model"));
    }
    if (!read_pod(is, version) || version != kLinearVersion) {
        return fail(bad("unsupported model format version " + std::to_string(version)));
    }
    std::uint64_t p = 0;
    if (!read_pod(is, cfg_.l2_penalty) || !read_pod(is, cfg_.fit_intercept) ||
        !read_pod(is, cfg_.conditioning_epsilon) || !read_pod(is, intercept_) || !read_pod(is, p)) {
        return fail(bad("truncated model header"));
    }
    coef_.resize(p);
    is.read(reinterpret_cast<char*>(coef_.data()),
            static_cast<std::streamsize>(p * sizeof(double)));
    if (is.gcount() != static_cast<std::streamsize>(p * sizeof(double))) {
        return fail(bad("truncated coefficient block"));
    }
    fitted_ = true;

    std::uint64_t stored = 0;
    if (!read_pod(is, stored)) return fail(bad("truncated model hash"));
    if (stored != parameter_hash()) {
        // A corrupt model would predict confidently and wrongly.
        return fail(bad("model parameter hash mismatch: the file is corrupt"));
    }
    diag_ = ModelDiagnostics{};
    diag_.n_features = coef_.size();
    diag_.intercept = intercept_;
    diag_.coefficients = coef_;
    return true;
}

std::uint64_t LinearRegression::parameter_hash() const noexcept {
    std::uint64_t h = 0xcbf29ce484222325ULL;
    hash_bytes(h, &intercept_, sizeof(intercept_));
    // Bit patterns, not rounded decimals: two models differing in the last ulp
    // must hash differently or the determinism test proves nothing.
    for (const double c : coef_) hash_bytes(h, &c, sizeof(c));
    return h;
}

std::unique_ptr<IModel> LinearRegression::clone_unfitted() const {
    return std::make_unique<LinearRegression>(cfg_);
}

void LinearRegression::reset() noexcept {
    fitted_ = false;
    intercept_ = 0.0;
    coef_.clear();
    diag_ = ModelDiagnostics{};
}

// ---------------------------------------------------------------------------
// RuleBaseline
// ---------------------------------------------------------------------------

std::string RuleBaselineConfig::signature() const {
    std::ostringstream ss;
    ss.precision(17);
    ss << "rule|col=" << signal_column << "|dir=" << direction << "|vol=" << volatility_column
       << "|target=" << target_volatility;
    return ss.str();
}
std::uint64_t RuleBaselineConfig::hash() const {
    return fnv1a64(signature());
}

Result<bool> RuleBaseline::fit(const TrainingData& data) {
    if (auto ok = data.validate(); !ok) return fail(ok.error());
    if (cfg_.signal_column >= data.features.cols()) {
        return fail(bad("signal column is out of range"));
    }

    // The only thing fitted is a scale, computed from the TRAINING rows: the
    // dispersion of the signal, so predictions are comparable in magnitude to a
    // model's. No coefficient is learned -- that is the point of a baseline.
    double mean = 0.0;
    double m2 = 0.0;
    std::size_t n = 0;
    for (std::size_t r = 0; r < data.features.rows(); ++r) {
        const double x = data.features.at(r, cfg_.signal_column);
        ++n;
        const double d = x - mean;
        mean += d / static_cast<double>(n);
        m2 += d * (x - mean);
    }
    const double sd = n > 1 ? std::sqrt(m2 / static_cast<double>(n - 1)) : 0.0;
    scale_ = sd > 1e-12 ? cfg_.target_volatility / sd : 1.0;

    diag_ = ModelDiagnostics{};
    diag_.n_train = data.features.rows();
    diag_.n_features = data.features.cols();
    fitted_ = true;
    return true;
}

Result<double> RuleBaseline::predict(std::span<const double> features) const {
    if (!fitted_) return fail(bad("baseline has not been fitted"));
    if (cfg_.signal_column >= features.size()) return fail(bad("signal column is out of range"));

    double y = cfg_.direction * features[cfg_.signal_column] * scale_;
    if (cfg_.volatility_column != static_cast<std::size_t>(-1) &&
        cfg_.volatility_column < features.size()) {
        const double vol = features[cfg_.volatility_column];
        // Volatility targeting: scale down when the instrument is moving more.
        // Zero volatility leaves the signal untouched rather than dividing.
        if (vol > 1e-12 && is_finite(vol)) y *= cfg_.target_volatility / vol;
    }
    return is_finite(y) ? Result<double>{y} : fail(bad("baseline prediction is not finite"));
}

Result<std::vector<double>> RuleBaseline::predict_batch(const DesignMatrix& X) const {
    if (!fitted_) return fail(bad("baseline has not been fitted"));
    std::vector<double> out;
    out.reserve(X.rows());
    for (std::size_t r = 0; r < X.rows(); ++r) {
        auto v = predict(X.row(r));
        if (!v) return fail(v.error());
        out.push_back(*v);
    }
    return out;
}

Result<bool> RuleBaseline::save(std::ostream& os) const {
    if (!fitted_) return fail(bad("cannot serialise an unfitted baseline"));
    write_pod(os, kLinearMagic);
    write_pod(os, kLinearVersion);
    write_pod(os, cfg_.signal_column);
    write_pod(os, cfg_.direction);
    write_pod(os, cfg_.volatility_column);
    write_pod(os, cfg_.target_volatility);
    write_pod(os, scale_);
    const std::uint64_t h = parameter_hash();
    write_pod(os, h);
    return os.good() ? Result<bool>{true} : fail(bad("write failed"));
}

Result<bool> RuleBaseline::load(std::istream& is) {
    std::uint32_t magic = 0;
    std::uint32_t version = 0;
    if (!read_pod(is, magic) || magic != kLinearMagic) return fail(bad("not a serialised model"));
    if (!read_pod(is, version) || version != kLinearVersion) {
        return fail(bad("unsupported model format version"));
    }
    if (!read_pod(is, cfg_.signal_column) || !read_pod(is, cfg_.direction) ||
        !read_pod(is, cfg_.volatility_column) || !read_pod(is, cfg_.target_volatility) ||
        !read_pod(is, scale_)) {
        return fail(bad("truncated baseline"));
    }
    fitted_ = true;
    std::uint64_t stored = 0;
    if (!read_pod(is, stored)) return fail(bad("truncated baseline hash"));
    if (stored != parameter_hash()) return fail(bad("baseline hash mismatch: file is corrupt"));
    return true;
}

std::uint64_t RuleBaseline::parameter_hash() const noexcept {
    std::uint64_t h = 0xcbf29ce484222325ULL;
    hash_bytes(h, &scale_, sizeof(scale_));
    hash_bytes(h, &cfg_.direction, sizeof(cfg_.direction));
    hash_bytes(h, &cfg_.signal_column, sizeof(cfg_.signal_column));
    return h;
}

std::unique_ptr<IModel> RuleBaseline::clone_unfitted() const {
    return std::make_unique<RuleBaseline>(cfg_);
}

void RuleBaseline::reset() noexcept {
    fitted_ = false;
    scale_ = 1.0;
    diag_ = ModelDiagnostics{};
}

}  // namespace ptl::models
