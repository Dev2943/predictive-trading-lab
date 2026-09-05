#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <memory>
#include <vector>

#include "ptl/paper/session.hpp"
#include "support/ptl_catch.hpp"

using namespace ptl;
using namespace ptl::paper;
using namespace std::chrono;

namespace {

Timestamp at(const char* iso) {
    Timestamp ts{};
    REQUIRE(parse_timestamp(iso, ts));
    return ts;
}

constexpr InstrumentId kSpy{0};

const market::Calendar& us() {
    static const market::Calendar cal = [] {
        auto r = market::Calendar::build(market::Calendar::us_equities_spec(), 2024, 2024);
        REQUIRE(r.has_value());
        return std::move(*r);
    }();
    return cal;
}

std::vector<market::MarketEvent> session_events(std::size_t count) {
    const auto session = us().session_on(at("2024-07-02"));
    REQUIRE(session.has_value());

    std::vector<market::MarketEvent> events;
    Timestamp t = session->open;
    for (std::size_t i = 0; i < count; ++i) {
        const double px = 500.0 + std::sin(static_cast<double>(i) * 0.1) * 2.0;
        auto bar = market::Bar::from_left_edge(kSpy, t, minutes{1}, Price{px}, Price{px + 0.05},
                                               Price{px - 0.05}, Price{px}, Volume{50000.0});
        REQUIRE(bar.has_value());
        events.emplace_back(*bar);
        t += minutes{1};
    }
    auto with_sessions = market::with_session_events(std::move(events), us());
    REQUIRE(with_sessions.has_value());
    return *with_sessions;
}

/// Records every order and fill, so two runs can be compared exactly.
class RecordingStrategy final : public engine::IStrategy {
public:
    struct Record {
        Timestamp ts{kNoTimestamp};
        Side side{Side::Buy};
        double quantity = 0.0;
        std::uint64_t order_id = 0;
    };

    [[nodiscard]] std::string_view name() const noexcept override { return "paper_test"; }

    [[nodiscard]] Result<bool> on_start(const engine::StrategyContext&) override {
        bars_ = 0;
        return true;
    }

    void on_bar(const market::Bar& bar, const engine::StrategyContext& ctx,
                engine::OrderSink& sink) override {
        ++bars_;
        if (bars_ % 10 != 0) return;

        const Side side = (bars_ / 10) % 2 == 0 ? Side::Sell : Side::Buy;
        // Do not sell what we do not hold: the risk gate would refuse it, and
        // this test is about parity rather than rejection handling.
        if (side == Side::Sell && ctx.position_of(bar.instrument()).get() < 10.0) return;

        LifecycleTimes times;
        times.decision_time = bar.close_time();
        auto order =
            oms::Order::market(sink.next_order_id(), bar.instrument(), side, Qty{10.0}, times);
        if (!order) return;
        const auto priced = order->with_arrival_price(bar.close());
        if (sink.submit(priced)) {
            orders.push_back({bar.close_time(), side, 10.0, oms::value_of(priced.id())});
        }
    }

    void on_fill(const oms::Fill& fill, const engine::StrategyContext&) override {
        fills.push_back(
            {fill.fill_time(), fill.side(), fill.quantity().get(), oms::value_of(fill.order_id())});
    }

    std::vector<Record> orders;
    std::vector<Record> fills;

private:
    std::size_t bars_ = 0;
};

[[nodiscard]] risk::RiskLimits permissive_limits() {
    risk::RiskLimits limits;
    limits.max_concentration = 1.0;
    limits.max_daily_turnover = 100.0;
    return limits;
}

/// Everything a run needs, assembled once.
struct Harness {
    std::vector<market::MarketEvent> events;
    SimulatedClock clock;
    std::unique_ptr<market::ReplaySource> source;
    execution::StandardCostModel costs;
    execution::StandardLatencyModel latency;
    execution::BrokerSimulator simulator;
    portfolio::Portfolio portfolio;
    oms::OrderManager oms;
    risk::RiskManager risk;
    accounting::Journal journal;
    RecordingStrategy strategy;

