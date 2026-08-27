#include "ptl/models/matrix.hpp"

#include <algorithm>
#include <cmath>

namespace ptl::models {
namespace {

[[nodiscard]] Error bad(std::string message, std::string context = {}) {
    return make_error(ErrorCode::ValidationFailed, std::move(message), std::move(context));
}

}  // namespace

bool DesignMatrix::all_finite() const noexcept {
    return std::all_of(data_.begin(), data_.end(), [](double v) { return is_finite(v); });
}

Result<DesignMatrix> DesignMatrix::from_features(const features::FeatureMatrix& source,
                                                 std::span<const std::size_t> rows) {
    if (source.cols() == 0) return fail(bad("feature matrix has no columns"));

    DesignMatrix out{rows.size(), source.cols()};
    for (std::size_t r = 0; r < rows.size(); ++r) {
        const std::size_t src = rows[r];
        if (src >= source.rows()) {
            return fail(bad("row index out of range", std::to_string(src)));
        }
        for (std::size_t c = 0; c < source.cols(); ++c) {
            out.set(r, c, source.at(src, c));
        }
    }
    if (!out.all_finite()) {
        // A single NaN turns an entire coefficient vector into NaNs, and the
        // failure would surface as an unexplained model rather than a bad row.
        return fail(bad("selected feature rows contain a non-finite value"));
    }
    return out;
}

Result<bool> TrainingData::validate() const {
    if (features.rows() != targets.size()) {
        return fail(bad("feature rows (" + std::to_string(features.rows()) +
                        ") and target count (" + std::to_string(targets.size()) + ") disagree"));
    }
    if (targets.empty()) return fail(bad("training set is empty"));
    if (features.cols() == 0) return fail(bad("training set has no features"));
    if (!weights.empty() && weights.size() != targets.size()) {
        return fail(bad("weight count does not match the target count"));
    }
    if (!features.all_finite()) return fail(bad("features contain a non-finite value"));
    for (std::size_t i = 0; i < targets.size(); ++i) {
        if (!is_finite(targets[i])) {
            return fail(bad("target " + std::to_string(i) + " is not finite"));
        }
    }
    for (const double w : weights) {
        if (!is_finite(w) || w < 0.0) return fail(bad("weights must be finite and non-negative"));
    }
    if (targets.size() <= features.cols()) {
        // Fewer observations than parameters: the system is underdetermined and
        // any solution is arbitrary. Refusing is better than returning one.
        return fail(bad("need more observations than features: " + std::to_string(targets.size()) +
                        " rows for " + std::to_string(features.cols()) + " features"));
    }
    return true;
}

Result<TrainingData> make_training_data(const features::FeatureMatrix& features,
                                        std::span<const double> targets,
                                        std::span<const std::size_t> rows,
                                        std::span<const double> weights) {
    auto design = DesignMatrix::from_features(features, rows);
    if (!design) return fail(design.error());

    TrainingData data;
    data.features = std::move(*design);
    data.targets.reserve(rows.size());
    for (const auto r : rows) {
        if (r >= targets.size()) {
            return fail(bad("row index exceeds the target series", std::to_string(r)));
        }
        data.targets.push_back(targets[r]);
    }
    if (!weights.empty()) {
        data.weights.reserve(rows.size());
        for (const auto r : rows) {
            if (r >= weights.size()) return fail(bad("row index exceeds the weight series"));
            data.weights.push_back(weights[r]);
        }
    }
    if (auto ok = data.validate(); !ok) return fail(ok.error());
    return data;
}

}  // namespace ptl::models
