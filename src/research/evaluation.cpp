#include "ptl/research/evaluation.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <numeric>
#include <sstream>

namespace ptl::research {
namespace {

[[nodiscard]] Error bad(std::string message) {
    return make_error(ErrorCode::InvalidArgument, std::move(message));
}

/// Fractional ranks with ties averaged. Ordinal ranking would impose an
/// arbitrary order on equal values and make the statistic depend on input
/// order.
[[nodiscard]] std::vector<double> ranks_of(std::span<const double> v) {
    const std::size_t n = v.size();
    std::vector<std::size_t> idx(n);
    std::iota(idx.begin(), idx.end(), 0);
    std::sort(idx.begin(), idx.end(), [v](std::size_t a, std::size_t b) {
        if (v[a] != v[b]) return v[a] < v[b];
        return a < b;  // stable tie-break: pure function of the inputs
    });

    std::vector<double> r(n, 0.0);
    std::size_t i = 0;
    while (i < n) {
        std::size_t j = i;
        while (j + 1 < n && v[idx[j + 1]] == v[idx[i]]) ++j;
        const double avg = (static_cast<double>(i) + static_cast<double>(j)) * 0.5;
        for (std::size_t k = i; k <= j; ++k) r[idx[k]] = avg;
        i = j + 1;
    }
    return r;
}

}  // namespace

double Evaluator::pearson(std::span<const double> a, std::span<const double> b) {
    const std::size_t n = std::min(a.size(), b.size());
    if (n < 2) return 0.0;

    double ma = 0.0;
    double mb = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        ma += a[i];
        mb += b[i];
    }
    ma /= static_cast<double>(n);
    mb /= static_cast<double>(n);

    double cov = 0.0;
    double va = 0.0;
    double vb = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        const double da = a[i] - ma;
        const double db = b[i] - mb;
        cov += da * db;
        va += da * da;
        vb += db * db;
    }
    // A constant series has undefined correlation. Zero is the honest answer;
    // dividing would give inf and poison every aggregate built on it.
    if (va <= 0.0 || vb <= 0.0) return 0.0;
    const double r = cov / std::sqrt(va * vb);
    if (!is_finite(r)) return 0.0;
    return std::clamp(r, -1.0, 1.0);
}

double Evaluator::spearman(std::span<const double> a, std::span<const double> b) {
    const std::size_t n = std::min(a.size(), b.size());
    if (n < 2) return 0.0;
    const auto ra = ranks_of(a.subspan(0, n));
    const auto rb = ranks_of(b.subspan(0, n));
    return pearson(ra, rb);
}

Result<RegressionMetrics> Evaluator::regression(std::span<const double> predictions,
                                                std::span<const double> outcomes) {
    if (predictions.size() != outcomes.size()) {
        return fail(bad("prediction and outcome series lengths differ"));
    }
    if (predictions.empty()) return fail(bad("cannot evaluate an empty series"));

    RegressionMetrics m;
    m.n = predictions.size();

    double sse = 0.0;
    double sae = 0.0;
    double mean_y = 0.0;
    std::size_t directional_hits = 0;

    for (std::size_t i = 0; i < m.n; ++i) {
        if (!is_finite(predictions[i]) || !is_finite(outcomes[i])) {
            return fail(bad("non-finite value at index " + std::to_string(i)));
        }
        const double e = predictions[i] - outcomes[i];
        sse += e * e;
        sae += std::abs(e);
        mean_y += outcomes[i];
        // A zero outcome counts as a miss rather than a free hit.
        if ((predictions[i] > 0.0 && outcomes[i] > 0.0) ||
            (predictions[i] < 0.0 && outcomes[i] < 0.0)) {
            ++directional_hits;
        }
    }
    mean_y /= static_cast<double>(m.n);

    double sst = 0.0;
    for (const double y : outcomes) {
        const double d = y - mean_y;
        sst += d * d;
    }

    m.mse = sse / static_cast<double>(m.n);
    m.rmse = std::sqrt(m.mse);
    m.mae = sae / static_cast<double>(m.n);
    // NOT clamped at zero. A negative out-of-sample R² means the model is worse
    // than predicting the mean, which is a real and important finding.
    m.r_squared = sst > 0.0 ? 1.0 - sse / sst : 0.0;
    m.information_coefficient = pearson(predictions, outcomes);
    m.rank_information_coefficient = spearman(predictions, outcomes);
    m.directional_accuracy = static_cast<double>(directional_hits) / static_cast<double>(m.n);
    return m;
}