    explicit Harness(std::size_t bars, std::uint64_t seed = 7)
        : events(session_events(bars)),
          simulator(clock, costs, latency, DeterministicRng{seed}),
          risk(permissive_limits()) {
        auto created = market::ReplaySource::create(events, &clock);
        REQUIRE(created.has_value());
        source = std::make_unique<market::ReplaySource>(std::move(*created));
    }
};

struct BacktestOutput {
    std::vector<RecordingStrategy::Record> orders;
    std::vector<RecordingStrategy::Record> fills;
    Notional final_equity{};
    std::string journal;
};

BacktestOutput run_backtest(std::size_t bars) {
    Harness h{bars};
    engine::Engine engine{h.clock, *h.source, h.strategy, h.simulator, h.portfolio,
                          h.oms,   h.risk,    h.journal,  &us()};
    auto summary = engine.run();
    REQUIRE(summary.has_value());

    BacktestOutput out;
    out.orders = h.strategy.orders;
    out.fills = h.strategy.fills;
    out.final_equity = h.portfolio.equity();
    out.journal = h.journal.to_csv();
    return out;
}

}  // namespace

// ---------------------------------------------------------------------------
// Account
// ---------------------------------------------------------------------------

TEST_CASE("the account is a view, never a second ledger", "[paper][account]") {
    portfolio::Portfolio portfolio;
    const PaperAccount account{portfolio};

    const auto snapshot = account.snapshot(at("2024-07-02T15:00:00Z"));
    // Every figure is READ from the portfolio, which maintains the identity the
    // journal reconciles against.
    REQUIRE(snapshot.cash.get() == portfolio.cash().get());
    REQUIRE(snapshot.equity.get() == portfolio.equity().get());
    REQUIRE(snapshot.status == AccountStatus::Active);
}

TEST_CASE("maintenance margin is charged on gross, not net", "[paper][account][property]") {
    // A market-neutral book still requires margin on both legs; netting would
    // understate the requirement to zero for a pair trade that can lose money.
    portfolio::Portfolio portfolio;
    MarginConfig config;
    config.maintenance_margin = 0.25;
    const PaperAccount account{portfolio, config};

    REQUIRE(account.maintenance_requirement().get() == Catch::Approx(0.0));
    REQUIRE(account.margin_excess().get() == Catch::Approx(portfolio.equity().get()));
    REQUIRE(account.status() == AccountStatus::Active);
    REQUIRE(account.available_buying_power().get() > 0.0);
}

TEST_CASE("a margin call still permits risk-reducing orders", "[paper][account][property]") {
    // Refusing them would trap the account in the very position that caused the
    // call, which is the opposite of what a margin call is for.
    portfolio::Portfolio portfolio{portfolio::PortfolioConfig{Notional{1000.0}}};
    MarginConfig config;
    config.buying_power_multiplier = 1.0;
    const PaperAccount account{portfolio, config};

    REQUIRE_FALSE(account.can_accept(Notional{1'000'000.0}, Side::Buy, false).has_value());
    REQUIRE(account.can_accept(Notional{1'000'000.0}, Side::Sell, true).has_value());
}

TEST_CASE("a suspended account accepts nothing", "[paper][account][edge]") {
    portfolio::Portfolio portfolio;
    PaperAccount account{portfolio};
    account.suspend();
    REQUIRE(account.status() == AccountStatus::Suspended);
    REQUIRE_FALSE(account.can_accept(Notional{1.0}, Side::Buy, true).has_value());
    account.resume();
    REQUIRE(account.status() == AccountStatus::Active);
}

TEST_CASE("margin interest is charged only on borrowed cash", "[paper][account][edge]") {
    // Interest EARNED on a positive balance belongs to the Phase 12 carry
    // model; computing it here too would double-count.
    portfolio::Portfolio portfolio;
    const PaperAccount account{portfolio};
    REQUIRE(account.accrue_interest(hours{24}).get() == Catch::Approx(0.0));
    REQUIRE(account.accrue_interest(Duration::zero()).get() == Catch::Approx(0.0));
}

// ---------------------------------------------------------------------------
// Broker
// ---------------------------------------------------------------------------

