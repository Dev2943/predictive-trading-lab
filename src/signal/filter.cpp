#include "ptl/signal/filter.hpp"

#include <algorithm>
#include <cmath>
#include <sstream>

namespace ptl::signal {

std::string_view to_string(FilterReason r) noexcept {
    switch (r) {
        case FilterReason::Passed:
            return "passed";
        case FilterReason::LowConfidence:
            return "low_confidence";
        case FilterReason::NegativeNetEdge:
            return "negative_net_edge";
        case FilterReason::VolatilityOutOfRange:
            return "volatility_out_of_range";
        case FilterReason::InsufficientLiquidity:
            return "insufficient_liquidity";
        case FilterReason::SpreadTooWide:
            return "spread_too_wide";
        case FilterReason::OutsideTradingHours:
            return "outside_trading_hours";
        case FilterReason::CooldownActive:
            return "cooldown_active";
        case FilterReason::StaleFeatures:
            return "stale_features";
        case FilterReason::StalePrediction:
            return "stale_prediction";
        case FilterReason::NoMarketData:
            return "no_market_data";
    }
    return "unknown";
}

std::string FilterDecision::describe() const {
    std::string out{to_string(reason)};
    if (!detail.empty()) out += ": " + detail;
    return out;
}

namespace {

[[nodiscard]] FilterDecision reject(FilterReason r, std::string detail) {
    return FilterDecision{r, std::move(detail)};
}

[[nodiscard]] std::string seconds_of(Duration d) {
    return std::to_string(d.count() / 1'000'000'000) + "s";
}

}  // namespace

FilterDecision SignalFilterChain::evaluate(const Signal& signal, const FilterContext& ctx,
                                           const market::Calendar* calendar) const {
    // A flat signal is not a trade, so no filter applies to it. Passing it
    // through keeps "the model said stay out" distinguishable from "a filter
    // removed the signal", which the coverage statistics depend on.
    if (signal.is_flat()) return FilterDecision{};

    // --- data availability, first --------------------------------------------
    // Every check below reads market state. A filter evaluated without data
    // reports compliance it cannot support, which is worse than no filter.
    if (!ctx.has_market_data) {
        return reject(FilterReason::NoMarketData, "no market data for this instrument");
    }

    // --- staleness -----------------------------------------------------------
    if (ctx.feature_age > cfg_.max_feature_age) {
        return reject(FilterReason::StaleFeatures, "features are " + seconds_of(ctx.feature_age) +
                                                       " old, limit is " +
                                                       seconds_of(cfg_.max_feature_age));
    }
    if (ctx.prediction_age > cfg_.max_prediction_age) {
        // A prediction older than its own horizon describes a market that has
        // since moved on.
        return reject(FilterReason::StalePrediction,
                      "prediction is " + seconds_of(ctx.prediction_age) + " old, limit is " +
                          seconds_of(cfg_.max_prediction_age));
    }

    // --- edge ----------------------------------------------------------------
    if (signal.confidence() < cfg_.min_confidence) {
        return reject(FilterReason::LowConfidence, std::to_string(signal.confidence()) + " below " +
                                                       std::to_string(cfg_.min_confidence));
    }
    if (cfg_.require_positive_net_edge && signal.net_edge() <= 0.0) {
        // THE COST GATE. A signal whose expected move cannot pay for its own
        // round trip is a losing trade however confident the model is.
        return reject(FilterReason::NegativeNetEdge,
                      "expected " + std::to_string(signal.expected_return()) +
                          " does not cover costs of " + std::to_string(signal.costs().total()));
    }

    // --- market conditions ---------------------------------------------------
    if (ctx.realized_volatility < cfg_.min_volatility ||
        ctx.realized_volatility > cfg_.max_volatility) {
        return reject(FilterReason::VolatilityOutOfRange,
                      std::to_string(ctx.realized_volatility) + " outside [" +
                          std::to_string(cfg_.min_volatility) + ", " +
                          std::to_string(cfg_.max_volatility) + "]");
    }
    if (ctx.interval_volume < cfg_.min_interval_volume) {
        return reject(FilterReason::InsufficientLiquidity,
                      "interval volume " + std::to_string(ctx.interval_volume) + " below " +
                          std::to_string(cfg_.min_interval_volume));
    }
    if (ctx.average_volume > 0.0) {
        const double relative = ctx.interval_volume / ctx.average_volume;
        if (relative < cfg_.min_relative_volume) {
            // Trading a name that is barely printing means the fill assumptions
            // in the execution simulator do not hold.
            return reject(FilterReason::InsufficientLiquidity,
                          "relative volume " + std::to_string(relative) + " below " +
                              std::to_string(cfg_.min_relative_volume));
        }
    }
    if (ctx.spread_bps.get() > cfg_.max_spread_bps.get()) {
        return reject(FilterReason::SpreadTooWide, std::to_string(ctx.spread_bps.get()) +
                                                       " bps exceeds " +
                                                       std::to_string(cfg_.max_spread_bps.get()));
    }

    // --- session -------------------------------------------------------------
    if (calendar != nullptr) {
        const auto session = calendar->session_containing(ctx.now);
        if (!session.has_value()) {
            return reject(FilterReason::OutsideTradingHours,
                          "no trading session contains " + to_iso8601(ctx.now));
        }
        // The open and close auctions have different microstructure, and a
        // model trained on continuous trading does not describe them.
        if (ctx.now < session->open + cfg_.open_buffer) {
            return reject(FilterReason::OutsideTradingHours, "inside the opening buffer");
        }
        if (ctx.now >= session->close - cfg_.close_buffer) {
            return reject(FilterReason::OutsideTradingHours, "inside the closing buffer");
        }
    }

    // --- cooldown ------------------------------------------------------------
    const auto last = last_accepted_.find(index_of(signal.instrument()));
    if (last != last_accepted_.end() && is_set(last->second)) {
        const Duration since = ctx.now - last->second;
        if (since < cfg_.cooldown) {
            // Prevents a noisy model churning a position on consecutive bars,
            // which costs real money and produces no expected return.
            return reject(FilterReason::CooldownActive, seconds_of(since) +
                                                            " since the last signal, cooldown is " +
                                                            seconds_of(cfg_.cooldown));
        }
    }

    return FilterDecision{};
}

void SignalFilterChain::record(InstrumentId instrument, const FilterDecision& decision,
                               Timestamp now) {
    // Passes are counted alongside rejections: "how many signals did the chain
    // see?" is as useful a question as "how many did it stop?".
    ++counts_[static_cast<std::uint8_t>(decision.reason)];
    if (decision.passed()) last_accepted_[index_of(instrument)] = now;
}

std::size_t SignalFilterChain::rejection_count() const noexcept {
    std::size_t total = 0;
    for (const auto& [reason, n] : counts_) {
        if (reason != static_cast<std::uint8_t>(FilterReason::Passed)) total += n;
    }
    return total;
}

std::size_t SignalFilterChain::rejection_count(FilterReason r) const noexcept {
    const auto it = counts_.find(static_cast<std::uint8_t>(r));
    return it == counts_.end() ? 0 : it->second;
}

std::size_t SignalFilterChain::pass_count() const noexcept {
    return rejection_count(FilterReason::Passed);
}

Timestamp SignalFilterChain::last_signal_time(InstrumentId instrument) const noexcept {
    const auto it = last_accepted_.find(index_of(instrument));
    return it == last_accepted_.end() ? kNoTimestamp : it->second;
}

std::string SignalFilterChain::summary() const {
    std::ostringstream ss;
    ss << "signal filters: " << pass_count() << " passed, " << rejection_count() << " rejected\n";
    for (const auto& [reason, n] : counts_) {
        if (reason == static_cast<std::uint8_t>(FilterReason::Passed)) continue;
        ss << "  " << to_string(static_cast<FilterReason>(reason)) << ": " << n << '\n';
    }
    return ss.str();
}

void SignalFilterChain::reset() noexcept {
    counts_.clear();
    last_accepted_.clear();
}

}  // namespace ptl::signal
