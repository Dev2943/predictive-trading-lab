#include "ptl/labels/label.hpp"

#include <algorithm>
#include <cmath>
#include <numeric>
#include <sstream>

namespace ptl::labels {
namespace {

[[nodiscard]] Error bad(std::string message, std::string context = {}) {
    return make_error(ErrorCode::ValidationFailed, std::move(message), std::move(context));
}

/// Trailing realised volatility of log returns, using ONLY observations up to
/// and including `index`. Causal by construction: the normaliser must be
/// knowable at the decision instant, or the label leaks.
[[nodiscard]] double trailing_vol(std::span<const PricePoint> prices, std::size_t index,
                                  std::size_t window) {
    if (index == 0 || window == 0) return 0.0;
    const std::size_t first = index >= window ? index - window + 1 : 1;
    double sumsq = 0.0;
    std::size_t n = 0;
    for (std::size_t i = first; i <= index; ++i) {
        const double a = prices[i - 1].mid.get();
        const double b = prices[i].mid.get();
        if (a <= 0.0 || b <= 0.0) continue;
        const double r = std::log(b / a);
        if (!is_finite(r)) continue;
        sumsq += r * r;
        ++n;
    }
    if (n == 0) return 0.0;
    return std::sqrt(sumsq / static_cast<double>(n));
}

}  // namespace

std::string_view to_string(LabelKind k) noexcept {
    switch (k) {
        case LabelKind::ForwardLogReturn:
            return "forward_log_return";
        case LabelKind::VolNormalisedReturn:
            return "vol_normalised_return";
        case LabelKind::CostAwareReturn:
            return "cost_aware_return";
        case LabelKind::Direction:
            return "direction";
        case LabelKind::TripleBarrier:
            return "triple_barrier";
    }
    return "unknown";
}

std::string LabelConfig::signature() const {
    std::ostringstream ss;
    ss.precision(17);
    ss << to_string(kind) << "|h=" << horizon << "|vw=" << vol_window
       << "|winsor=" << (winsorize ? winsor_sigma : 0.0) << "|thr=" << direction_threshold
       << "|cost=" << round_trip_cost_bps << "|bu=" << barrier_upper_sigma
       << "|bl=" << barrier_lower_sigma;
    return ss.str();
}

std::uint64_t LabelConfig::hash() const {
    return fnv1a64(signature());
}

std::size_t LabelSet::valid_count() const noexcept {
    return static_cast<std::size_t>(
        std::count_if(labels.begin(), labels.end(), [](const Label& l) { return l.valid; }));
}

std::vector<std::size_t> LabelSet::valid_rows() const {
    std::vector<std::size_t> out;
    out.reserve(labels.size());
    for (std::size_t i = 0; i < labels.size(); ++i) {
        if (labels[i].valid) out.push_back(i);
    }
    return out;
}

std::vector<double> LabelSet::values() const {
    std::vector<double> out;
    out.reserve(labels.size());
    for (const auto& l : labels) out.push_back(l.value);
    return out;
}

std::vector<ObservationInterval> LabelSet::intervals() const {
    std::vector<ObservationInterval> out;
    out.reserve(labels.size());
    for (const auto& l : labels) out.push_back(l.interval);
    return out;
}

Result<LabelSet> LabelBuilder::build(std::span<const PricePoint> prices) const {
    if (cfg_.horizon == 0) return fail(bad("label horizon must be at least one bar"));

    for (std::size_t i = 1; i < prices.size(); ++i) {
        if (prices[i].ts <= prices[i - 1].ts) {
            // An unsorted series would look forward by accident: index i+h
            // would not be h bars in the future.
            return fail(
                bad("label price series is not strictly chronological", to_iso8601(prices[i].ts)));
        }
    }

    LabelSet out;
    out.config = cfg_;
    out.config_hash = cfg_.hash();
    out.labels.reserve(prices.size());

    const std::size_t h = cfg_.horizon;

    for (std::size_t i = 0; i < prices.size(); ++i) {
        Label l;
        l.instrument = prices[i].instrument;

        // All four stamps, always. Purging needs the label INTERVAL, and a
        // horizon longer than the decision step makes consecutive labels
        // overlap -- an endpoint comparison would leave contaminated rows.
        l.interval.sample_start_time = prices[0].ts;
        l.interval.feature_end_time = prices[i].ts;
        l.interval.label_start_time = prices[i].ts;

        // Rows near the end have no future to look at. Marked invalid rather
        // than silently given a shorter horizon, which would make the target
        // mean something different for the last h rows.
        if (i + h >= prices.size()) {
            l.interval.label_end_time = prices[i].ts + Duration{1};
            l.valid = false;
            out.labels.push_back(l);
            continue;
        }

        l.interval.label_end_time = prices[i + h].ts;

        const double p0 = prices[i].mid.get();
        const double p1 = prices[i + h].mid.get();
        if (!(p0 > 0.0) || !(p1 > 0.0) || !is_finite(p0) || !is_finite(p1)) {
            l.valid = false;
            out.labels.push_back(l);
            continue;
        }

        // MIDPRICE, never the close the simulator fills at. Coupling the target
        // to the execution price would mean a change to the fill model silently
        // changed what the model was trained to predict.
        const double raw = std::log(p1 / p0);
        if (!is_finite(raw)) {
            l.valid = false;
            out.labels.push_back(l);
            continue;
        }

        switch (cfg_.kind) {
            case LabelKind::ForwardLogReturn:
                l.value = raw;
                l.valid = true;
                break;

            case LabelKind::VolNormalisedReturn: {
                // Normaliser uses only data up to i: causal by construction.
                const double sigma = trailing_vol(prices, i, cfg_.vol_window);
                if (sigma <= 0.0) {
                    l.valid = false;
                } else {
                    l.value = raw / sigma;
                    l.valid = true;
                }
                break;
            }

            case LabelKind::CostAwareReturn: {
                const double cost = cfg_.round_trip_cost_bps * 1e-4;
                // The cost is subtracted from the MAGNITUDE, so a small move in
                // either direction becomes unprofitable rather than a small
                // negative move becoming attractive.
                const double net = std::abs(raw) - cost;
                l.value = net <= 0.0 ? 0.0 : std::copysign(net, raw);
                l.valid = true;
                break;
            }

            case LabelKind::Direction:
                l.value = raw > cfg_.direction_threshold ? 1.0 : 0.0;
                l.valid = true;
                break;

            case LabelKind::TripleBarrier: {
                const double sigma = trailing_vol(prices, i, cfg_.vol_window);
                if (sigma <= 0.0) {
                    l.valid = false;
                    break;
                }
                const double upper = cfg_.barrier_upper_sigma * sigma;
                const double lower = -cfg_.barrier_lower_sigma * sigma;

                // Walk FORWARD to the first barrier touch. This is the one
                // genuinely path-dependent label, and the reason it lives in
                // the isolated module: it reads the future bar by bar.
                int touched = 0;
                for (std::size_t j = i + 1; j <= i + h; ++j) {
                    const double pj = prices[j].mid.get();
                    if (!(pj > 0.0)) continue;
                    const double r = std::log(pj / p0);
                    if (!is_finite(r)) continue;
                    if (r >= upper) {
                        touched = 1;
                        break;
                    }
                    if (r <= lower) {
                        touched = -1;
                        break;
                    }
                }
                // Neither barrier touched before the horizon expired: a genuine
                // third outcome, not a missing value.
                l.barrier_touched = touched;
                l.value = static_cast<double>(touched);
                l.valid = true;
                break;
            }
        }
        out.labels.push_back(l);
    }

    // Winsorise on the CONTINUOUS kinds only. Clipping a categorical label
    // would collapse its classes.
    const bool continuous = cfg_.kind == LabelKind::ForwardLogReturn ||
                            cfg_.kind == LabelKind::VolNormalisedReturn ||
                            cfg_.kind == LabelKind::CostAwareReturn;
    if (cfg_.winsorize && continuous && cfg_.winsor_sigma > 0.0) {
        double mean = 0.0;
        std::size_t n = 0;
        for (const auto& l : out.labels) {
            if (!l.valid) continue;
            mean += l.value;
            ++n;
        }
        if (n >= 2) {
            mean /= static_cast<double>(n);
            double m2 = 0.0;
            for (const auto& l : out.labels) {
                if (!l.valid) continue;
                const double d = l.value - mean;
                m2 += d * d;
            }
            const double sd = std::sqrt(m2 / static_cast<double>(n - 1));
            if (sd > 0.0) {
                const double lo = mean - cfg_.winsor_sigma * sd;
                const double hi = mean + cfg_.winsor_sigma * sd;
                for (auto& l : out.labels) {
                    if (l.valid) l.value = std::clamp(l.value, lo, hi);
                }
            }
        }
    }

    // Overlapping labels share information. Down-weighting by the overlap
    // factor stops a horizon longer than the step from silently multiplying the
    // effective sample size.
    const double overlap_weight = 1.0 / static_cast<double>(h);
    for (auto& l : out.labels) {
        if (l.valid) l.weight = overlap_weight;
    }
    return out;
}

Result<LabelSet> LabelBuilder::build_panel(
    std::span<const std::vector<PricePoint>> per_instrument) const {
    LabelSet out;
    out.config = cfg_;
    out.config_hash = cfg_.hash();

    for (const auto& series : per_instrument) {
        auto one = build(series);
        if (!one) return fail(one.error());
        out.labels.insert(out.labels.end(), one->labels.begin(), one->labels.end());
    }

    // Sort by decision time, then instrument. The tie-break is what makes the
    // pooled ordering a pure function of the inputs -- without it, two runs
    // could order simultaneous observations differently and float summation is
    // not associative.
    std::stable_sort(out.labels.begin(), out.labels.end(), [](const Label& a, const Label& b) {
        if (a.interval.feature_end_time != b.interval.feature_end_time) {
            return a.interval.feature_end_time < b.interval.feature_end_time;
        }
        return index_of(a.instrument) < index_of(b.instrument);
    });
    return out;
}

}  // namespace ptl::labels
