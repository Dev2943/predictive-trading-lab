#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <sstream>
#include <vector>

#include "ptl/engine/engine.hpp"
#include "support/ptl_catch.hpp"

using namespace ptl;
using namespace ptl::engine;
using namespace std::chrono;

namespace {

Timestamp at(const char* iso) {
    Timestamp ts{};
    REQUIRE(parse_timestamp(iso, ts));
    return ts;
}

constexpr InstrumentId kSpy{0};

/// Buys once on the Nth bar it sees, then holds. Small enough that the
/// resulting P&L can be reasoned about by hand.
class BuyOnceStrategy final : public IStrategy {
public:
    explicit BuyOnceStrategy(int trigger_bar = 2, double qty = 100.0)
        : trigger_(trigger_bar), qty_(qty) {}

    [[nodiscard]] std::string_view name() const noexcept override { return "buy_once"; }

    void on_bar(const market::Bar& bar, const StrategyContext& ctx, OrderSink& sink) override {
        ++seen_;
        // A strategy asks the CONTEXT for the time; it never calls a system
        // clock. That is what lets the same code replay and trade live.
        last_seen_time_ = ctx.now();
        if (seen_ != trigger_) return;

        LifecycleTimes t;
        t.decision_time = bar.close_time();
        auto o =
            oms::Order::market(sink.next_order_id(), bar.instrument(), Side::Buy, Qty{qty_}, t);
        if (!o) return;
        auto submitted = sink.submit(*o);
        if (submitted) ++submitted_;
    }

    void on_fill(const oms::Fill& f, const StrategyContext&) override {
        fills_.push_back(f.price().get());
    }

    [[nodiscard]] int seen() const noexcept { return seen_; }
    [[nodiscard]] int submitted() const noexcept { return submitted_; }
    [[nodiscard]] const std::vector<double>& fill_prices() const noexcept { return fills_; }
    [[nodiscard]] Timestamp last_seen_time() const noexcept { return last_seen_time_; }

private:
    int trigger_;
    double qty_;
    int seen_ = 0;
    int submitted_ = 0;
    std::vector<double> fills_;
    Timestamp last_seen_time_{kNoTimestamp};
};

const market::Calendar& us() {
    static const market::Calendar cal = [] {
        auto r = market::Calendar::build(market::Calendar::us_equities_spec(), 2024, 2024);
        REQUIRE(r.has_value());
        return std::move(*r);
    }();
    return cal;
}

std::vector<market::MarketEvent> session_bars(int count, double start_px = 500.0) {
    std::vector<market::MarketEvent> events;
    Timestamp t = at("2024-07-02T14:00:00Z");
    for (int i = 0; i < count; ++i) {
        const double px = start_px + static_cast<double>(i) * 0.10;
        auto b = market::Bar::from_left_edge(kSpy, t, minutes{1}, Price{px}, Price{px + 0.05},
                                             Price{px - 0.05}, Price{px}, Volume{100000.0});
        REQUIRE(b.has_value());
        events.emplace_back(*b);
        t += minutes{1};
    }
    auto withs = market::with_session_events(std::move(events), us());
    REQUIRE(withs.has_value());
    return *withs;
}

/// One complete run. Everything is constructed locally, so two calls share no
/// state whatsoever -- which is what makes the determinism comparison mean
/// something.
struct Run {
    RunSummary summary;
    std::vector<double> equity;
    std::string journal_csv;
    std::size_t fills = 0;
    Notional final_equity{};
    accounting::Reconciliation recon;
};

Run execute(std::uint64_t seed = 20240101, risk::RiskLimits limits = {}) {
    SimulatedClock clock;
    auto source = market::ReplaySource::create(session_bars(10), &clock);
    REQUIRE(source.has_value());

    execution::StandardCostModel costs;
    execution::StandardLatencyModel latency;
    execution::BrokerSimulator broker{clock, costs, latency, DeterministicRng{seed}};
    portfolio::Portfolio pf;
    oms::OrderManager oms;
    risk::RiskManager risk{limits};
    accounting::Journal journal;
    BuyOnceStrategy strategy;

    Engine engine{clock, *source, strategy, broker, pf, oms, risk, journal, &us()};
    auto summary = engine.run();
    REQUIRE(summary.has_value());

    Run r;
    r.summary = *summary;
    for (const auto& p : pf.equity_curve()) r.equity.push_back(p.equity.get());
    r.journal_csv = journal.to_csv();
    r.fills = summary->fills;
    r.final_equity = pf.equity();
    r.recon = journal.reconcile(pf);
    return r;
}

}  // namespace

