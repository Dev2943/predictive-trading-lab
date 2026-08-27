#include "ptl/features/feature_set.hpp"

#include <algorithm>
#include <cmath>
#include <sstream>

namespace ptl::features {
namespace {

/// Emission order. Anything appended must go at the END: inserting in the
/// middle shifts every later index and silently reinterprets every cached
/// matrix built with the old layout.
void build_names(const FeatureConfig& c, std::vector<std::string>& out, bool with_params) {
    const auto push = [&out, with_params](const std::string& base, std::size_t param) {
        out.push_back(with_params ? base + "(" + std::to_string(param) + ")"
                                  : base + "_" + std::to_string(param));
    };
    for (const auto lag : c.return_lags) push("ret", lag);
    for (const auto lag : c.reversal_lags) push("reversal", lag);
    for (const auto w : c.ma_windows) push("ma_dev", w);
    for (const auto w : c.volatility_windows) push("realized_vol", w);
    push("ret_zscore", c.zscore_window);
    push("atr", c.atr_period);
    push("relative_volume", c.volume_profile_lookback);
    push("spread_bps", c.spread_window);
    push("spread_zscore", c.spread_window);
    push("illiquidity", c.illiquidity_window);
    push("vol_regime", c.volatility_windows.empty() ? 60 : c.volatility_windows.front());
    push("vwap_dev", c.ma_windows.empty() ? 60 : c.ma_windows.front());
    out.emplace_back(with_params ? "session_vwap_dev()" : "session_vwap_dev");
    out.emplace_back(with_params ? "twap_vwap_gap()" : "twap_vwap_gap");
    if (c.include_seasonal) {
        out.emplace_back(with_params ? "seasonal_sin()" : "seasonal_sin");
        out.emplace_back(with_params ? "seasonal_cos()" : "seasonal_cos");
    }
    if (c.include_market_relative) {
        push("beta", c.beta_window);
        out.emplace_back(with_params ? "market_relative_return()" : "market_relative_return");
    }
}

}  // namespace

std::vector<std::string> feature_names(const FeatureConfig& c) {
    std::vector<std::string> out;
    build_names(c, out, false);
    return out;
}

std::vector<std::string> feature_signatures(const FeatureConfig& c) {
    std::vector<std::string> out;
    build_names(c, out, true);
    return out;
}

FeatureSet::FeatureSet(FeatureConfig cfg, InstrumentId instrument)
    : cfg_(std::move(cfg)),
      instrument_(instrument),
      return_z_(cfg_.zscore_window),
      atr_(cfg_.atr_period),
      volume_profile_(cfg_.volume_profile_slots, cfg_.volume_profile_lookback),
      spread_(cfg_.spread_window),
      illiquidity_(cfg_.illiquidity_window),
      vol_bucket_(cfg_.volatility_windows.empty() ? 60 : cfg_.volatility_windows.front()),
      beta_(cfg_.beta_window),
      rolling_vwap_(cfg_.ma_windows.empty() ? 60 : cfg_.ma_windows.front()),
      rolling_twap_(cfg_.ma_windows.empty() ? 60 : cfg_.ma_windows.front()) {
    for (const auto lag : cfg_.return_lags) returns_.emplace_back(lag);
    for (const auto lag : cfg_.reversal_lags) reversals_.emplace_back(lag);
    for (const auto w : cfg_.ma_windows) ma_devs_.emplace_back(w);
    for (const auto w : cfg_.volatility_windows) {
        vols_.emplace_back(w, cfg_.annualization_periods);
    }

    const auto names = feature_names(cfg_);
    // 64 features is the width of the ready mask. Exceeding it silently would
    // leave later features permanently unready, so it is a hard limit.
    values_.assign(std::min<std::size_t>(names.size(), 64), 0.0);
    full_mask_ = values_.size() >= 64 ? ~0ULL : (1ULL << values_.size()) - 1ULL;
    feature_set_id_ = compute_feature_set_id(feature_signatures(cfg_));
}

void FeatureSet::on_bar(const market::Bar& bar, const market::Session* session) noexcept {
    const double close = bar.close().get();
    if (!(close > 0.0) || !is_finite(close)) return;

    if (last_close_ > 0.0) {
        const double r = std::log(close / last_close_);
        if (is_finite(r)) {
            last_log_return_ = r;
            has_return_ = true;
        }
    }

    for (auto& f : returns_) f.update(close);
    for (auto& f : reversals_) f.update(close);
    for (auto& f : ma_devs_) f.update(close);
    if (has_return_) {
        for (auto& f : vols_) f.update(last_log_return_);
        return_z_.update(last_log_return_);
        illiquidity_.update(std::abs(last_log_return_), close * bar.volume().get());
    }
    atr_.update(bar.high().get(), bar.low().get(), close);
    if (!vols_.empty()) vol_bucket_.update(vols_.front().value());

    rolling_vwap_.update(close, bar.volume().get());
    rolling_twap_.update(close);
    session_vwap_.update(close, bar.volume().get());

    if (session != nullptr && session->is_open()) {
        const std::int32_t minute = minute_of_session(bar.close_time(), *session);
        // Relative volume is measured against the baseline BEFORE this bar is
        // folded in, then the baseline is updated. Updating first would compare
        // the bar against a baseline that already contains it.
        last_minute_ = minute;
        last_volume_ = bar.volume().get();
        volume_profile_.update(minute, bar.volume().get());
        if (cfg_.include_seasonal) {
            const auto len = session->length().count();
            const double progress =
                len > 0 ? static_cast<double>((bar.close_time() - session->open).count()) /
                              static_cast<double>(len)
                        : 0.0;
            seasonal_.update(progress);
        }
    }

    last_close_ = close;
    // The bar's CLOSE, never its open: that is the instant its contents came
    // into existence, and stamping the open would claim knowledge one interval
    // early.
    feature_end_time_ = bar.close_time();
    refresh();
}

void FeatureSet::on_quote(const market::Quote& quote) noexcept {
    spread_.update(quote.bid().get(), quote.ask().get());
    const Timestamp ts = quote.time().exchange_time;
    if (!is_set(feature_end_time_) || ts > feature_end_time_) feature_end_time_ = ts;
    refresh();
}

void FeatureSet::on_market_context(double market_log_return) noexcept {
    if (!is_finite(market_log_return)) return;
    pending_market_return_ = market_log_return;
    if (has_return_) beta_.update(market_log_return, last_log_return_);
    refresh();
}

void FeatureSet::on_session_open() noexcept {
    session_vwap_.on_session_open();
}

void FeatureSet::refresh() noexcept {
    std::size_t i = 0;
    std::uint64_t mask = 0;
    const auto set = [&](double v, bool ready) {
        if (i >= values_.size()) return;
        values_[i] = is_finite(v) ? v : 0.0;
        if (ready) mask |= (1ULL << i);
        ++i;
    };

    for (const auto& f : returns_) set(f.value(), f.ready());
    for (const auto& f : reversals_) set(f.value(), f.ready());
    for (const auto& f : ma_devs_) set(f.value(), f.ready());
    for (const auto& f : vols_) set(f.value(), f.ready());
    set(return_z_.value(), return_z_.ready());
    set(atr_.value(), atr_.ready());

    const std::int32_t minute = last_minute_;
    set(volume_profile_.relative_volume(minute, last_volume_), volume_profile_.ready(minute));
    set(spread_.current_bps(), spread_.ready());
    set(spread_.zscore(), spread_.ready());
    set(illiquidity_.value(), illiquidity_.ready());
    set(vol_bucket_.value(), vol_bucket_.ready());

    const double vwap = rolling_vwap_.value();
    set(vwap > 0.0 ? (last_close_ - vwap) / vwap : 0.0, rolling_vwap_.ready());
    const double svwap = session_vwap_.value();
    set(svwap > 0.0 ? (last_close_ - svwap) / svwap : 0.0, session_vwap_.ready());
    const double twap = rolling_twap_.value();
    set(twap > 0.0 && vwap > 0.0 ? (twap - vwap) / vwap : 0.0, rolling_twap_.ready());

    if (cfg_.include_seasonal) {
        set(seasonal_.sin_component, true);
        set(seasonal_.cos_component, true);
    }
    if (cfg_.include_market_relative) {
        set(beta_.value(), beta_.ready());
        set(has_return_ ? beta_.residual(pending_market_return_, last_log_return_) : 0.0,
            beta_.ready() && has_return_);
    }
    ready_mask_ = mask;
}

FeatureRow FeatureSet::row(std::uint64_t data_version) const noexcept {
    FeatureRow r;
    r.feature_end_time = feature_end_time_;
    r.instrument = instrument_;
    r.data_version = data_version;
    r.feature_set_id = feature_set_id_;
    r.ready_mask = ready_mask_;
    r.values = values_;
    return r;
}

void FeatureSet::reset() noexcept {
    for (auto& f : returns_) f.reset();
    for (auto& f : reversals_) f.reset();
    for (auto& f : ma_devs_) f.reset();
    for (auto& f : vols_) f.reset();
    return_z_.reset();
    atr_.reset();
    volume_profile_.reset();
    spread_.reset();
    illiquidity_.reset();
    vol_bucket_.reset();
    beta_.reset();
    session_vwap_.reset();
    rolling_vwap_.reset();
    rolling_twap_.reset();
    seasonal_ = SeasonalControls{};
    std::fill(values_.begin(), values_.end(), 0.0);
    ready_mask_ = 0;
    feature_end_time_ = kNoTimestamp;
    last_close_ = 0.0;
    last_log_return_ = 0.0;
    has_return_ = false;
    pending_market_return_ = 0.0;
    last_minute_ = -1;
    last_volume_ = 0.0;
}

}  // namespace ptl::features
