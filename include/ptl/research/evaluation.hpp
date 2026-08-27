#pragma once

/// \file evaluation.hpp
/// Prediction quality metrics.
///
/// R-SQUARED IS NOT THE HEADLINE HERE. Daily cross-sectional R² of 0.001-0.01
/// is normal and can be profitable; an R² of 0.4 on financial returns means a
/// bug, not alpha. The primary statistic is the INFORMATION COEFFICIENT -- the
/// correlation between prediction and realised forward return -- and its
/// stability over time.

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "ptl/core/result.hpp"
#include "ptl/core/types.hpp"

namespace ptl::research {

struct RegressionMetrics {
    std::size_t n = 0;
    double mse = 0.0;
    double rmse = 0.0;
    double mae = 0.0;
    /// Out-of-sample R² can be NEGATIVE -- worse than predicting the mean --
    /// and that is a meaningful result, not an error to clamp away.
    double r_squared = 0.0;
    /// Pearson correlation of prediction with outcome.
    double information_coefficient = 0.0;
    /// Spearman rank correlation. More robust to the heavy tails of returns,
    /// and the statistic to trust when the two disagree.
    double rank_information_coefficient = 0.0;
    /// Fraction of predictions whose sign matched the outcome.
    double directional_accuracy = 0.0;

    [[nodiscard]] std::string describe() const;
};

struct ClassificationMetrics {
    std::size_t n = 0;
    std::size_t true_positives = 0;
    std::size_t false_positives = 0;
    std::size_t true_negatives = 0;
    std::size_t false_negatives = 0;

    double accuracy = 0.0;
    double precision = 0.0;
    double recall = 0.0;
    double f1 = 0.0;
    /// Ranking quality, threshold-free. More informative than accuracy on the
    /// imbalanced classes financial labels usually produce.
    double auc = 0.0;
    /// Mean absolute deviation of predicted probability from realised
    /// frequency, in ten bins. A model can rank well and still be badly
    /// calibrated, and position sizing depends on calibration.
    double calibration_error = 0.0;

    [[nodiscard]] std::string describe() const;
};

struct RankingMetrics {
    std::size_t n = 0;
    double spearman = 0.0;
    double kendall_tau = 0.0;
    /// Mean outcome per prediction decile, lowest first. The single most
    /// legible diagnostic in the project: a monotone staircase is a working
    /// signal, and anything else is visible at a glance.
    std::vector<double> decile_means;
    /// Top decile mean minus bottom decile mean.
    double long_short_spread = 0.0;
    /// True when decile means increase monotonically.
    bool monotone = false;

    [[nodiscard]] std::string describe() const;
};

/// IC computed per cross-section and then averaged across time.
///
/// Pooling every observation into one correlation would let a few large
/// cross-sections dominate and would confound cross-sectional skill with
/// time-series skill. Averaging per-period ICs measures the thing the strategy
/// actually does.
struct CrossSectionalEvaluation {
    std::size_t periods = 0;
    double mean_ic = 0.0;
    double ic_stdev = 0.0;
    /// mean_ic / ic_stdev, annualised. The information ratio of the signal
    /// itself, and a far better guide to whether it is tradeable than a single
    /// pooled correlation.
    double icir = 0.0;
    double hit_rate = 0.0;
    std::vector<double> per_period_ic;

    [[nodiscard]] std::string describe() const;
};

class Evaluator {
public:
    [[nodiscard]] static Result<RegressionMetrics> regression(std::span<const double> predictions,
                                                              std::span<const double> outcomes);

    /// \param threshold probability above which a prediction counts as positive
    [[nodiscard]] static Result<ClassificationMetrics> classification(
        std::span<const double> predictions, std::span<const double> outcomes,
        double threshold = 0.5);

    [[nodiscard]] static Result<RankingMetrics> ranking(std::span<const double> predictions,
                                                        std::span<const double> outcomes,
                                                        std::size_t buckets = 10);

    /// \param period_ids groups observations into cross-sections; equal ids
    ///        belong to the same instant.
    [[nodiscard]] static Result<CrossSectionalEvaluation> cross_sectional(
        std::span<const double> predictions, std::span<const double> outcomes,
        std::span<const std::int64_t> period_ids, double periods_per_year = 252.0);

    /// Permutation importance: how much worse the IC gets when one feature's
    /// column is shuffled.
    ///
    /// Model-agnostic, and measures what the FITTED model actually uses rather
    /// than what a coefficient suggests it might. Deterministic: the shuffle is
    /// driven by an injected seeded generator, never by std::shuffle with a
    /// default engine.
    [[nodiscard]] static Result<double> permutation_importance(
        std::span<const double> baseline_predictions, std::span<const double> outcomes,
        std::span<const double> permuted_predictions);

    [[nodiscard]] static double pearson(std::span<const double> a, std::span<const double> b);
    [[nodiscard]] static double spearman(std::span<const double> a, std::span<const double> b);
};

}  // namespace ptl::research
