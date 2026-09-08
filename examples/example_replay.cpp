/// \file example_replay.cpp
/// A complete backtest: strategy, engine, results.
///
/// This is the smallest program that exercises the whole trading pipeline.
/// Everything a real strategy does -- features, models, optimization -- plugs
/// into the same on_bar hook shown here.

#include <cmath>
#include <iostream>
#include <vector>

#include "ptl/accounting/journal.hpp"
#include "ptl/analytics/performance_analyzer.hpp"
#include "ptl/engine/engine.hpp"
#include "ptl/execution/broker.hpp"
#include "ptl/market/source.hpp"

namespace {

constexpr ptl::InstrumentId kSpy{0};

/// Buys every tenth bar and flattens ten bars later. It has no edge; it exists
/// to make the machinery do something visible.
class ExampleStrategy final : public ptl::engine::IStrategy {
public:
    [[nodiscard]] std::string_view name() const noexcept override { return "example_momentum"; }

    [[nodiscard]] ptl::Result<bool> on_start(const ptl::engine::StrategyContext&) override {
        bars_ = 0;
        return true;
    }

    void on_bar(const ptl::market::Bar& bar, const ptl::engine::StrategyContext& ctx,
                ptl::engine::OrderSink& sink) override {
        ++bars_;
        if (bars_ % 10 != 0) return;

        const bool holding = ctx.position_of(bar.instrument()).get() > 0.0;
        const ptl::Side side = holding ? ptl::Side::Sell : ptl::Side::Buy;

        ptl::LifecycleTimes times;
        // FROM THE EVENT, never the wall clock. A strategy that reads
        // system_clock cannot be replayed.
        times.decision_time = bar.close_time();

        auto order = ptl::oms::Order::market(sink.next_order_id(), bar.instrument(), side,
                                             ptl::Qty{10}, times);
        if (!order) return;

        // submit() is the ONLY route to the venue, and it runs the risk gate.
        (void)sink.submit(order->with_arrival_price(bar.close()));
    }

private:
    std::size_t bars_ = 0;
};

/// A synthetic minute-bar session. Real runs load from the market layer.
[[nodiscard]] std::vector<ptl::market::MarketEvent> synthetic_session(
    const ptl::market::Calendar& calendar, std::size_t bars) {
    ptl::Timestamp date{};
    if (!ptl::parse_timestamp("2024-07-02", date)) {
        std::cerr << "cannot parse the session date\n";
        return {};
    }
    const auto day = calendar.session_on(date);
    if (!day) {
        // session_on returns an optional, not a Result: a date with no session
        // is a holiday, which is an absence rather than an error.
        std::cerr << "2024-07-02 is not a trading day on this calendar\n";
        return {};
    }

    std::vector<ptl::market::MarketEvent> events;
    ptl::Timestamp t = day->open;
    for (std::size_t i = 0; i < bars; ++i) {
        // The session is HALF-OPEN, so a bar whose close lands exactly on the
        // session close is outside it. Bounding generation by the close keeps
        // the last bar inside rather than one minute past.
        if (t + std::chrono::minutes{1} >= day->close) break;
        const double px = 500.0 + std::sin(static_cast<double>(i) * 0.1) * 3.0;
        auto bar = ptl::market::Bar::from_left_edge(
            kSpy, t, std::chrono::minutes{1}, ptl::Price{px}, ptl::Price{px + 0.05},
            ptl::Price{px - 0.05}, ptl::Price{px}, ptl::Volume{50'000.0});
        if (!bar) {
            std::cerr << "bar " << i << ": " << bar.error().message << '\n';
            return {};
        }
        events.emplace_back(*bar);
        t += std::chrono::minutes{1};
    }
    auto with_sessions = ptl::market::with_session_events(std::move(events), calendar);
    if (!with_sessions) {
        std::cerr << "session events: " << with_sessions.error().message << '\n';
        return {};
    }
    return *with_sessions;
}

}  // namespace

int main() {
    auto calendar =
        ptl::market::Calendar::build(ptl::market::Calendar::us_equities_spec(), 2024, 2024);
    if (!calendar) {
        std::cerr << "calendar: " << calendar.error().message << '\n';
        return 1;
    }

    const auto events = synthetic_session(*calendar, 390);
    if (events.empty()) {
        std::cerr << "no events generated\n";
        return 1;
    }

    // --- the injected trio: clock, source, broker -------------------------
    ptl::SimulatedClock clock;
    auto source = ptl::market::ReplaySource::create(events, &clock);
    if (!source) {
        std::cerr << "source: " << source.error().message << '\n';
        return 1;
    }

    ptl::execution::StandardCostModel costs;
    ptl::execution::StandardLatencyModel latency;
    ptl::execution::BrokerSimulator broker{clock, costs, latency, ptl::DeterministicRng{20240101}};

    // --- everything below is shared by replay, paper and live -------------
    ptl::portfolio::Portfolio portfolio;
    ptl::oms::OrderManager oms;
    ptl::risk::RiskLimits limits;
    limits.max_concentration = 1.0;
    ptl::risk::RiskManager risk{limits};
    ptl::accounting::Journal journal;
    ExampleStrategy strategy;

    ptl::engine::Engine engine{clock, *source, strategy, broker,    portfolio,
                               oms,   risk,    journal,  &*calendar};

    const auto summary = engine.run();
    if (!summary) {
        std::cerr << "run failed: " << summary.error().message << '\n';
        return 1;
    }

    std::cout << "events processed  " << summary->events_processed << '\n'
              << "orders submitted  " << summary->orders_submitted << '\n'
              << "orders rejected   " << summary->orders_rejected << '\n'
              << "fills             " << summary->fills << '\n'
              << "final equity      " << summary->final_equity.get() << '\n'
              << "chain violations  " << summary->chain_violations << '\n'
              << "reconciled        " << (summary->reconciled ? "yes" : "NO") << '\n';

    // `reconciled` is the one to check. It is false when the journal and the
    // portfolio disagree, which means the numbers above cannot be trusted.
    return summary->reconciled ? 0 : 1;
}
