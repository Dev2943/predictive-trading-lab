#pragma once

/// \file engine.hpp
/// The deterministic event loop shared by replay and live.
///
/// THE PARITY MECHANISM. This class is constructed with a clock, an event
/// source and a venue. A backtest passes SimulatedClock + ReplaySource +
/// BrokerSimulator; a paper session passes WallClock + a streaming source + a
/// broker adapter. Nothing else differs, because nothing else CAN differ --
/// the loop below is the only implementation.
///
/// Determinism guarantees:
///   - events are consumed in strict receive-time order;
///   - the clock is advanced by the source, never by the strategy;
///   - RNG streams are forked per consumer from the run seed;
///   - every container iterated in the loop is ordered, never hash-ordered.

#include <memory>
#include <vector>

#include "ptl/accounting/journal.hpp"
#include "ptl/analytics/metrics.hpp"
#include "ptl/core/clock.hpp"
#include "ptl/engine/strategy.hpp"
#include "ptl/execution/broker.hpp"
#include "ptl/market/source.hpp"
#include "ptl/portfolio/portfolio.hpp"
#include "ptl/risk/risk_manager.hpp"

namespace ptl::engine {

struct EngineConfig {
    /// Snapshot the equity curve at every session close. Off means snapshot on
    /// every bar, which is finer but far larger.
    bool snapshot_on_session_close = true;
    bool snapshot_on_bar = false;
    /// Abort on the first risk rejection rather than counting it. Off by
    /// default: rejections are normal and are reported.
    bool halt_on_risk_rejection = false;
    /// Volume assumed per interval when a bar carries none.
    Volume default_interval_volume{0.0};
};

struct RunSummary {
    std::size_t events_processed = 0;
    std::size_t bars = 0;
    std::size_t quotes = 0;
    std::size_t orders_submitted = 0;
    std::size_t orders_rejected = 0;
    std::size_t fills = 0;
    std::uint64_t chain_violations = 0;
    Timestamp first_event{kNoTimestamp};
    Timestamp last_event{kNoTimestamp};
    Notional final_equity{};
    bool reconciled = false;
};

class Engine {
public:
    /// All collaborators are BORROWED and must outlive the engine. Constructor
    /// injection rather than internal construction, so a test can substitute
    /// any one of them.
    Engine(IClock& clock, market::IMarketDataSource& source, IStrategy& strategy,
           execution::BrokerSimulator& broker, portfolio::Portfolio& pf, oms::OrderManager& oms,
           risk::RiskManager& risk, accounting::Journal& journal,
           const market::Calendar* calendar = nullptr, EngineConfig cfg = {});

    /// Run to completion. The Phase 3 entry point, behaviour unchanged.
    [[nodiscard]] Result<RunSummary> run();

    /// Stepped execution: begin() -> step()* -> finish().
    ///
    /// WHY THIS EXISTS. A paper session runs continuously and must do work
    /// BETWEEN events -- persist state, honour a pause, roll the trading day,
    /// answer a stop request. run() is all-or-nothing and cannot express that.
    ///
    /// The only alternative would be a second dispatch loop inside the session,
    /// duplicating the strategy dispatch, risk gating and fill routing this
    /// class already owns. That is the precise failure the one-loop rule exists
    /// to prevent, and it is worse than widening this interface.
    ///
    /// run() is implemented in terms of these, so there is still exactly ONE
    /// dispatch loop; the caller merely owns its cadence. Determinism is
    /// unaffected: step() is a bounded run(), and neither the events consumed
    /// nor their order depends on the chunk size.
    [[nodiscard]] Result<bool> begin();
    [[nodiscard]] Result<std::size_t> step(std::size_t max_events);
    [[nodiscard]] Result<RunSummary> finish();

    [[nodiscard]] const RunSummary& summary() const noexcept { return summary_; }

private:
    class Sink;

    [[nodiscard]] Result<bool> handle_bar(const market::Bar&, Sink&);
    [[nodiscard]] Result<bool> handle_quote(const market::Quote&, Sink&);
    [[nodiscard]] Result<bool> dispatch_fills(std::vector<oms::Fill>&&);

    IClock* clock_;
    market::IMarketDataSource* source_;
    IStrategy* strategy_;
    execution::BrokerSimulator* broker_;
    portfolio::Portfolio* pf_;
    oms::OrderManager* oms_;
    risk::RiskManager* risk_;
    accounting::Journal* journal_;
    const market::Calendar* calendar_;
    EngineConfig cfg_;
    RunSummary summary_;
    /// Guards step() against being called before begin().
    bool started_ = false;

    // Per-instrument market state, kept between events so the venue always has
    // the most recent tradable prices. std::map for deterministic iteration.
    std::map<std::uint32_t, execution::MarketState> state_;
    std::map<std::uint32_t, Timestamp> last_update_;
    Notional peak_equity_{};
    Notional turnover_today_{};
};

}  // namespace ptl::engine
