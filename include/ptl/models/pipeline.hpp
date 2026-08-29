#pragma once

/// \file pipeline.hpp
/// Scaler plus model as one unit, and the walk-forward runner.
///
/// BUNDLING IS THE POINT. A scaler and a model kept as separate variables can
/// drift apart: fit the scaler on one row set, the model on another, and
/// nothing complains. Pipeline::fit takes exactly ONE row set and fits both
/// from it, so the two cannot disagree about what "training" meant.

#include <cstdint>
#include <istream>
#include <memory>
#include <ostream>
#include <span>
#include <string>
#include <vector>

#include "ptl/core/result.hpp"
#include "ptl/features/matrix.hpp"
#include "ptl/models/model.hpp"
#include "ptl/models/scaler.hpp"
#include "ptl/validation/fold.hpp"

namespace ptl::models {

struct PipelineConfig {
    bool standardize = true;
    /// Clip standardised features at this many deviations. Fitted on training
    /// rows only -- clipping with full-sample quantiles is the same leak as an
    /// unfitted scaler.
    double clip_sigma = 0.0;
};

class Pipeline {
public:
    /// \param model borrowed for construction and cloned; the pipeline owns its
    ///        own unfitted instance so no caller can hold a reference to the
    ///        fitted parameters and mutate them.
    Pipeline(std::unique_ptr<IModel> model, PipelineConfig cfg = {})
        : model_(std::move(model)), cfg_(cfg) {}

    /// Fit scaler AND model on the same rows.
    ///
    /// One row set, one call. There is no way to fit the scaler on a different
    /// selection from the model.
    [[nodiscard]] Result<bool> fit(const features::FeatureMatrix& features,
                                   std::span<const double> targets,
                                   std::span<const std::size_t> train_rows,
                                   std::span<const double> weights = {});

    /// Predict for the given rows, applying the TRAINING statistics.
    [[nodiscard]] Result<std::vector<double>> predict(const features::FeatureMatrix& features,
                                                      std::span<const std::size_t> rows) const;

    [[nodiscard]] Result<double> predict_one(std::span<const double> raw_features) const;

    [[nodiscard]] bool fitted() const noexcept { return fitted_; }
    [[nodiscard]] const IModel& model() const noexcept { return *model_; }
    [[nodiscard]] const StandardScaler& scaler() const noexcept { return scaler_; }

    /// Model and scaler together. A model reloaded without its scaler would
    /// receive unstandardised inputs and produce confident nonsense.
    [[nodiscard]] Result<bool> save(std::ostream&) const;
    [[nodiscard]] Result<bool> load(std::istream&);

    [[nodiscard]] std::uint64_t parameter_hash() const noexcept;

private:
    std::unique_ptr<IModel> model_;
    StandardScaler scaler_;
    PipelineConfig cfg_;
    bool fitted_ = false;
};

// ---------------------------------------------------------------------------
// Walk-forward runner
// ---------------------------------------------------------------------------

/// One out-of-sample prediction.
struct OosPrediction {
    Timestamp feature_end_time{kNoTimestamp};
    InstrumentId instrument{kInvalidInstrument};
    double prediction = 0.0;
    double actual = 0.0;
    int fold_id = 0;
    /// True when the row came from the TEST set rather than validation.
    bool is_test = true;
};

struct WalkForwardResult {
    std::vector<OosPrediction> predictions;
    std::vector<ModelDiagnostics> per_fold_diagnostics;
    std::vector<std::uint64_t> per_fold_parameter_hashes;
    std::size_t folds_fitted = 0;
    std::size_t folds_skipped = 0;

    /// Predictions only, in order. Feeds the Phase 5 evaluators directly.
    [[nodiscard]] std::vector<double> prediction_values() const;
    [[nodiscard]] std::vector<double> actual_values() const;
    /// Content hash over every prediction. Two identical runs must match.
    [[nodiscard]] std::uint64_t content_hash() const;
};

/// Fits one model per fold and collects out-of-sample predictions.
///
/// A FRESH MODEL PER FOLD, via clone_unfitted(). Reusing a fitted instance
/// would carry the previous fold's parameters into the next fold's starting
/// point -- for IRLS that changes the answer, and for any model it means fold k
/// saw data from fold k-1's test set through the initialisation.
class WalkForwardRunner {
public:
    WalkForwardRunner(std::unique_ptr<IModel> prototype, PipelineConfig cfg = {})
        : prototype_(std::move(prototype)), cfg_(cfg) {}

    [[nodiscard]] Result<WalkForwardResult> run(const features::FeatureMatrix& features,
                                                std::span<const double> targets,
                                                std::span<const validation::Fold> folds,
                                                std::span<const double> weights = {});

private:
    std::unique_ptr<IModel> prototype_;
    PipelineConfig cfg_;
};

}  // namespace ptl::models
