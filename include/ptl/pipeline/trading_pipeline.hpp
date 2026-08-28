#pragma once

/// \file trading_pipeline.hpp
/// The end-to-end trading strategy: prediction to order, in one event loop.
///
/// TradingPipeline IS an engine::IStrategy. It does not run its own loop, own a
/// clock, or read the event source; it receives bars from the Phase 3 engine
/// exactly as any other strategy does. That is what keeps replay and live
/// identical -- there is still only one loop in the system, and Phase 7 adds a
/// participant to it rather than a second one.
///
/// The mandated path, with NO BYPASS:
///
///     prediction -> signal -> filter -> sizing -> target -> rebalance
///                -> risk -> OMS -> broker
///
/// Orders reach the venue only through OrderSink::submit, which the engine
/// implements and which runs the Phase 3 risk gate on every order. The pipeline
/// holds no broker reference and cannot construct a Fill.

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "ptl/construction/rebalance.hpp"
#include "ptl/core/result.hpp"
#include "ptl/engine/strategy.hpp"
#include "ptl/signal/filter.hpp"
#include "ptl/signal/generator.hpp"
#include "ptl/sizing/sizer.hpp"

namespace ptl::pipeline {

/// Supplies a prediction for one instrument at one instant.
///
/// An interface rather than a model reference, so a backtest can replay
/// precomputed walk-forward predictions while a live session queries a fitted
/// model -- through the same pipeline, with no branch inside it.
class IPredictionSource {
public:
    IPredictionSource() = default;
    virtual ~IPredictionSource() = default;
    IPredictionSource(const IPredictionSource&) = delete;
    IPredictionSource& operator=(const IPredictionSource&) = delete;

protected:
    IPredictionSource(IPredictionSource&&) = default;
    IPredictionSource& operator=(IPredictionSource&&) = default;

public:
    /// \returns the prediction and the instant it was produced, or nullopt when
    ///          none is available. A source must NEVER return a prediction
    ///          stamped after `as_of`; the generator re-checks, but a source
    ///          that does so is broken.
    struct Prediction {
        double value = 0.0;
        Timestamp produced_at{kNoTimestamp};
    };
    [[nodiscard]] virtual std::optional<Prediction> predict_at(InstrumentId,
                                                               Timestamp as_of) const = 0;
};

/// Replays precomputed out-of-sample predictions.
///
/// Stores them per instrument in time order and returns the LATEST prediction
/// AT OR BEFORE the query instant -- never the nearest, which could be in the
/// future. That single choice is what makes replaying stored predictions safe.
class StoredPredictionSource final : public IPredictionSource {
public:
    [[nodiscard]] Result<bool> add(InstrumentId, Timestamp produced_at, double value);

    [[nodiscard]] std::optional<Prediction> predict_at(InstrumentId,
                                                       Timestamp as_of) const override;

    [[nodiscard]] std::size_t size() const noexcept { return count_; }

private:
    // Ordered by time within each instrument, so the lookup is a bounded search
    // rather than a scan, and the iteration order is deterministic.
    std::map<std::uint32_t, std::map<Timestamp, double>> by_instrument_;
    std::size_t count_ = 0;
};

/// Live performance statistics, accumulated as the run proceeds.
struct PipelineStats {
    std::size_t bars_seen = 0;
    std::size_t predictions_consumed = 0;
    std::size_t signals_generated = 0;
    std::size_t signals_actionable = 0;
    std::size_t signals_filtered = 0;
    std::size_t rebalances = 0;
    std::size_t orders_submitted = 0;
    std::size_t orders_rejected = 0;
    std::size_t fills_received = 0;

    Notional gross_turnover{};
    Notional realized_costs{};

    /// Signals whose realised move matched their direction.
    std::size_t signal_hits = 0;
    std::size_t signals_resolved = 0;

    Duration total_holding_period{Duration::zero()};
    std::size_t closed_positions = 0;

    double peak_gross_leverage = 0.0;
    double peak_net_leverage = 0.0;

