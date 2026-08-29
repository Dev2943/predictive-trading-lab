#include <Eigen/Cholesky>
#include <Eigen/Dense>
#include <algorithm>
#include <cmath>
#include <sstream>

#include "ptl/models/model.hpp"

namespace ptl::models {
namespace {

constexpr std::uint32_t kLogisticMagic = 0x50544C47;  // "PTLG"
constexpr std::uint32_t kLogisticVersion = 1;

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

/// Numerically stable logistic function.
///
/// The naive 1/(1+exp(-z)) overflows for large negative z. Branching on the
/// sign keeps the exponent negative in both cases, which matters because IRLS
/// drives |z| large precisely when the classes separate cleanly.
[[nodiscard]] double sigmoid(double z) noexcept {
    if (z >= 0.0) return 1.0 / (1.0 + std::exp(-z));
    const double e = std::exp(z);
    return e / (1.0 + e);
}

}  // namespace

std::string LogisticConfig::signature() const {
    std::ostringstream ss;
    ss.precision(17);
    ss << "logistic|l2=" << l2_penalty << "|intercept=" << (fit_intercept ? 1 : 0)
       << "|iters=" << max_iterations << "|tol=" << tolerance;
    return ss.str();
}
std::uint64_t LogisticConfig::hash() const {
    return fnv1a64(signature());
}

Result<bool> LogisticRegression::fit(const TrainingData& data) {
    if (auto ok = data.validate(); !ok) return fail(ok.error());

    const auto n = static_cast<Eigen::Index>(data.features.rows());
    const auto p = static_cast<Eigen::Index>(data.features.cols());
    const Eigen::Index k = cfg_.fit_intercept ? p + 1 : p;

    Eigen::MatrixXd X(n, k);
    Eigen::VectorXd y(n);
    std::size_t positives = 0;
    for (Eigen::Index r = 0; r < n; ++r) {
        for (Eigen::Index c = 0; c < p; ++c) {
            X(r, c) = data.features.at(static_cast<std::size_t>(r), static_cast<std::size_t>(c));
        }
        if (cfg_.fit_intercept) X(r, p) = 1.0;
        const double target = data.targets[static_cast<std::size_t>(r)];
        if (target != 0.0 && target != 1.0) {
            return fail(bad("logistic targets must be exactly 0 or 1", std::to_string(target)));
        }
        y(r) = target;
        if (target > 0.5) ++positives;
    }
    if (positives == 0 || positives == static_cast<std::size_t>(n)) {
        // One class absent: the likelihood is maximised by pushing the
        // intercept to infinity. Refusing is honest; iterating would produce
        // enormous coefficients that predict the constant class with certainty.
        return fail(bad("logistic regression needs both classes present in training"));
    }

    // IRLS rather than gradient descent: it converges in a handful of Newton
    // steps with no learning rate to tune, and a learning rate would be one
    // more hyperparameter to declare in the trial registry. Deterministic --
    // the iteration is a pure function of the data, starting from zero.
    Eigen::VectorXd beta = Eigen::VectorXd::Zero(k);
    converged_ = false;
    iterations_ = 0;

    for (std::size_t iter = 0; iter < cfg_.max_iterations; ++iter) {
        ++iterations_;
        const Eigen::VectorXd eta = X * beta;

        Eigen::VectorXd mu(n);
        Eigen::VectorXd w(n);
        for (Eigen::Index i = 0; i < n; ++i) {
            const double m = sigmoid(eta(i));
            mu(i) = m;
            // Floor the IRLS weight. As mu approaches 0 or 1 the weight goes to
            // zero and the Hessian becomes singular; clamping keeps the step
            // well defined under separation.
            w(i) = std::max(m * (1.0 - m), 1e-10);
        }

        Eigen::VectorXd residual = y - mu;
        if (!data.weights.empty()) {
            for (Eigen::Index i = 0; i < n; ++i) {
                const double sw = data.weights[static_cast<std::size_t>(i)];
                w(i) *= sw;
                residual(i) *= sw;
            }
        }

        Eigen::MatrixXd hessian = X.transpose() * w.asDiagonal() * X;
        // Penalise the slopes, never the intercept -- the same rule as ridge,
        // and for the same reason.
        for (Eigen::Index i = 0; i < p; ++i) {
            hessian(i, i) += cfg_.l2_penalty + cfg_.conditioning_epsilon;
        }
        if (cfg_.fit_intercept) hessian(k - 1, k - 1) += cfg_.conditioning_epsilon;

        Eigen::VectorXd gradient = X.transpose() * residual;
        for (Eigen::Index i = 0; i < p; ++i) gradient(i) -= cfg_.l2_penalty * beta(i);

        const Eigen::LDLT<Eigen::MatrixXd> ldlt(hessian);
        if (ldlt.info() != Eigen::Success) {
            return fail(bad("logistic Hessian is singular; increase the l2 penalty"));
        }
        const Eigen::VectorXd step = ldlt.solve(gradient);
        if (!step.allFinite()) return fail(bad("logistic step is not finite"));

        beta += step;
        if (step.cwiseAbs().maxCoeff() < cfg_.tolerance) {
            converged_ = true;
            break;
        }
    }

    if (!beta.allFinite()) return fail(bad("logistic coefficients are not finite"));

    coef_.assign(static_cast<std::size_t>(p), 0.0);
    for (Eigen::Index i = 0; i < p; ++i) coef_[static_cast<std::size_t>(i)] = beta(i);
    intercept_ = cfg_.fit_intercept ? beta(k - 1) : 0.0;

    diag_ = ModelDiagnostics{};
    diag_.n_train = static_cast<std::size_t>(n);
    diag_.n_features = static_cast<std::size_t>(p);
    diag_.intercept = intercept_;
    diag_.coefficients = coef_;

    // McFadden pseudo-R^2. Reported under the same field name as the linear
    // R^2 but it is NOT comparable to one: values around 0.2-0.4 indicate an
    // excellent fit for a logistic model.
    double loglik = 0.0;
    double null_loglik = 0.0;
    const double base_rate = static_cast<double>(positives) / static_cast<double>(n);
    for (Eigen::Index i = 0; i < n; ++i) {
        const double m = std::clamp(sigmoid((X.row(i) * beta)(0)), 1e-15, 1.0 - 1e-15);
        loglik += y(i) * std::log(m) + (1.0 - y(i)) * std::log(1.0 - m);
        null_loglik += y(i) * std::log(base_rate) + (1.0 - y(i)) * std::log(1.0 - base_rate);
    }
    diag_.r_squared = null_loglik < 0.0 ? 1.0 - loglik / null_loglik : 0.0;

    fitted_ = true;
    return true;
}

Result<double> LogisticRegression::predict(std::span<const double> features) const {
    return predict_proba(features);
}

Result<double> LogisticRegression::predict_proba(std::span<const double> features) const {
    if (!fitted_) return fail(bad("model has not been fitted"));
    if (features.size() != coef_.size()) {
        return fail(bad("expected " + std::to_string(coef_.size()) + " features, got " +
                        std::to_string(features.size())));
    }
    double z = intercept_;
    for (std::size_t i = 0; i < coef_.size(); ++i) z += coef_[i] * features[i];
    const double p = sigmoid(z);
    if (!is_finite(p)) return fail(bad("probability is not finite"));
    return p;
}

Result<std::vector<double>> LogisticRegression::predict_batch(const DesignMatrix& X) const {
    if (!fitted_) return fail(bad("model has not been fitted"));
    if (X.cols() != coef_.size()) return fail(bad("feature width does not match the model"));

    std::vector<double> out;
    out.reserve(X.rows());
    for (std::size_t r = 0; r < X.rows(); ++r) {
        double z = intercept_;
        const auto row = X.row(r);
        for (std::size_t c = 0; c < coef_.size(); ++c) z += coef_[c] * row[c];
        out.push_back(sigmoid(z));
    }
    return out;
}

Result<bool> LogisticRegression::save(std::ostream& os) const {
    if (!fitted_) return fail(bad("cannot serialise an unfitted model"));
    write_pod(os, kLogisticMagic);
    write_pod(os, kLogisticVersion);
    write_pod(os, cfg_.l2_penalty);
    write_pod(os, cfg_.fit_intercept);
    write_pod(os, cfg_.max_iterations);
    write_pod(os, cfg_.tolerance);
    write_pod(os, intercept_);
    const std::uint64_t p = coef_.size();
    write_pod(os, p);
    os.write(reinterpret_cast<const char*>(coef_.data()),
             static_cast<std::streamsize>(p * sizeof(double)));
    const std::uint64_t h = parameter_hash();
    write_pod(os, h);
    return os.good() ? Result<bool>{true} : fail(bad("write failed"));
}

Result<bool> LogisticRegression::load(std::istream& is) {
    std::uint32_t magic = 0;
    std::uint32_t version = 0;
    if (!read_pod(is, magic) || magic != kLogisticMagic) {
        return fail(bad("not a serialised logistic model"));
    }
    if (!read_pod(is, version) || version != kLogisticVersion) {
        return fail(bad("unsupported model format version " + std::to_string(version)));
    }
    std::uint64_t p = 0;
    if (!read_pod(is, cfg_.l2_penalty) || !read_pod(is, cfg_.fit_intercept) ||
        !read_pod(is, cfg_.max_iterations) || !read_pod(is, cfg_.tolerance) ||
        !read_pod(is, intercept_) || !read_pod(is, p)) {
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
        return fail(bad("model parameter hash mismatch: the file is corrupt"));
    }
    diag_ = ModelDiagnostics{};
    diag_.n_features = coef_.size();
    diag_.intercept = intercept_;
    diag_.coefficients = coef_;
    return true;
}

std::uint64_t LogisticRegression::parameter_hash() const noexcept {
    std::uint64_t h = 0xcbf29ce484222325ULL;
    hash_bytes(h, &intercept_, sizeof(intercept_));
    for (const double c : coef_) hash_bytes(h, &c, sizeof(c));
    return h;
}

std::unique_ptr<IModel> LogisticRegression::clone_unfitted() const {
    return std::make_unique<LogisticRegression>(cfg_);
}

void LogisticRegression::reset() noexcept {
    fitted_ = false;
    converged_ = false;
    iterations_ = 0;
    intercept_ = 0.0;
    coef_.clear();
    diag_ = ModelDiagnostics{};
}

}  // namespace ptl::models