TEST_CASE("the broker refuses to submit while disconnected", "[paper][broker][leakage]") {
    Harness h{10};
    const PaperAccount account{h.portfolio};
    PaperBroker broker{h.clock, h.simulator, account};

    LifecycleTimes times;
    times.decision_time = h.clock.now();
    auto order = oms::Order::market(oms::OrderId{1}, kSpy, Side::Buy, Qty{10}, times);
    const auto priced = order->with_arrival_price(Price{500.0});

    REQUIRE_FALSE(broker.connected());
    // A disconnected venue is an ERROR, not a rejection: the order never
    // reached anyone, and the caller must tell those apart.
    REQUIRE_FALSE(broker.submit(priced).has_value());

    REQUIRE(broker.connect().has_value());
    auto ack = broker.submit(priced);
    REQUIRE(ack.has_value());
    REQUIRE(ack->accepted());
    // Stamped in the future: no real venue acknowledges instantaneously.
    REQUIRE(ack->acknowledged_at > h.clock.now());
}

TEST_CASE("the venue screens on its own policy, not our risk gate", "[paper][broker][property]") {
    Harness h{10};
    const PaperAccount account{h.portfolio};

    PaperBrokerConfig config;
    config.max_order_notional = Notional{1000.0};
    PaperBroker broker{h.clock, h.simulator, account, config};
    REQUIRE(broker.connect().has_value());

    LifecycleTimes times;
    times.decision_time = h.clock.now();
    auto order = oms::Order::market(oms::OrderId{1}, kSpy, Side::Buy, Qty{100}, times);
    auto ack = broker.submit(order->with_arrival_price(Price{500.0}));

    REQUIRE(ack.has_value());
    REQUIRE(ack->status == AckStatus::Rejected);
    REQUIRE(ack->reject_reason == "exceeds_venue_limit");
    // Recorded by reason, so a session can tell a venue refusal from a risk
    // refusal and know whether to fix its limits or call the broker.
    REQUIRE(broker.stats().rejections_by_reason.at(
                static_cast<std::uint8_t>(RejectReason::ExceedsVenueLimit)) == 1);
}

TEST_CASE("injected rejections are deterministic", "[paper][broker][determinism]") {
    const auto run = [] {
        Harness h{10};
        const PaperAccount account{h.portfolio};
        PaperBrokerConfig config;
        config.reject_every_n = 3;
        config.enforce_buying_power = false;
        PaperBroker broker{h.clock, h.simulator, account, config};
        REQUIRE(broker.connect().has_value());

        std::vector<bool> outcomes;
        for (std::uint64_t i = 1; i <= 9; ++i) {
            LifecycleTimes times;
            times.decision_time = h.clock.now();
            auto order = oms::Order::market(oms::OrderId{i}, kSpy, Side::Buy, Qty{1}, times);
            auto ack = broker.submit(order->with_arrival_price(Price{500.0}));
            outcomes.push_back(ack.has_value() && ack->accepted());
        }
        return outcomes;
    };
    const auto a = run();
    REQUIRE(a == run());
    REQUIRE_FALSE(a[2]);
    REQUIRE_FALSE(a[5]);
    REQUIRE(a[0]);
}

TEST_CASE("fills are polled, never pushed", "[paper][broker][determinism]") {
    // A callback would run on whatever thread the venue chose, and determinism
    // rests on one thread draining events in order.
    Harness h{10};
    const PaperAccount account{h.portfolio};
    PaperBrokerConfig config;
    config.enforce_buying_power = false;
    PaperBroker broker{h.clock, h.simulator, account, config};
    REQUIRE(broker.connect().has_value());

    LifecycleTimes times;
    times.decision_time = h.clock.now();
    auto order = oms::Order::market(oms::OrderId{1}, kSpy, Side::Buy, Qty{10}, times);
    REQUIRE(broker.submit(order->with_arrival_price(Price{500.0})).has_value());
    REQUIRE(broker.poll_fills().empty());
    REQUIRE(broker.working_orders().size() == 1);

    execution::MarketState state;
    state.bid = Price{500.0};
    state.ask = Price{500.0};
    // Displayed size matters: under the Phase 8 rule a quote with no size at
    // the touch offers nothing to take, so omitting these produces no fill.
    state.bid_size = Qty{1e6};
    state.ask_size = Qty{1e6};
    state.interval_volume = Volume{1e9};
    state.has_quote = true;
    h.clock.advance_by(seconds{1});
    auto fills = h.simulator.on_market(kSpy, state, h.clock.now());
    REQUIRE(fills.has_value());
    broker.route(*fills);

    REQUIRE(broker.poll_fills().size() == 1);
    // Draining twice yields nothing: the queue is consumed, not replayed.
    REQUIRE(broker.poll_fills().empty());
    REQUIRE(broker.working_orders().empty());
}