Result<ClassificationMetrics> Evaluator::classification(std::span<const double> predictions,
                                                        std::span<const double> outcomes,
                                                        double threshold) {
    if (predictions.size() != outcomes.size()) {
        return fail(bad("prediction and outcome series lengths differ"));
    }
    if (predictions.empty()) return fail(bad("cannot evaluate an empty series"));

    ClassificationMetrics m;
    m.n = predictions.size();

    for (std::size_t i = 0; i < m.n; ++i) {
        const bool predicted = predictions[i] >= threshold;
        const bool actual = outcomes[i] > 0.5;
        if (predicted && actual)
            ++m.true_positives;
        else if (predicted && !actual)
            ++m.false_positives;
        else if (!predicted && actual)
            ++m.false_negatives;
        else
            ++m.true_negatives;
    }

    const double correct = static_cast<double>(m.true_positives + m.true_negatives);
    m.accuracy = correct / static_cast<double>(m.n);
    const double pp = static_cast<double>(m.true_positives + m.false_positives);
    const double ap = static_cast<double>(m.true_positives + m.false_negatives);
    m.precision = pp > 0.0 ? static_cast<double>(m.true_positives) / pp : 0.0;
    m.recall = ap > 0.0 ? static_cast<double>(m.true_positives) / ap : 0.0;
    m.f1 = (m.precision + m.recall) > 0.0 ? 2.0 * m.precision * m.recall / (m.precision + m.recall)
                                          : 0.0;

    // AUC via the rank-sum identity: equivalent to the trapezoidal ROC area but
    // exact under ties, which matter when many predictions are identical.
    const auto r = ranks_of(predictions);
    double positive_rank_sum = 0.0;
    std::size_t n_pos = 0;
    for (std::size_t i = 0; i < m.n; ++i) {
        if (outcomes[i] > 0.5) {
            positive_rank_sum += r[i] + 1.0;  // ranks_of is zero-based
            ++n_pos;
        }
    }
    const std::size_t n_neg = m.n - n_pos;
    if (n_pos > 0 && n_neg > 0) {
        const double np = static_cast<double>(n_pos);
        const double nn = static_cast<double>(n_neg);
        m.auc = (positive_rank_sum - np * (np + 1.0) * 0.5) / (np * nn);
    } else {
        // One class absent: AUC is undefined. 0.5 is the honest "no information"
        // value rather than a flattering 1.0.
        m.auc = 0.5;
    }

    // Calibration in ten bins. A model can rank well and still be badly
    // calibrated, and position sizing depends on the probability being real.
    constexpr std::size_t kBins = 10;
    std::array<double, kBins> bin_pred{};
    std::array<double, kBins> bin_actual{};
    std::array<std::size_t, kBins> bin_count{};
    for (std::size_t i = 0; i < m.n; ++i) {
        const double p = std::clamp(predictions[i], 0.0, 1.0);
        auto b = static_cast<std::size_t>(p * static_cast<double>(kBins));
        if (b >= kBins) b = kBins - 1;
        bin_pred[b] += p;
        bin_actual[b] += outcomes[i] > 0.5 ? 1.0 : 0.0;
        ++bin_count[b];
    }
    double total_error = 0.0;
    std::size_t used = 0;
    for (std::size_t b = 0; b < kBins; ++b) {
        if (bin_count[b] == 0) continue;
        const double nb = static_cast<double>(bin_count[b]);
        total_error += std::abs(bin_pred[b] / nb - bin_actual[b] / nb);
        ++used;
    }
    m.calibration_error = used > 0 ? total_error / static_cast<double>(used) : 0.0;
    return m;
}