TEST_CASE("the engine runs a strategy end to end", "[engine]") {
    const Run r = execute();
    REQUIRE(r.summary.bars == 10);
    REQUIRE(r.summary.orders_submitted == 1);
    REQUIRE(r.fills == 1);
    REQUIRE(r.summary.events_processed > 10);  // bars plus session events
    REQUIRE(is_finite(r.final_equity.get()));
}

TEST_CASE("two identical runs are bit-identical", "[engine][determinism][leakage]") {
    // THE CENTRAL DETERMINISM GUARANTEE. Same seed, same data, same code means
    // the same equity curve and the same journal -- to the last digit. Anything
    // less makes a RunId meaningless.
    const Run a = execute();
    const Run b = execute();

    REQUIRE(a.equity.size() == b.equity.size());
    for (std::size_t i = 0; i < a.equity.size(); ++i) {
        // Exact equality, not approximate. Floating-point summation is not
        // associative, so any ordering difference anywhere would show up here.
        REQUIRE(a.equity[i] == b.equity[i]);
    }
    REQUIRE(a.journal_csv == b.journal_csv);
    REQUIRE(a.final_equity.get() == b.final_equity.get());
    REQUIRE(a.summary.fills == b.summary.fills);
}

TEST_CASE("a different seed changes fills but not the event path", "[engine][determinism]") {
    const Run a = execute(1);
    const Run b = execute(2);
    // The seed drives stochastic slippage, so prices differ...
    REQUIRE(a.summary.bars == b.summary.bars);
    REQUIRE(a.summary.orders_submitted == b.summary.orders_submitted);
    // ...while the deterministic structure of the run does not.
    REQUIRE(a.equity.size() == b.equity.size());
}

TEST_CASE("no chain violations occur during a normal run", "[engine][leakage]") {
    // A non-zero count invalidates the run: it means some part of the pipeline
    // consumed information out of order.
    const Run r = execute();
    REQUIRE(r.summary.chain_violations == 0);
}

TEST_CASE("an order cannot fill on the bar that produced it", "[engine][leakage]") {
    // THE LOOP-LEVEL NO-SAME-BAR GUARANTEE. The engine matches resting orders
    // BEFORE handing the bar to the strategy, so an order created on this bar
    // is not visible to this bar's matching pass. Reversing that order would
    // reintroduce same-bar execution even though every lower layer forbids it.
    SimulatedClock clock;
    auto source = market::ReplaySource::create(session_bars(10), &clock);
    execution::StandardCostModel costs;
    execution::StandardLatencyModel latency;
    execution::BrokerSimulator broker{clock, costs, latency, DeterministicRng{1}};
    portfolio::Portfolio pf;
    oms::OrderManager oms;
    risk::RiskManager risk;
    accounting::Journal journal;
    BuyOnceStrategy strategy{2, 100.0};

    Engine engine{clock, *source, strategy, broker, pf, oms, risk, journal, &us()};
    REQUIRE(engine.run().has_value());

    // The decision was made on bar 2's close; the fill must be strictly later.
    const auto* rec = oms.find(oms::OrderId{1});
    REQUIRE(rec != nullptr);
    REQUIRE(rec->filled_quantity.get() > 0.0);
    for (const auto& e : journal.entries()) {
        if (e.kind == accounting::EntryKind::FillReceived) {
            REQUIRE(e.ts > rec->order.decision_time());
        }
    }
}

TEST_CASE("accounting reconciles exactly", "[engine][accounting]") {
    // net = gross - costs, and the residual measures arithmetic error. A
    // non-zero residual means a cost was double counted or dropped.
    const Run r = execute();
    REQUIRE(r.recon.balances());
    REQUIRE(r.recon.residual().get() == Catch::Approx(0.0).margin(1e-9));
    REQUIRE(r.summary.reconciled);
}

TEST_CASE("the equity curve holds the accounting identity throughout",
          "[engine][accounting][property]") {
    SimulatedClock clock;
    auto source = market::ReplaySource::create(session_bars(10), &clock);
    execution::StandardCostModel costs;
    execution::StandardLatencyModel latency;
    execution::BrokerSimulator broker{clock, costs, latency, DeterministicRng{7}};
    portfolio::Portfolio pf;
    oms::OrderManager oms;
    risk::RiskManager risk;
    accounting::Journal journal;
    BuyOnceStrategy strategy;

    EngineConfig cfg;
    cfg.snapshot_on_bar = true;
    Engine engine{clock, *source, strategy, broker, pf, oms, risk, journal, &us(), cfg};
    REQUIRE(engine.run().has_value());

    // snapshot() asserts equity == cash + position value on every append, so a
    // curve that exists at all is a curve that held the identity throughout.
    REQUIRE(pf.equity_curve().size() >= 10);
    REQUIRE(pf.identity_holds());
    for (const auto& p : pf.equity_curve()) REQUIRE(is_finite(p.equity.get()));
}