// ---------------------------------------------------------------------------
// Session and parity
// ---------------------------------------------------------------------------

TEST_CASE("a paper session runs start to stop", "[paper][session][e2e]") {
    const auto root = std::filesystem::temp_directory_path() / "ptl_paper_e2e";
    std::filesystem::remove_all(root);
    storage::ArtifactStore artifacts{root.string()};

    Harness h{120};
    PaperAccount account{h.portfolio};
    PaperBroker broker{h.clock, h.simulator, account};

    SessionConfig config;
    config.session_id = "e2e";
    config.persist_every_events = 25;

    PaperSession session{config,      h.clock, *h.source, h.strategy, h.simulator, broker,
                         h.portfolio, h.oms,   h.risk,    h.journal,  artifacts,   &us()};

    REQUIRE(session.start().has_value());
    REQUIRE(session.phase() == SessionPhase::Running);

    auto stats = session.run();
    REQUIRE(stats.has_value());
    REQUIRE(stats->events_processed > 0);
    REQUIRE(stats->persists > 0);
    REQUIRE(session.phase() == SessionPhase::Stopped);
    REQUIRE(artifacts.contains("paper/e2e/state"));

    std::filesystem::remove_all(root);
}

TEST_CASE("a paper session reproduces a backtest exactly",
          "[paper][session][parity][determinism]") {
    // THE CENTRAL CLAIM OF PHASE 14. Same engine, same strategy; only the clock
    // and source differ. Anything that diverges is a bug, not a tolerance.
    const auto backtest = run_backtest(120);

    const auto root = std::filesystem::temp_directory_path() / "ptl_paper_parity";
    std::filesystem::remove_all(root);
    storage::ArtifactStore artifacts{root.string()};

    Harness h{120};
    PaperAccount account{h.portfolio};
    PaperBrokerConfig broker_config;
    broker_config.enforce_buying_power = false;
    PaperBroker broker{h.clock, h.simulator, account, broker_config};

    SessionConfig config;
    config.session_id = "parity";
    // Persistence and the daily reset must NOT perturb trading. If they did,
    // this comparison would fail -- which is exactly why they are enabled here.
    config.persist_every_events = 10;
    config.daily_reset = true;

    PaperSession session{config,      h.clock, *h.source, h.strategy, h.simulator, broker,
                         h.portfolio, h.oms,   h.risk,    h.journal,  artifacts,   &us()};
    REQUIRE(session.start().has_value());
    REQUIRE(session.run().has_value());

    REQUIRE(h.strategy.orders.size() == backtest.orders.size());
    REQUIRE(h.strategy.fills.size() == backtest.fills.size());
    for (std::size_t i = 0; i < backtest.orders.size(); ++i) {
        REQUIRE(h.strategy.orders[i].ts == backtest.orders[i].ts);
        REQUIRE(h.strategy.orders[i].side == backtest.orders[i].side);
        // EXACT equality: float summation is not associative, so any ordering
        // difference would surface here.
        REQUIRE(h.strategy.orders[i].quantity == backtest.orders[i].quantity);
        REQUIRE(h.strategy.orders[i].order_id == backtest.orders[i].order_id);
    }
    for (std::size_t i = 0; i < backtest.fills.size(); ++i) {
        REQUIRE(h.strategy.fills[i].ts == backtest.fills[i].ts);
        REQUIRE(h.strategy.fills[i].quantity == backtest.fills[i].quantity);
    }
    REQUIRE(h.portfolio.equity().get() == backtest.final_equity.get());
    REQUIRE(h.journal.to_csv() == backtest.journal);

    std::filesystem::remove_all(root);
}

