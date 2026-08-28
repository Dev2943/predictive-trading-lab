#pragma once

/// \file feature_set.hpp
/// Per-instrument composition of streaming estimators.
///
/// Owns one instance of each configured estimator for ONE instrument, updates
/// them from a bar, and emits a positional value array with a ready mask. The
/// mask is the warmup gate: a feature that has not seen its full window
/// contributes a cleared bit, and a consumer that requires it will skip the row
/// rather than train on garbage.

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "ptl/core/result.hpp"
#include "ptl/features/bivariate.hpp"
#include "ptl/features/feature.hpp"
#include "ptl/features/intraday.hpp"
#include "ptl/features/momentum.hpp"
#include "ptl/market/calendar.hpp"

namespace ptl::features {

/// The initial signal library from the research. Windows are in BARS, so at
/// one-minute sampling 390 is one regular session.
struct FeatureConfig {
    std::vector<std::size_t> return_lags{1, 5, 15, 30, 60, 390};
    std::vector<std::size_t> reversal_lags{1, 5};
    std::vector<std::size_t> ma_windows{30, 60, 390};
    std::vector<std::size_t> volatility_windows{15, 60, 390};
    std::size_t zscore_window = 60;
    std::size_t atr_period = 14;
    std::size_t volume_profile_lookback = 20;
    std::size_t volume_profile_slots = 390;
    std::size_t spread_window = 60;
    std::size_t illiquidity_window = 60;
    std::size_t beta_window = 390;
    /// Annualisation for realised volatility: 252 * 390 for one-minute bars.
    double annualization_periods = 252.0 * 390.0;
    bool include_seasonal = true;
    bool include_market_relative = true;
};

/// Names, in emission order. Stable: the values array is positional, and a
/// reordering changes the feature_set_id and invalidates every cache entry.
[[nodiscard]] std::vector<std::string> feature_names(const FeatureConfig&);
[[nodiscard]] std::vector<std::string> feature_signatures(const FeatureConfig&);

/// All estimators for one instrument.
///
/// Values live in a single contiguous vector sized once at construction. There
/// is no allocation on the update path.
class FeatureSet {
public:
    explicit FeatureSet(FeatureConfig cfg, InstrumentId instrument);

    /// Fold one bar in. `session` supplies session-relative context; pass
    /// nullptr when no calendar is available, in which case seasonal and
    /// minute-of-day features stay unready rather than being silently faked.
    void on_bar(const market::Bar& bar, const market::Session* session) noexcept;

    void on_quote(const market::Quote& quote) noexcept;

    /// Market context for beta and relative return. Supplied by the
    /// cross-sectional stage AFTER every per-instrument set has been updated,
    /// which is what keeps it same-bar rather than lookahead.
    void on_market_context(double market_log_return) noexcept;

    void on_session_open() noexcept;

    [[nodiscard]] std::span<const double> values() const noexcept { return values_; }
    [[nodiscard]] std::uint64_t ready_mask() const noexcept { return ready_mask_; }
    [[nodiscard]] bool all_ready() const noexcept { return ready_mask_ == full_mask_; }
    [[nodiscard]] std::uint64_t full_mask() const noexcept { return full_mask_; }
    [[nodiscard]] std::size_t size() const noexcept { return values_.size(); }
    [[nodiscard]] InstrumentId instrument() const noexcept { return instrument_; }
    [[nodiscard]] std::uint64_t feature_set_id() const noexcept { return feature_set_id_; }

    /// Maximum information timestamp folded in so far. Emitted on every row as
    /// the lineage field, so leakage is detectable after the fact.
    [[nodiscard]] Timestamp feature_end_time() const noexcept { return feature_end_time_; }

    /// Most recent per-bar log return, for the market aggregate.
    [[nodiscard]] double last_log_return() const noexcept { return last_log_return_; }
    [[nodiscard]] bool has_return() const noexcept { return has_return_; }

    [[nodiscard]] FeatureRow row(std::uint64_t data_version) const noexcept;

    void reset() noexcept;

private:
    void refresh() noexcept;

    FeatureConfig cfg_;
    InstrumentId instrument_;
    std::uint64_t feature_set_id_ = 0;
    std::vector<double> values_;
    std::uint64_t ready_mask_ = 0;
    std::uint64_t full_mask_ = 0;
    Timestamp feature_end_time_{kNoTimestamp};

    std::vector<LaggedReturn> returns_;
    std::vector<ShortTermReversal> reversals_;
    std::vector<MaDeviation> ma_devs_;
    std::vector<RealizedVolatility> vols_;
    RollingZScore return_z_;
    AverageTrueRange atr_;
    MinuteOfDayVolumeProfile volume_profile_;
    SpreadStatistics spread_;
    AmihudIlliquidity illiquidity_;
    VolatilityBucket vol_bucket_;
    RollingBeta beta_;
    SessionVwap session_vwap_;
    RollingVwap rolling_vwap_;
    RollingTwap rolling_twap_;
    SeasonalControls seasonal_;

    double last_close_ = 0.0;
    double last_log_return_ = 0.0;
    bool has_return_ = false;
    double pending_market_return_ = 0.0;
    std::int32_t last_minute_ = -1;
    double last_volume_ = 0.0;
};

}  // namespace ptl::features