    [[nodiscard]] double hit_rate() const noexcept;
    [[nodiscard]] Duration average_holding_period() const noexcept;
    [[nodiscard]] std::string describe() const;
};

struct PipelineConfig {
    /// Bars between rebalances. Rebalancing every bar pays the spread far more
    /// often than a 15-minute signal justifies.
    std::size_t rebalance_interval_bars = 5;

    /// Flatten everything before the close. A position held overnight is
    /// exposed to a gap the intraday model never described.
    bool flatten_at_session_close = true;

    signal::FilterConfig filters;
    sizing::SizingConfig sizing;
    construction::RebalanceConfig rebalance;

    /// Sector id per instrument, for the sector exposure limit.
    std::map<std::uint32_t, std::int32_t> sectors;
};

/// The Phase 7 strategy.
class TradingPipeline final : public engine::IStrategy {
public:
    /// \param generator borrowed; must outlive the pipeline.
    /// \param predictions borrowed; may be null for a pure rule strategy.
    /// \param calendar borrowed; may be null, in which case the trading-hours
    ///        filter is skipped and that is recorded.
    TradingPipeline(signal::ISignalGenerator& generator, IPredictionSource* predictions,
                    const market::Calendar* calendar, PipelineConfig cfg = {});

    [[nodiscard]] std::string_view name() const noexcept override { return "trading_pipeline"; }

    [[nodiscard]] Result<bool> on_start(const engine::StrategyContext&) override;
    void on_bar(const market::Bar&, const engine::StrategyContext&, engine::OrderSink&) override;
    void on_fill(const oms::Fill&, const engine::StrategyContext&) override;
    void on_session_open(Timestamp, const engine::StrategyContext&, engine::OrderSink&) override;
    void on_session_close(Timestamp, const engine::StrategyContext&, engine::OrderSink&) override;

    [[nodiscard]] const PipelineStats& stats() const noexcept { return stats_; }
    [[nodiscard]] const signal::SignalFilterChain& filters() const noexcept { return filters_; }
    [[nodiscard]] const std::vector<signal::Signal>& emitted_signals() const noexcept {
        return emitted_;
    }

    /// Hash over every signal and order the pipeline produced. Two identical
    /// runs must match.
    [[nodiscard]] std::uint64_t content_hash() const noexcept;

private:
    /// Per-instrument state the pipeline maintains between bars. All of it is
    /// derived from bars ALREADY SEEN; nothing here can reach forward.
    struct InstrumentState {
        Price last_close{};
        Timestamp last_bar_time{kNoTimestamp};
        double volatility = 0.0;
        double interval_volume = 0.0;
        double average_volume = 0.0;
        Bps spread_bps{0.0};
        /// Welford accumulators over log returns seen so far.
        double return_mean = 0.0;
        double return_m2 = 0.0;
        std::size_t return_count = 0;
        double previous_close = 0.0;
        /// Instant the current position was opened, for holding-period stats.
        Timestamp position_opened{kNoTimestamp};
    };

    void update_state(const market::Bar&);
    [[nodiscard]] signal::CostEstimate estimate_costs(const InstrumentState&,
                                                      double turnover_fraction) const;
    void rebalance(Timestamp, const engine::StrategyContext&, engine::OrderSink&, bool flatten);

    signal::ISignalGenerator* generator_;
    IPredictionSource* predictions_;
    const market::Calendar* calendar_;
    PipelineConfig cfg_;

    signal::SignalFilterChain filters_;
    sizing::PositionSizer sizer_;
    construction::RebalanceEngine rebalancer_;

    // std::map throughout: iteration order is part of the determinism contract.
    std::map<std::uint32_t, InstrumentState> state_;
    std::map<std::uint32_t, signal::Signal> latest_signals_;
    std::vector<signal::Signal> emitted_;

    PipelineStats stats_;
    std::size_t bars_since_rebalance_ = 0;
    Timestamp last_bar_time_{kNoTimestamp};
};

}  // namespace ptl::pipeline
