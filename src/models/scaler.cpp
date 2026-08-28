#include "ptl/models/scaler.hpp"

#include <algorithm>
#include <cmath>

namespace ptl::models {
namespace {

constexpr std::uint32_t kScalerMagic = 0x50544C53;  // "PTLS"
constexpr std::uint32_t kScalerVersion = 1;

[[nodiscard]] Error bad(std::string message) {
    return make_error(ErrorCode::ValidationFailed, std::move(message));
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

}  // namespace

Result<bool> StandardScaler::fit(const DesignMatrix& training_features, double clip_sigma) {
    if (training_features.empty()) return fail(bad("cannot fit a scaler on empty data"));
    if (training_features.rows() < 2) {
        return fail(bad("scaler needs at least two observations to estimate a variance"));
    }

    const std::size_t n = training_features.rows();
    const std::size_t p = training_features.cols();
    means_.assign(p, 0.0);
    stdevs_.assign(p, 1.0);

    for (std::size_t c = 0; c < p; ++c) {
        // Welford per column: numerically stable where the naive
        // sum-of-squares form cancels catastrophically for large means.
        double mean = 0.0;
        double m2 = 0.0;
        std::size_t seen = 0;
        for (std::size_t r = 0; r < n; ++r) {
            const double x = training_features.at(r, c);
            ++seen;
            const double delta = x - mean;
            mean += delta / static_cast<double>(seen);
            m2 += delta * (x - mean);
        }
        means_[c] = mean;
        const double var = m2 / static_cast<double>(n - 1);
        const double sd = std::sqrt(std::max(0.0, var));
        // A constant feature has zero variance. Dividing would produce inf for
        // every row at once; a scale of 1.0 leaves the column as a constant
        // offset, which the intercept absorbs harmlessly.
        stdevs_[c] = (sd > 1e-12 && is_finite(sd)) ? sd : 1.0;
    }

    clip_sigma_ = clip_sigma;
    fitted_ = true;
    return true;
}

Result<bool> StandardScaler::transform(DesignMatrix& features) const {
    if (!fitted_) return fail(bad("scaler has not been fitted"));
    if (features.cols() != means_.size()) {
        return fail(bad("scaler was fitted on " + std::to_string(means_.size()) +
                        " features but was given " + std::to_string(features.cols())));
    }
    for (std::size_t r = 0; r < features.rows(); ++r) {
        for (std::size_t c = 0; c < features.cols(); ++c) {
            double z = (features.at(r, c) - means_[c]) / stdevs_[c];
            if (clip_sigma_ > 0.0) z = std::clamp(z, -clip_sigma_, clip_sigma_);
            features.set(r, c, is_finite(z) ? z : 0.0);
        }
    }
    return true;
}

Result<bool> StandardScaler::transform_row(std::span<double> row) const {
    if (!fitted_) return fail(bad("scaler has not been fitted"));
    if (row.size() != means_.size()) {
        return fail(bad("scaler width does not match the observation width"));
    }
    for (std::size_t c = 0; c < row.size(); ++c) {
        double z = (row[c] - means_[c]) / stdevs_[c];
        if (clip_sigma_ > 0.0) z = std::clamp(z, -clip_sigma_, clip_sigma_);
        row[c] = is_finite(z) ? z : 0.0;
    }
    return true;
}

void StandardScaler::write(std::ostream& os) const {
    write_pod(os, kScalerMagic);
    write_pod(os, kScalerVersion);
    write_pod(os, clip_sigma_);
    const std::uint64_t p = means_.size();
    write_pod(os, p);
    os.write(reinterpret_cast<const char*>(means_.data()),
             static_cast<std::streamsize>(p * sizeof(double)));
    os.write(reinterpret_cast<const char*>(stdevs_.data()),
             static_cast<std::streamsize>(p * sizeof(double)));
}

Result<StandardScaler> StandardScaler::read(std::istream& is) {
    std::uint32_t magic = 0;
    std::uint32_t version = 0;
    if (!read_pod(is, magic) || magic != kScalerMagic) {
        return fail(bad("not a serialised scaler"));
    }
    if (!read_pod(is, version) || version != kScalerVersion) {
        return fail(bad("unsupported scaler format version " + std::to_string(version)));
    }
    StandardScaler s;
    std::uint64_t p = 0;
    if (!read_pod(is, s.clip_sigma_) || !read_pod(is, p)) {
        return fail(bad("truncated scaler header"));
    }
    s.means_.resize(p);
    s.stdevs_.resize(p);
    is.read(reinterpret_cast<char*>(s.means_.data()),
            static_cast<std::streamsize>(p * sizeof(double)));
    if (is.gcount() != static_cast<std::streamsize>(p * sizeof(double))) {
        return fail(bad("truncated scaler means"));
    }
    is.read(reinterpret_cast<char*>(s.stdevs_.data()),
            static_cast<std::streamsize>(p * sizeof(double)));
    if (is.gcount() != static_cast<std::streamsize>(p * sizeof(double))) {
        return fail(bad("truncated scaler deviations"));
    }
    s.fitted_ = true;
    return s;
}

std::uint64_t StandardScaler::content_hash() const noexcept {
    std::uint64_t h = 0xcbf29ce484222325ULL;
    hash_bytes(h, &clip_sigma_, sizeof(clip_sigma_));
    for (const double m : means_) hash_bytes(h, &m, sizeof(m));
    for (const double s : stdevs_) hash_bytes(h, &s, sizeof(s));
    return h;
}

void StandardScaler::reset() noexcept {
    fitted_ = false;
    clip_sigma_ = 0.0;
    means_.clear();
    stdevs_.clear();
}

}  // namespace ptl::models