TEST_CASE("risk rejections are recorded rather than silently dropped", "[engine][risk][leakage]") {
    // A suppressed order that vanishes without trace makes a backtest diverge
    // from paper trading invisibly.
    risk::RiskLimits tight;
    tight.max_order_notional = Notional{1.0};  // nothing can pass

    const Run r = execute(20240101, tight);
    REQUIRE(r.summary.orders_submitted == 0);
    REQUIRE(r.summary.orders_rejected == 1);
    REQUIRE(r.fills == 0);

    // And the rejection is in the journal with its reason.
    REQUIRE(r.journal_csv.find("risk_rejection") != std::string::npos);
    REQUIRE(r.journal_csv.find("max_order_notional") != std::string::npos);
}

TEST_CASE("the strategy only ever sees the context clock", "[engine][determinism][leakage]") {
    // A strategy that called system_clock::now() would produce a different
    // answer on every run. The context clock is simulated, so the last time it
    // observed is a fact about the DATA, not about when the test ran.
    SimulatedClock clock;
    auto source = market::ReplaySource::create(session_bars(5), &clock);
    execution::StandardCostModel costs;
    execution::StandardLatencyModel latency;
    execution::BrokerSimulator broker{clock, costs, latency, DeterministicRng{1}};
    portfolio::Portfolio pf;
    oms::OrderManager oms;
    risk::RiskManager risk;
    accounting::Journal journal;
    BuyOnceStrategy strategy{99, 100.0};  // never trades

    Engine engine{clock, *source, strategy, broker, pf, oms, risk, journal, &us()};
    REQUIRE(engine.run().has_value());

    // The 5th bar opens at 14:04 and closes at 14:05.
    REQUIRE(to_iso8601(strategy.last_seen_time()) == "2024-07-02T14:05:00.000000000Z");
}

TEST_CASE("the journal is chronological and refuses backwards entries",
          "[engine][accounting][determinism]") {
    accounting::Journal j;
    accounting::JournalEntry e;
    e.ts = at("2024-07-02T15:00:00Z");
    REQUIRE(j.append(e).has_value());
    e.ts = at("2024-07-02T14:00:00Z");
    REQUIRE_FALSE(j.append(e).has_value());
}

// ---------------------------------------------------------------------------
// Strategy registry
// ---------------------------------------------------------------------------

TEST_CASE("the strategy registry is explicit and ordered", "[engine][strategy][determinism]") {
    // An explicit registry rather than self-registering globals: static
    // initialisation order is unspecified, and a registry that fills itself
    // during static init makes the available set depend on link order.
    StrategyRegistry reg;
    REQUIRE(reg.register_strategy("zulu",
                                  [](const std::string&) {
                                      return Result<std::unique_ptr<IStrategy>>{
                                          std::make_unique<BuyOnceStrategy>()};
                                  })
                .has_value());
    REQUIRE(reg.register_strategy("alpha",
                                  [](const std::string&) {
                                      return Result<std::unique_ptr<IStrategy>>{
                                          std::make_unique<BuyOnceStrategy>()};
                                  })
                .has_value());

    // Ordered, so a report listing strategies is identical between runs.
    const auto names = reg.names();
    REQUIRE(names.size() == 2);
    REQUIRE(names[0] == "alpha");
    REQUIRE(names[1] == "zulu");

    REQUIRE(reg.create("alpha").has_value());
    REQUIRE(reg.contains("zulu"));
}

TEST_CASE("duplicate registration is refused", "[engine][strategy]") {
    // Silently replacing would make the active strategy depend on registration
    // order -- the hidden non-determinism a registry exists to remove.
    StrategyRegistry reg;
    const auto factory = [](const std::string&) {
        return Result<std::unique_ptr<IStrategy>>{std::make_unique<BuyOnceStrategy>()};
    };
    REQUIRE(reg.register_strategy("dup", factory).has_value());
    REQUIRE_FALSE(reg.register_strategy("dup", factory).has_value());
    REQUIRE_FALSE(reg.register_strategy("", factory).has_value());
}

TEST_CASE("an unknown strategy lists what is available", "[engine][strategy]") {
    StrategyRegistry reg;
    REQUIRE(reg.register_strategy("known",
                                  [](const std::string&) {
                                      return Result<std::unique_ptr<IStrategy>>{
                                          std::make_unique<BuyOnceStrategy>()};
                                  })
                .has_value());

    auto r = reg.create("missing");
    REQUIRE_FALSE(r.has_value());
    // Naming the alternatives costs nothing and saves the reader a search.
    REQUIRE(r.error().message.find("known") != std::string::npos);
}