Result<RankingMetrics> Evaluator::ranking(std::span<const double> predictions,
                                          std::span<const double> outcomes, std::size_t buckets) {
    if (predictions.size() != outcomes.size()) {
        return fail(bad("prediction and outcome series lengths differ"));
    }
    if (predictions.size() < buckets || buckets < 2) {
        return fail(bad("need at least as many observations as buckets"));
    }

    RankingMetrics m;
    m.n = predictions.size();
    m.spearman = spearman(predictions, outcomes);

    // Kendall tau-a over pairs. O(n^2), so capped: this is a diagnostic, and a
    // quadratic scan over a million rows would dominate a report that nobody
    // asked to be slow.
    constexpr std::size_t kMaxPairs = 2000;
    const std::size_t limit = std::min(m.n, kMaxPairs);
    long long concordant = 0;
    long long discordant = 0;
    for (std::size_t i = 0; i < limit; ++i) {
        for (std::size_t j = i + 1; j < limit; ++j) {
            const double dp = predictions[i] - predictions[j];
            const double dy = outcomes[i] - outcomes[j];
            if (dp == 0.0 || dy == 0.0) continue;
            if ((dp > 0.0) == (dy > 0.0))
                ++concordant;
            else
                ++discordant;
        }
    }
    const long long total = concordant + discordant;
    m.kendall_tau =
        total > 0 ? static_cast<double>(concordant - discordant) / static_cast<double>(total) : 0.0;

    // Decile means: the most legible diagnostic in the project. A monotone
    // staircase is a working signal; anything else is visible at a glance.
    std::vector<std::size_t> idx(m.n);
    std::iota(idx.begin(), idx.end(), 0);
    std::sort(idx.begin(), idx.end(), [predictions](std::size_t a, std::size_t b) {
        if (predictions[a] != predictions[b]) return predictions[a] < predictions[b];
        return a < b;
    });

    m.decile_means.assign(buckets, 0.0);
    const double per_bucket = static_cast<double>(m.n) / static_cast<double>(buckets);
    for (std::size_t b = 0; b < buckets; ++b) {
        const auto lo = static_cast<std::size_t>(static_cast<double>(b) * per_bucket);
        const auto hi = static_cast<std::size_t>(static_cast<double>(b + 1) * per_bucket);
        double sum = 0.0;
        std::size_t count = 0;
        for (std::size_t k = lo; k < hi && k < m.n; ++k) {
            sum += outcomes[idx[k]];
            ++count;
        }
        m.decile_means[b] = count > 0 ? sum / static_cast<double>(count) : 0.0;
    }
    m.long_short_spread = m.decile_means.back() - m.decile_means.front();

    m.monotone = true;
    for (std::size_t b = 1; b < m.decile_means.size(); ++b) {
        if (m.decile_means[b] < m.decile_means[b - 1]) {
            m.monotone = false;
            break;
        }
    }
    return m;
}