TEST_CASE("two paper sessions over identical events agree", "[paper][session][determinism]") {
    const auto run_once = [] {
        const auto root = std::filesystem::temp_directory_path() / "ptl_paper_repeat";
        std::filesystem::remove_all(root);
        storage::ArtifactStore artifacts{root.string()};

        Harness h{100};
        PaperAccount account{h.portfolio};
        PaperBrokerConfig broker_config;
        broker_config.enforce_buying_power = false;
        PaperBroker broker{h.clock, h.simulator, account, broker_config};

        SessionConfig config;
        config.session_id = "repeat";
        PaperSession session{config,      h.clock, *h.source, h.strategy, h.simulator, broker,
                             h.portfolio, h.oms,   h.risk,    h.journal,  artifacts,   &us()};
        REQUIRE(session.start().has_value());
        REQUIRE(session.run().has_value());
        auto out =
            std::make_tuple(session.content_hash(), h.portfolio.equity().get(), h.journal.to_csv());
        std::filesystem::remove_all(root);
        return out;
    };
    REQUIRE(run_once() == run_once());
}

TEST_CASE("pausing stops orders but not market data", "[paper][session][property]") {
    // Halting data too would leave a hole in the equity curve and the positions
    // mismarked on resume.
    const auto root = std::filesystem::temp_directory_path() / "ptl_paper_pause";
    std::filesystem::remove_all(root);
    storage::ArtifactStore artifacts{root.string()};

    Harness h{100};
    PaperAccount account{h.portfolio};
    PaperBroker broker{h.clock, h.simulator, account};

    SessionConfig config;
    config.session_id = "pause";
    PaperSession session{config,      h.clock, *h.source, h.strategy, h.simulator, broker,
                         h.portfolio, h.oms,   h.risk,    h.journal,  artifacts,   &us()};
    REQUIRE(session.start().has_value());
    REQUIRE(session.step(10).has_value());

    session.pause("operator intervention");
    REQUIRE(session.phase() == SessionPhase::Paused);
    REQUIRE(session.pause_reason() == "operator intervention");
    REQUIRE_FALSE(session.trading_permitted());
    REQUIRE_FALSE(broker.connected());

    // Data still flows while paused.
    auto processed = session.step(5);
    REQUIRE(processed.has_value());
    REQUIRE(*processed > 0);

    REQUIRE(session.resume().has_value());
    REQUIRE(session.phase() == SessionPhase::Running);
    REQUIRE(broker.connected());
    // Resuming a running session is refused.
    REQUIRE_FALSE(session.resume().has_value());

    std::filesystem::remove_all(root);
}

// ---------------------------------------------------------------------------
// Persistence and recovery
// ---------------------------------------------------------------------------

