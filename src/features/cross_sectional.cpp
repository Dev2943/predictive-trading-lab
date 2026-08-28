#include "ptl/features/cross_sectional.hpp"

#include <algorithm>
#include <cmath>
#include <numeric>

namespace ptl::features {
namespace {

[[nodiscard]] Error bad(std::string message, std::string context = {}) {
    return make_error(ErrorCode::ValidationFailed, std::move(message), std::move(context));
}

}  // namespace

double median_of(std::span<const double> values) {
    if (values.empty()) return 0.0;
    std::vector<double> v(values.begin(), values.end());
    // Restating the invariant AFTER the copy. It is implied by the check above,
    // but the optimiser cannot prove it survives the vector construction and
    // warns about a dereference it thinks might be null. Making the guarantee
    // local is better than suppressing the diagnostic, which would also hide a
    // genuine one later.
    if (v.empty()) return 0.0;
    const std::size_t mid = v.size() / 2;
    std::nth_element(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(mid), v.end());
    if (v.size() % 2 == 1) return v[mid];
    const double hi = v[mid];
    // For an even count, nth_element leaves everything below mid unsorted, so
    // the lower middle is the max of the left partition -- not v[mid-1].
    const double lo = *std::max_element(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(mid));
    return (lo + hi) * 0.5;
}

std::vector<double> percentile_ranks(std::span<const double> values) {
    const std::size_t n = values.size();
    std::vector<double> out(n, 0.5);
    if (n == 0) return out;
    if (n == 1) return out;

    std::vector<std::size_t> idx(n);
    std::iota(idx.begin(), idx.end(), 0);
    // Stable tie-break on the original index, so the ordering is a pure
    // function of the inputs regardless of the sort implementation.
    std::sort(idx.begin(), idx.end(), [values](std::size_t a, std::size_t b) {
        if (values[a] != values[b]) return values[a] < values[b];
        return a < b;
    });

    std::size_t i = 0;
    while (i < n) {
        std::size_t j = i;
        while (j + 1 < n && values[idx[j + 1]] == values[idx[i]]) ++j;
        // Average rank across the tied block. Ordinal ranking would impose an
        // arbitrary order on equal values -- common before warmup, when many
        // features are still zero -- and make the output depend on input order.
        const double avg_rank = (static_cast<double>(i) + static_cast<double>(j)) * 0.5;
        const double pct = avg_rank / static_cast<double>(n - 1);
        for (std::size_t k = i; k <= j; ++k) out[idx[k]] = pct;
        i = j + 1;
    }
    return out;
}

std::vector<double> cross_sectional_zscore(std::span<const double> values) {
    const std::size_t n = values.size();
    std::vector<double> out(n, 0.0);
    if (n < 2) return out;

    double mean = 0.0;
    for (const double v : values) mean += v;
    mean /= static_cast<double>(n);

    double m2 = 0.0;
    for (const double v : values) {
        const double d = v - mean;
        m2 += d * d;
    }
    const double sd = std::sqrt(m2 / static_cast<double>(n - 1));
    // A cross-section where every value is identical has no dispersion. All
    // zeros is the honest answer; dividing would give infinities across the
    // entire universe at once.
    if (sd <= 0.0 || !is_finite(sd)) return out;

    for (std::size_t i = 0; i < n; ++i) {
        const double z = (values[i] - mean) / sd;
        out[i] = is_finite(z) ? z : 0.0;
    }
    return out;
}

std::vector<double> winsorize(std::span<const double> values, double lower_quantile,
                              double upper_quantile) {
    std::vector<double> out(values.begin(), values.end());
    const std::size_t n = out.size();
    if (n < 3) return out;
    if (!(lower_quantile >= 0.0 && upper_quantile <= 1.0 && lower_quantile < upper_quantile)) {
        return out;
    }

    std::vector<double> sorted(values.begin(), values.end());
    std::sort(sorted.begin(), sorted.end());
    // NEAREST-RANK, not truncation. Truncating puts quantile 0.15 of seven
    // values at index 0 -- which clips nothing at all, so a caller asking to
    // winsorise the tails would silently get their outliers back. Rounding is
    // symmetric between the two bounds and does what the caller asked.
    const auto lo_idx =
        static_cast<std::size_t>(std::llround(lower_quantile * static_cast<double>(n - 1)));
    const auto hi_idx =
        static_cast<std::size_t>(std::llround(upper_quantile * static_cast<double>(n - 1)));
    const double lo = sorted[lo_idx];
    const double hi = sorted[hi_idx];

    for (auto& v : out) {
        if (!is_finite(v)) {
            v = 0.0;
            continue;
        }
        // Clipped to the boundary, not dropped: the cross-section keeps its
        // size and every instrument still receives a value.
        v = std::clamp(v, lo, hi);
    }
    return out;
}

std::vector<double> demean(std::span<const double> values) {
    std::vector<double> out(values.begin(), values.end());
    if (out.empty()) return out;
    double mean = 0.0;
    for (const double v : out) mean += v;
    mean /= static_cast<double>(out.size());
    if (!is_finite(mean)) return out;
    for (auto& v : out) v -= mean;
    return out;
}

std::vector<double> group_demean(std::span<const double> values,
                                 std::span<const std::int32_t> groups) {
    std::vector<double> out(values.begin(), values.end());
    if (out.size() != groups.size() || out.empty()) return out;

    // std::map for deterministic iteration over groups.
    std::map<std::int32_t, std::pair<double, std::size_t>> sums;
    for (std::size_t i = 0; i < out.size(); ++i) {
        auto& [sum, count] = sums[groups[i]];
        sum += out[i];
        ++count;
    }
    for (std::size_t i = 0; i < out.size(); ++i) {
        const auto& [sum, count] = sums[groups[i]];
        if (count == 0) continue;
        const double mean = sum / static_cast<double>(count);
        if (is_finite(mean)) out[i] -= mean;
    }
    return out;
}

CrossSectionalStage::CrossSectionalStage(CrossSectionalConfig cfg) : cfg_(std::move(cfg)) {}

Result<bool> CrossSectionalStage::contribute(InstrumentId instrument, Timestamp feature_end_time,
                                             double log_return, double volume) {
    if (!is_set(feature_end_time)) {
        return fail(bad("cross-sectional contribution has no feature_end_time"));
    }
    if (!is_set(bar_time_)) {
        bar_time_ = feature_end_time;
    } else if (feature_end_time != bar_time_) {
        // THE BARRIER'S CENTRAL CHECK. A contribution stamped differently from
        // the rest of the bar means we are about to mix one instrument's state
        // with another instrument's state from a DIFFERENT instant. If the
        // newcomer is later, that is lookahead; if earlier, it is a stale value
        // silently presented as current. Both are refused.
        return fail(bad("cross-sectional contribution is not from the current bar",
                        to_iso8601(feature_end_time) + " vs " + to_iso8601(bar_time_)));
    }

    const auto it =
        std::find_if(cfg_.universe.begin(), cfg_.universe.end(),
                     [instrument](const UniverseMember& m) { return m.instrument == instrument; });
    if (it == cfg_.universe.end()) {
        return fail(bad("instrument is not a member of the configured universe"));
    }
    if (pending_.contains(index_of(instrument))) {
        return fail(bad("duplicate cross-sectional contribution for one bar"));
    }

    Contribution c;
    c.instrument = instrument;
    c.sector = it->sector;
    c.log_return = is_finite(log_return) ? log_return : 0.0;
    c.volume = is_finite(volume) ? volume : 0.0;
    c.is_proxy = it->is_market_proxy;
    pending_.emplace(index_of(instrument), c);
    return true;
}

std::optional<double> CrossSectionalStage::market_return() const noexcept {
    for (const auto& [key, c] : pending_) {
        if (c.is_proxy) return c.log_return;
    }
    return std::nullopt;
}

Result<std::vector<CrossSectionalRow>> CrossSectionalStage::compute() {
    if (pending_.empty()) return std::vector<CrossSectionalRow>{};

    if (cfg_.require_complete_universe && pending_.size() != cfg_.universe.size()) {
        // A partial cross-section silently changes what a rank means: being
        // top-decile among four names is not being top-decile among nine.
        return fail(bad("incomplete cross-section: " + std::to_string(pending_.size()) + " of " +
                        std::to_string(cfg_.universe.size()) + " contributed"));
    }

    // std::map iteration is ordered by instrument index, so the vectors below
    // are built in a deterministic order regardless of arrival sequence.
    std::vector<InstrumentId> ids;
    std::vector<double> returns;
    std::vector<double> volumes;
    std::vector<std::int32_t> sectors;
    ids.reserve(pending_.size());
    returns.reserve(pending_.size());
    volumes.reserve(pending_.size());
    sectors.reserve(pending_.size());

    for (const auto& [key, c] : pending_) {
        ids.push_back(c.instrument);
        returns.push_back(c.log_return);
        volumes.push_back(c.volume);
        sectors.push_back(c.sector);
    }

    std::vector<double> work =
        cfg_.winsorize_inputs ? winsorize(returns, cfg_.winsor_lower, cfg_.winsor_upper) : returns;

    const auto proxy = market_return();
    // Market-relative: subtract the proxy when one exists, otherwise the
    // cross-sectional mean. Both are honest definitions of "relative"; which
    // one was used is recorded by the universe configuration.
    std::vector<double> relative(work.size(), 0.0);
    if (proxy.has_value()) {
        for (std::size_t i = 0; i < work.size(); ++i) relative[i] = work[i] - *proxy;
    } else {
        relative = demean(work);
    }

    const std::vector<double> sector_relative =
        cfg_.sector_neutral ? group_demean(work, sectors) : relative;

    const auto ranks = percentile_ranks(work);
    const auto zscores = cross_sectional_zscore(work);
    const auto vol_ranks = percentile_ranks(volumes);

    std::vector<CrossSectionalRow> out;
    out.reserve(ids.size());
    for (std::size_t i = 0; i < ids.size(); ++i) {
        CrossSectionalRow r;
        r.instrument = ids[i];
        r.feature_end_time = bar_time_;
        r.market_relative_return = relative[i];
        r.sector_relative_return = sector_relative[i];
        r.return_rank = ranks[i];
        r.return_zscore = zscores[i];
        r.volume_rank = vol_ranks[i];
        r.ready = true;
        out.push_back(r);
    }

    clear();
    return out;
}

void CrossSectionalStage::clear() noexcept {
    pending_.clear();
    bar_time_ = kNoTimestamp;
}

}  // namespace ptl::features