Result<CrossSectionalEvaluation> Evaluator::cross_sectional(
    std::span<const double> predictions, std::span<const double> outcomes,
    std::span<const std::int64_t> period_ids, double periods_per_year) {
    if (predictions.size() != outcomes.size() || predictions.size() != period_ids.size()) {
        return fail(bad("prediction, outcome and period series lengths differ"));
    }
    if (predictions.empty()) return fail(bad("cannot evaluate an empty series"));

    // std::map so periods are visited in order: the per-period IC series must
    // be a pure function of the inputs.
    std::map<std::int64_t, std::pair<std::vector<double>, std::vector<double>>> by_period;
    for (std::size_t i = 0; i < predictions.size(); ++i) {
        auto& [p, o] = by_period[period_ids[i]];
        p.push_back(predictions[i]);
        o.push_back(outcomes[i]);
    }

    CrossSectionalEvaluation e;
    std::size_t positive = 0;
    for (const auto& [period, series] : by_period) {
        // A cross-section of one has no dispersion and no meaningful IC.
        if (series.first.size() < 2) continue;
        const double ic = spearman(series.first, series.second);
        e.per_period_ic.push_back(ic);
        if (ic > 0.0) ++positive;
    }

    e.periods = e.per_period_ic.size();
    if (e.periods == 0) return e;

    double sum = 0.0;
    for (const double ic : e.per_period_ic) sum += ic;
    e.mean_ic = sum / static_cast<double>(e.periods);

    if (e.periods >= 2) {
        double m2 = 0.0;
        for (const double ic : e.per_period_ic) {
            const double d = ic - e.mean_ic;
            m2 += d * d;
        }
        e.ic_stdev = std::sqrt(m2 / static_cast<double>(e.periods - 1));
    }
    // ICIR: the information ratio of the SIGNAL. A small but stable IC beats a
    // large erratic one, and a single pooled correlation cannot show that.
    if (e.ic_stdev > 0.0) {
        e.icir = e.mean_ic / e.ic_stdev * std::sqrt(periods_per_year);
    }
    e.hit_rate = static_cast<double>(positive) / static_cast<double>(e.periods);
    return e;
}

Result<double> Evaluator::permutation_importance(std::span<const double> baseline_predictions,
                                                 std::span<const double> outcomes,
                                                 std::span<const double> permuted_predictions) {
    if (baseline_predictions.size() != outcomes.size() ||
        permuted_predictions.size() != outcomes.size()) {
        return fail(bad("permutation importance series lengths differ"));
    }
    const double base = spearman(baseline_predictions, outcomes);
    const double permuted = spearman(permuted_predictions, outcomes);
    // Positive means shuffling the feature HURT, so the model was using it.
    // Negative means it helped, which is noise and worth reporting as such.
    return base - permuted;
}

std::string RegressionMetrics::describe() const {
    std::ostringstream ss;
    ss.precision(6);
    ss << std::fixed;
    ss << "regression over " << n << " observations\n";
    ss << "  RMSE                 " << rmse << '\n';
    ss << "  MAE                  " << mae << '\n';
    ss << "  R^2                  " << r_squared
       << (r_squared < 0.0 ? "  (worse than the mean)" : "") << '\n';
    ss << "  IC (Pearson)         " << information_coefficient << '\n';
    ss << "  rank IC (Spearman)   " << rank_information_coefficient << '\n';
    ss << "  directional accuracy " << directional_accuracy << '\n';
    return ss.str();
}

std::string ClassificationMetrics::describe() const {
    std::ostringstream ss;
    ss.precision(6);
    ss << std::fixed;
    ss << "classification over " << n << " observations\n";
    ss << "  accuracy    " << accuracy << '\n';
    ss << "  precision   " << precision << '\n';
    ss << "  recall      " << recall << '\n';
    ss << "  F1          " << f1 << '\n';
    ss << "  AUC         " << auc << '\n';
    ss << "  calibration " << calibration_error << '\n';
    return ss.str();
}

std::string RankingMetrics::describe() const {
    std::ostringstream ss;
    ss.precision(6);
    ss << std::fixed;
    ss << "ranking over " << n << " observations\n";
    ss << "  Spearman           " << spearman << '\n';
    ss << "  Kendall tau        " << kendall_tau << '\n';
    ss << "  long-short spread  " << long_short_spread << '\n';
    ss << "  monotone deciles   " << (monotone ? "yes" : "no") << '\n';
    ss << "  decile means      ";
    for (const double d : decile_means) ss << ' ' << d;
    ss << '\n';
    return ss.str();
}

std::string CrossSectionalEvaluation::describe() const {
    std::ostringstream ss;
    ss.precision(6);
    ss << std::fixed;
    ss << "cross-sectional evaluation over " << periods << " periods\n";
    ss << "  mean IC   " << mean_ic << '\n';
    ss << "  IC stdev  " << ic_stdev << '\n';
    ss << "  ICIR      " << icir << '\n';
    ss << "  hit rate  " << hit_rate << '\n';
    return ss.str();
}

}  // namespace ptl::research