TEST_CASE("session state round-trips exactly and detects corruption",
          "[paper][recovery][serialization]") {
    SessionState state;
    state.session_id = "recover";
    state.sequence = 7;
    state.config_fingerprint = 0xABCDEF;
    state.events_processed = 4242;
    state.fills_received = 12;
    state.orders_submitted = 15;
    state.account.cash = Notional{50'000.0};
    state.account.equity = Notional{101'250.5};
    // Values that are NOT exactly representable at six decimal places. Writing
    // them rounded made the bit-exact checksum refuse its own output, and
    // recovery failed for almost every real book.
    state.positions.push_back({0, 100.123456789012, 499.987654321098, 25.0});
    state.working_orders.push_back({9, 0, 0, 50.0, 10.0});

    const std::string json = state.to_json();
    auto restored = SessionState::from_json(json);
    REQUIRE(restored.has_value());
    REQUIRE(restored->session_id == "recover");
    REQUIRE(restored->events_processed == 4242);
    REQUIRE(restored->positions.size() == 1);
    // EXACT, not approximate: persistence must restore the bits it saved.
    REQUIRE(restored->positions.front().quantity == 100.123456789012);
    REQUIRE(restored->positions.front().average_cost == 499.987654321098);
    REQUIRE(restored->working_orders.size() == 1);
    REQUIRE(restored->working_orders.front().filled == Catch::Approx(10.0));

    // Corrupting the event count invalidates the checksum. A damaged state is
    // REFUSED, not repaired: resuming from it would look complete.
    std::string tampered = json;
    const auto pos = tampered.find("4242");
    REQUIRE(pos != std::string::npos);
    tampered.replace(pos, 4, "9999");
    REQUIRE_FALSE(SessionState::from_json(tampered).has_value());

    REQUIRE_FALSE(SessionState::from_json("{}").has_value());
    REQUIRE_FALSE(SessionState::from_json("not json").has_value());
}

TEST_CASE("a session recovers after restart", "[paper][recovery][e2e]") {
    const auto root = std::filesystem::temp_directory_path() / "ptl_paper_recover";
    std::filesystem::remove_all(root);
    storage::ArtifactStore artifacts{root.string()};

    std::size_t events_before = 0;
    {
        Harness h{100};
        PaperAccount account{h.portfolio};
        PaperBrokerConfig broker_config;
        broker_config.enforce_buying_power = false;
        PaperBroker broker{h.clock, h.simulator, account, broker_config};

        SessionConfig config;
        config.session_id = "recover";
        config.config_fingerprint = 0x1234;
        PaperSession session{config,      h.clock, *h.source, h.strategy, h.simulator, broker,
                             h.portfolio, h.oms,   h.risk,    h.journal,  artifacts,   &us()};
        REQUIRE(session.start().has_value());
        REQUIRE(session.step(40).has_value());
        REQUIRE(session.persist().has_value());
        events_before = session.stats().events_processed;
        REQUIRE(events_before == 40);
    }

    // A fresh process, same session id and configuration.
    Harness h{100};
    PaperAccount account{h.portfolio};
    PaperBroker broker{h.clock, h.simulator, account};

    SessionConfig config;
    config.session_id = "recover";
    config.config_fingerprint = 0x1234;
    PaperSession session{config,      h.clock, *h.source, h.strategy, h.simulator, broker,
                         h.portfolio, h.oms,   h.risk,    h.journal,  artifacts,   &us()};
    REQUIRE(session.start().has_value());
    // The event count carried across, so a restart does not re-count work
    // already done.
    REQUIRE(session.stats().events_processed == events_before);

    std::filesystem::remove_all(root);
}

TEST_CASE("recovery into a different configuration is refused", "[paper][recovery][leakage]") {
    // Resuming a book under a configuration that did not create it mixes two
    // runs, and every attribution afterwards is wrong.
    const auto root = std::filesystem::temp_directory_path() / "ptl_paper_cfg";
    std::filesystem::remove_all(root);
    storage::ArtifactStore artifacts{root.string()};

    SessionState prior;
    prior.session_id = "cfg";
    prior.config_fingerprint = 0xAAAA;
    REQUIRE(artifacts.put("paper/cfg/state", prior.to_json()).has_value());

    Harness h{20};
    PaperAccount account{h.portfolio};
    PaperBroker broker{h.clock, h.simulator, account};

    SessionConfig config;
    config.session_id = "cfg";
    config.config_fingerprint = 0xBBBB;  // different
    config.require_matching_config = true;

    PaperSession session{config,      h.clock, *h.source, h.strategy, h.simulator, broker,
                         h.portfolio, h.oms,   h.risk,    h.journal,  artifacts,   &us()};
    auto started = session.start();
    REQUIRE_FALSE(started.has_value());
    REQUIRE(started.error().message.find("refusing to resume") != std::string::npos);
    REQUIRE(session.phase() == SessionPhase::Failed);

    std::filesystem::remove_all(root);
}

TEST_CASE("orders reach the venue only through the risk gate", "[paper][session][leakage]") {
    // NO BYPASS. Tightening a risk limit must reduce what gets through, which
    // proves the session cannot reach the simulator directly.
    const auto root = std::filesystem::temp_directory_path() / "ptl_paper_risk";
    std::filesystem::remove_all(root);
    storage::ArtifactStore artifacts{root.string()};

    Harness h{120};
    risk::RiskLimits tight;
    tight.max_order_notional = Notional{1.0};  // nothing can pass
    risk::RiskManager strict{tight};

    PaperAccount account{h.portfolio};
    PaperBroker broker{h.clock, h.simulator, account};

    SessionConfig config;
    config.session_id = "risk";
    PaperSession session{config,      h.clock, *h.source, h.strategy, h.simulator, broker,
                         h.portfolio, h.oms,   strict,    h.journal,  artifacts,   &us()};
    REQUIRE(session.start().has_value());
    REQUIRE(session.run().has_value());

    REQUIRE(h.strategy.orders.empty());
    REQUIRE(session.stats().fills_received == 0);
    REQUIRE(h.journal.to_csv().find("risk_rejection") != std::string::npos);

    std::filesystem::remove_all(root);
}
