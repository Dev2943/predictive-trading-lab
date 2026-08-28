#include "ptl/models/pipeline.hpp"

#include <algorithm>
#include <cmath>

namespace ptl::models {
namespace {

[[nodiscard]] Error bad(std::string message, std::string context = {}) {
    return make_error(ErrorCode::ValidationFailed, std::move(message), std::move(context));
}

void hash_bytes(std::uint64_t& h, const void* data, std::size_t len) {
    const auto* p = static_cast<const unsigned char*>(data);
    for (std::size_t i = 0; i < len; ++i) {
        h ^= static_cast<std::uint64_t>(p[i]);
        h *= 0x100000001b3ULL;
    }
}

}  // namespace

Result<bool> Pipeline::fit(const features::FeatureMatrix& features, std::span<const double> targets,
                           std::span<const std::size_t> train_rows,
                           std::span<const double> weights) {
    if (model_ == nullptr) return fail(bad("pipeline has no model"));
    if (train_rows.empty()) return fail(bad("training row set is empty"));

    auto data = make_training_data(features, targets, train_rows, weights);
    if (!data) return fail(data.error());

    if (cfg_.standardize) {
        // Fitted on the TRAINING rows and nothing else. This single call is
        // what separates a valid backtest from one where every training row
        // knew the test period's mean and variance.
        if (auto ok = scaler_.fit(data->features, cfg_.clip_sigma); !ok) {
            return fail(ok.error());
        }
        if (auto ok = scaler_.transform(data->features); !ok) return fail(ok.error());
    }

    model_->reset();
    if (auto ok = model_->fit(*data); !ok) return fail(ok.error());
    fitted_ = true;
    return true;
}

Result<std::vector<double>> Pipeline::predict(const features::FeatureMatrix& features,
                                              std::span<const std::size_t> rows) const {
    if (!fitted_) return fail(bad("pipeline has not been fitted"));

    auto design = DesignMatrix::from_features(features, rows);
    if (!design) return fail(design.error());

    if (cfg_.standardize) {
        // The TRAINING statistics, applied to test rows. scaler_ is const here,
        // so there is no path by which this call could refit on the data it is
        // about to score.
        if (auto ok = scaler_.transform(*design); !ok) return fail(ok.error());
    }
    return model_->predict_batch(*design);
}

Result<double> Pipeline::predict_one(std::span<const double> raw_features) const {
    if (!fitted_) return fail(bad("pipeline has not been fitted"));

    std::vector<double> row(raw_features.begin(), raw_features.end());
    if (cfg_.standardize) {
        if (auto ok = scaler_.transform_row(row); !ok) return fail(ok.error());
    }
    return model_->predict(row);
}

Result<bool> Pipeline::save(std::ostream& os) const {
    if (!fitted_) return fail(bad("cannot serialise an unfitted pipeline"));
    const std::uint8_t standardize = cfg_.standardize ? 1 : 0;
    os.write(reinterpret_cast<const char*>(&standardize), sizeof(standardize));
    if (cfg_.standardize) scaler_.write(os);
    return model_->save(os);
}

Result<bool> Pipeline::load(std::istream& is) {
    if (model_ == nullptr) return fail(bad("pipeline has no model to load into"));
    std::uint8_t standardize = 0;
    is.read(reinterpret_cast<char*>(&standardize), sizeof(standardize));
    if (is.gcount() != sizeof(standardize)) return fail(bad("truncated pipeline header"));

    cfg_.standardize = standardize != 0;
    if (cfg_.standardize) {
        auto s = StandardScaler::read(is);
        if (!s) return fail(s.error());
        scaler_ = std::move(*s);
    }
    if (auto ok = model_->load(is); !ok) return fail(ok.error());
    fitted_ = true;
    return true;
}

std::uint64_t Pipeline::parameter_hash() const noexcept {
    std::uint64_t h = model_ != nullptr ? model_->parameter_hash() : 0;
    const std::uint64_t s = scaler_.content_hash();
    hash_bytes(h, &s, sizeof(s));
    return h;
}

// ---------------------------------------------------------------------------
// WalkForwardRunner
// ---------------------------------------------------------------------------

std::vector<double> WalkForwardResult::prediction_values() const {
    std::vector<double> out;
    out.reserve(predictions.size());
    for (const auto& p : predictions) out.push_back(p.prediction);
    return out;
}

std::vector<double> WalkForwardResult::actual_values() const {
    std::vector<double> out;
    out.reserve(predictions.size());
    for (const auto& p : predictions) out.push_back(p.actual);
    return out;
}

std::uint64_t WalkForwardResult::content_hash() const {
    std::uint64_t h = 0xcbf29ce484222325ULL;
    for (const auto& p : predictions) {
        const std::int64_t ns = p.feature_end_time.time_since_epoch().count();
        hash_bytes(h, &ns, sizeof(ns));
        const std::uint32_t inst = index_of(p.instrument);
        hash_bytes(h, &inst, sizeof(inst));
        // Bit patterns, so a one-ulp divergence between two runs is caught.
        hash_bytes(h, &p.prediction, sizeof(p.prediction));
        hash_bytes(h, &p.actual, sizeof(p.actual));
    }
    return h;
}

Result<WalkForwardResult> WalkForwardRunner::run(const features::FeatureMatrix& features,
                                                 std::span<const double> targets,
                                                 std::span<const validation::Fold> folds,
                                                 std::span<const double> weights) {
    if (prototype_ == nullptr) return fail(bad("runner has no model prototype"));
    if (targets.size() != features.rows()) {
        // A length mismatch here is the misalignment that looks like a signal
        // and is entirely lookahead.
        return fail(bad("target series length does not match the feature matrix"));
    }

    WalkForwardResult result;
    const auto keys = features.keys();

    for (const auto& fold : folds) {
        if (fold.train_rows.empty() || fold.test_rows.empty()) {
            ++result.folds_skipped;
            continue;
        }

        // A FRESH MODEL PER FOLD. Reusing a fitted instance would carry the
        // previous fold's parameters into this fold's starting point, which for
        // IRLS changes the answer and for any model means this fold saw the
        // previous fold's test data through its initialisation.
        Pipeline pipeline{prototype_->clone_unfitted(), cfg_};

        auto fitted = pipeline.fit(features, targets, fold.train_rows, weights);
        if (!fitted) {
            // A fold that cannot be fitted is SKIPPED and counted, never
            // silently dropped: a run that quietly evaluated half its folds
            // would report a number nobody could interpret.
            ++result.folds_skipped;
            continue;
        }

        auto predictions = pipeline.predict(features, fold.test_rows);
        if (!predictions) return fail(predictions.error());
        if (predictions->size() != fold.test_rows.size()) {
            return fail(bad("prediction count does not match the test row count"));
        }

        for (std::size_t i = 0; i < fold.test_rows.size(); ++i) {
            const std::size_t row = fold.test_rows[i];
            OosPrediction p;
            p.feature_end_time = row < keys.size() ? keys[row].feature_end_time : kNoTimestamp;
            p.instrument = row < keys.size() ? keys[row].instrument : kInvalidInstrument;
            p.prediction = (*predictions)[i];
            p.actual = targets[row];
            p.fold_id = fold.fold_id;
            p.is_test = true;
            result.predictions.push_back(p);
        }

        result.per_fold_diagnostics.push_back(pipeline.model().diagnostics());
        result.per_fold_parameter_hashes.push_back(pipeline.parameter_hash());
        ++result.folds_fitted;
    }

    if (result.folds_fitted == 0) {
        return fail(bad("no fold could be fitted: " + std::to_string(result.folds_skipped) +
                        " were skipped"));
    }
    return result;
}

}  // namespace ptl::models
