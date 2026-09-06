#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <filesystem>
#include <memory>
#include <vector>

#include "ptl/live/session.hpp"
#include "support/ptl_catch.hpp"

using namespace ptl;
using namespace ptl::live;
using namespace std::chrono;

namespace {

Timestamp at(const char* iso) {
    Timestamp ts{};
    REQUIRE(parse_timestamp(iso, ts));
    return ts;
}

constexpr InstrumentId kSpy{0};

/// A transport backed by a scripted message list. THE ONLY THING A REAL
/// DEPLOYMENT WOULD REPLACE -- everything above it is exercised unchanged, which
/// is what makes live logic testable without a socket or credentials.
class ScriptedTransport final : public ILiveTransport {
public:
    [[nodiscard]] std::string_view venue() const noexcept override { return "scripted"; }

    [[nodiscard]] Result<bool> open() override {
        if (fail_open) {
            return fail(make_error(ErrorCode::IoError, "scripted open failure"));
        }
        open_ = true;
        ++opens;
        return true;
    }
    void close() noexcept override { open_ = false; }
    [[nodiscard]] bool is_open() const noexcept override { return open_; }

    [[nodiscard]] Result<bool> send(std::string_view payload) override {
        if (fail_send) {
            return fail(make_error(ErrorCode::IoError, "scripted send failure"));
        }
        sent.emplace_back(payload);
        return true;
    }

    [[nodiscard]] std::vector<BrokerMessage> poll() override {
        auto out = std::move(inbound);
        inbound.clear();
        return out;
    }

    std::vector<BrokerMessage> inbound;
    std::vector<std::string> sent;
    std::size_t opens = 0;
    bool fail_open = false;
    bool fail_send = false;

private:
    bool open_ = false;
};

oms::Order make_order(std::uint64_t id, Side side, double qty, double price, Timestamp decision,
                      oms::TimeInForce tif = oms::TimeInForce::Day) {
    LifecycleTimes times;
    times.decision_time = decision;
    auto order =
        oms::Order::market(static_cast<oms::OrderId>(id), kSpy, side, Qty{qty}, times, tif);
    REQUIRE(order.has_value());
    return order->with_arrival_price(Price{price});
}

oms::Order make_limit(std::uint64_t id, Side side, double qty, double limit, Timestamp decision,
                      oms::TimeInForce tif = oms::TimeInForce::Day) {
    LifecycleTimes times;
    times.decision_time = decision;
    auto order = oms::Order::limit(static_cast<oms::OrderId>(id), kSpy, side, Qty{qty},
                                   Price{limit}, times, tif);
    REQUIRE(order.has_value());
    return order->with_arrival_price(Price{limit});
}

InstrumentTable spy_table() {
    InstrumentTable table;
    const auto id = table.intern("SPY");
    REQUIRE(id == kSpy);
    return table;
}

}  // namespace

// ---------------------------------------------------------------------------
// Heartbeat and supervisor
// ---------------------------------------------------------------------------

TEST_CASE("a late heartbeat degrades before it kills", "[live][heartbeat][property]") {
    // Tearing down a healthy session over one slow heartbeat is itself an
    // outage, so LATE and GONE are distinct states.
    HeartbeatConfig config;
    config.degraded_after = seconds{20};
    config.dead_after = seconds{45};
    HeartbeatMonitor monitor{config};

    const Timestamp t0 = at("2024-07-02T15:00:00Z");
    monitor.record_heartbeat(t0);

    REQUIRE(monitor.healthy(t0 + seconds{10}));
    REQUIRE_FALSE(monitor.degraded(t0 + seconds{10}));

    REQUIRE_FALSE(monitor.healthy(t0 + seconds{30}));
    REQUIRE(monitor.degraded(t0 + seconds{30}));
    REQUIRE_FALSE(monitor.dead(t0 + seconds{30}));

    REQUIRE(monitor.dead(t0 + seconds{60}));
    REQUIRE(monitor.beats() == 1);
}

TEST_CASE("any inbound message proves liveness", "[live][heartbeat][edge]") {
    // A venue busy streaming quotes may legitimately skip a heartbeat, and
    // declaring it dead would be an outage we caused ourselves.
    HeartbeatMonitor monitor;
    const Timestamp t0 = at("2024-07-02T15:00:00Z");
    monitor.record_heartbeat(t0);
    monitor.record_activity(t0 + seconds{30});
    REQUIRE(monitor.healthy(t0 + seconds{35}));
}

TEST_CASE("reconnect backoff grows and is capped", "[live][supervisor][property]") {
    // Unbounded doubling means a session that drops overnight is still asleep
    // when the market opens.
    SupervisorConfig config;
    config.initial_backoff = seconds{1};
    config.max_backoff = seconds{8};
    config.backoff_multiplier = 2.0;
    config.max_attempts = 0;  // unlimited
    ConnectionSupervisor supervisor{config};

    Timestamp t = at("2024-07-02T15:00:00Z");
    REQUIRE(supervisor.should_attempt(t));

    std::vector<std::int64_t> backoffs;
    for (int i = 0; i < 6; ++i) {
        supervisor.record_attempt(t);
        backoffs.push_back(supervisor.current_backoff().count());
        t += seconds{100};
    }
    // Doubling, then flat at the ceiling.
    REQUIRE(backoffs.front() < backoffs[1]);
    // Duration is nanoseconds, so the ceiling must be converted rather than
    // compared against a raw seconds count.
    REQUIRE(backoffs.back() == duration_cast<Duration>(seconds{8}).count());
}

TEST_CASE("the supervisor gives up after its attempt limit", "[live][supervisor][edge]") {
    SupervisorConfig config;
    config.max_attempts = 3;
    ConnectionSupervisor supervisor{config};

    const Timestamp t = at("2024-07-02T15:00:00Z");
    for (int i = 0; i < 3; ++i) supervisor.record_attempt(t);
    REQUIRE(supervisor.exhausted());
    REQUIRE_FALSE(supervisor.should_attempt(t + hours{1}));

    // A success resets the count, so a flapping link is not permanently barred.
    supervisor.record_success(t);
    REQUIRE_FALSE(supervisor.exhausted());
}

// ---------------------------------------------------------------------------
// Connection hygiene
// ---------------------------------------------------------------------------

TEST_CASE("orders are refused until the connection is synchronized",
          "[live][connection][leakage]") {
    // Trading on a stale book is how a reconnect doubles a position.
    REQUIRE_FALSE(permits_trading(ConnectionState::Connected));
    REQUIRE_FALSE(permits_trading(ConnectionState::Synchronizing));
    REQUIRE_FALSE(permits_trading(ConnectionState::Degraded));
    REQUIRE_FALSE(permits_trading(ConnectionState::Reconnecting));
    REQUIRE(permits_trading(ConnectionState::Ready));

    SimulatedClock clock{at("2024-07-02T15:00:00Z")};
    ScriptedTransport transport;
    LiveConnection connection{clock, transport};

    REQUIRE(connection.connect().has_value());
    // Connected but NOT ready.
    REQUIRE(connection.state() == ConnectionState::Synchronizing);
    REQUIRE_FALSE(connection.trading_permitted());
    REQUIRE_FALSE(connection.send("{}").has_value());

    REQUIRE(connection.mark_synchronized().has_value());
    REQUIRE(connection.trading_permitted());
    REQUIRE(connection.send("{}").has_value());
}

TEST_CASE("duplicate venue messages are dropped once", "[live][connection][leakage]") {
    // A venue that redelivers after a reconnect is normal; applying a fill
    // twice is not.
    SimulatedClock clock{at("2024-07-02T15:00:00Z")};
    ScriptedTransport transport;
    LiveConnection connection{clock, transport};
    REQUIRE(connection.connect().has_value());

    BrokerMessage first;
    first.kind = MessageKind::Fill;
    first.sequence = 10;
    first.client_order_id = "ptl-1";

    BrokerMessage replay = first;  // same sequence: a redelivery

    BrokerMessage later = first;
    later.sequence = 11;

    transport.inbound = {first, replay, later};
    const auto accepted = connection.poll();
    REQUIRE(accepted.size() == 2);
    REQUIRE(connection.stats().duplicates_dropped == 1);
    REQUIRE(connection.last_sequence() == 11);
}

TEST_CASE("unsequenced streams deduplicate on venue fill id", "[live][connection][edge]") {
    SimulatedClock clock{at("2024-07-02T15:00:00Z")};
    ScriptedTransport transport;
    LiveConnection connection{clock, transport};
    REQUIRE(connection.connect().has_value());

    BrokerMessage fill;
    fill.kind = MessageKind::Fill;
    fill.sequence = 0;  // venue does not sequence this stream
    fill.venue_fill_id = "vf-1";

    transport.inbound = {fill, fill};
    REQUIRE(connection.poll().size() == 1);
    REQUIRE(connection.stats().duplicates_dropped == 1);
}

TEST_CASE("heartbeats are consumed and unknown messages counted", "[live][connection]") {
    SimulatedClock clock{at("2024-07-02T15:00:00Z")};
    ScriptedTransport transport;
    LiveConnection connection{clock, transport};
    REQUIRE(connection.connect().has_value());

    BrokerMessage beat;
    beat.kind = MessageKind::Heartbeat;
    BrokerMessage mystery;
    mystery.kind = MessageKind::Unknown;

    transport.inbound = {beat, mystery};
    // Neither reaches the caller, but the unknown one is COUNTED: it is
    // evidence of a protocol change, and hiding it defers the discovery.
    REQUIRE(connection.poll().empty());
    REQUIRE(connection.heartbeat().beats() == 1);
    REQUIRE(connection.stats().unknown_messages == 1);
}

TEST_CASE("a silent venue is noticed without any message arriving",
          "[live][connection][property]") {
    // A message-driven check would never fire here, which is exactly the
    // failure that matters most.
    HeartbeatConfig heartbeat;
    heartbeat.degraded_after = seconds{20};
    heartbeat.dead_after = seconds{45};

    SimulatedClock clock{at("2024-07-02T15:00:00Z")};
    ScriptedTransport transport;
    LiveConnection connection{clock, transport, heartbeat};
    REQUIRE(connection.connect().has_value());
    REQUIRE(connection.mark_synchronized().has_value());

    clock.advance_by(seconds{30});
    connection.tick();
    REQUIRE(connection.state() == ConnectionState::Degraded);
    REQUIRE_FALSE(connection.trading_permitted());

    clock.advance_by(seconds{60});
    connection.tick();
    REQUIRE(connection.state() == ConnectionState::Reconnecting);
}

TEST_CASE("a reconnect preserves sequence tracking", "[live][connection][recovery][leakage]") {
    // Resetting it would let every redelivered message through as new, which is
    // precisely the duplicate-fill scenario.
    SimulatedClock clock{at("2024-07-02T15:00:00Z")};
    ScriptedTransport transport;
    LiveConnection connection{clock, transport};
    REQUIRE(connection.connect().has_value());

    BrokerMessage message;
    message.kind = MessageKind::Fill;
    message.sequence = 42;
    transport.inbound = {message};
    REQUIRE(connection.poll().size() == 1);
    REQUIRE(connection.last_sequence() == 42);

    clock.advance_by(seconds{5});
    REQUIRE(connection.maybe_reconnect().has_value());
    REQUIRE(connection.last_sequence() == 42);

    // The venue redelivers on reconnect; it is still recognised as a duplicate.
    transport.inbound = {message};
    REQUIRE(connection.poll().empty());
    REQUIRE(connection.stats().duplicates_dropped == 1);
}

TEST_CASE("a failed transport open leaves the connection failed", "[live][connection][edge]") {
    SimulatedClock clock{at("2024-07-02T15:00:00Z")};
    ScriptedTransport transport;
    transport.fail_open = true;
    LiveConnection connection{clock, transport};

    REQUIRE_FALSE(connection.connect().has_value());
    REQUIRE(connection.state() == ConnectionState::Failed);
    REQUIRE_FALSE(connection.trading_permitted());
}

// ---------------------------------------------------------------------------
// Order translation
// ---------------------------------------------------------------------------

TEST_CASE("order translation is total or it fails", "[live][translator][property]") {
    // A translator that silently downgrades an unsupported order would send the
    // venue something the strategy did not ask for.
    const auto table = spy_table();
    AlpacaOrderTranslator translator{AlpacaOrderTranslator::Config{}, &table};
    const Timestamp t = at("2024-07-02T15:00:00Z");

    // IOC on a MARKET order is not expressible at this venue.
    const auto ioc_market =
        make_order(1, Side::Buy, 10, 500.0, t, oms::TimeInForce::ImmediateOrCancel);
    auto refused = translator.supports(ioc_market);
    REQUIRE_FALSE(refused.has_value());
    REQUIRE(refused.error().message.find("IOC or FOK on a market order") != std::string::npos);

    // The same TIF on a limit order is fine.
    const auto ioc_limit =
        make_limit(2, Side::Buy, 10, 500.0, t, oms::TimeInForce::ImmediateOrCancel);
    REQUIRE(translator.supports(ioc_limit).has_value());

    auto encoded = translator.encode_new(ioc_limit);
    REQUIRE(encoded.has_value());
    REQUIRE(encoded->payload.find("\"time_in_force\": \"ioc\"") != std::string::npos);
    REQUIRE(encoded->payload.find("\"type\": \"limit\"") != std::string::npos);
    REQUIRE(encoded->payload.find("\"symbol\": \"SPY\"") != std::string::npos);
}

TEST_CASE("every order type and time in force has a venue spelling", "[live][translator]") {
    // A wrong string here is a wrong order at the venue.
    REQUIRE(*AlpacaOrderTranslator::type_string(oms::OrderType::Market) == "market");
    REQUIRE(*AlpacaOrderTranslator::type_string(oms::OrderType::Limit) == "limit");
    REQUIRE(*AlpacaOrderTranslator::type_string(oms::OrderType::Stop) == "stop");
    REQUIRE(*AlpacaOrderTranslator::type_string(oms::OrderType::StopLimit) == "stop_limit");

    REQUIRE(*AlpacaOrderTranslator::tif_string(oms::TimeInForce::Day) == "day");
    REQUIRE(*AlpacaOrderTranslator::tif_string(oms::TimeInForce::ImmediateOrCancel) == "ioc");
    REQUIRE(*AlpacaOrderTranslator::tif_string(oms::TimeInForce::FillOrKill) == "fok");
    REQUIRE(*AlpacaOrderTranslator::tif_string(oms::TimeInForce::GoodTillCancel) == "gtc");
}

TEST_CASE("translation refuses to guess a symbol", "[live][translator][leakage]") {
    // Guessing would send an order for the wrong security.
    AlpacaOrderTranslator translator;  // no instrument table
    const auto order = make_order(1, Side::Buy, 10, 500.0, at("2024-07-02T15:00:00Z"));
    auto refused = translator.encode_new(order);
    REQUIRE_FALSE(refused.has_value());
    REQUIRE(refused.error().message.find("refusing to guess") != std::string::npos);
}

TEST_CASE("cancel and replace need the venue's order id", "[live][translator][edge]") {
    // The venue does not accept a cancel keyed on our own id, so sending one
    // would silently do nothing.
    const auto table = spy_table();
    AlpacaOrderTranslator translator{AlpacaOrderTranslator::Config{}, &table};

    REQUIRE_FALSE(translator.encode_cancel(oms::OrderId{1}, "").has_value());
    REQUIRE(translator.encode_cancel(oms::OrderId{1}, "venue-9").has_value());

    const auto amended = make_limit(1, Side::Buy, 5, 499.0, at("2024-07-02T15:00:00Z"));
    REQUIRE_FALSE(translator.encode_replace(amended, "").has_value());
    auto replaced = translator.encode_replace(amended, "venue-9");
    REQUIRE(replaced.has_value());
    REQUIRE(replaced->payload.find("\"action\": \"replace\"") != std::string::npos);
}

TEST_CASE("client order ids are deterministic", "[live][translator][determinism]") {
    // A reconnect cannot match its own outstanding requests otherwise.
    AlpacaOrderTranslator translator;
    const auto first = translator.client_order_id(oms::OrderId{77});
    REQUIRE(first == translator.client_order_id(oms::OrderId{77}));
    REQUIRE(first != translator.client_order_id(oms::OrderId{78}));
}

TEST_CASE("prices are encoded round-trip exact", "[live][translator][regression]") {
    // A price rounded on the wire is a DIFFERENT ORDER from the one risk
    // approved, and the difference is invisible in the log.
    const auto table = spy_table();
    AlpacaOrderTranslator translator{AlpacaOrderTranslator::Config{}, &table};
    const auto order = make_limit(1, Side::Buy, 10, 499.123456789012, at("2024-07-02T15:00:00Z"));
    auto encoded = translator.encode_new(order);
    REQUIRE(encoded.has_value());

    // Assert the PROPERTY, not a guessed literal: whatever digits the encoder
    // emits must parse back to the exact double that risk approved.
    const std::string needle = "\"limit_price\": \"";
    const auto start = encoded->payload.find(needle);
    REQUIRE(start != std::string::npos);
    const auto value_start = start + needle.size();
    const auto value_end = encoded->payload.find('"', value_start);
    REQUIRE(value_end != std::string::npos);
    const std::string encoded_price = encoded->payload.substr(value_start, value_end - value_start);
    REQUIRE(std::stod(encoded_price) == 499.123456789012);
}

TEST_CASE("fractional quantities are refused when the venue forbids them",
          "[live][translator][edge]") {
    const auto table = spy_table();
    AlpacaOrderTranslator translator{AlpacaOrderTranslator::Config{}, &table};
    const auto fractional = make_order(1, Side::Buy, 10.5, 500.0, at("2024-07-02T15:00:00Z"));
    REQUIRE_FALSE(translator.supports(fractional).has_value());

    AlpacaOrderTranslator::Config permissive;
    permissive.allow_fractional = true;
    AlpacaOrderTranslator lenient{permissive, &table};
    REQUIRE(lenient.supports(fractional).has_value());
}

// ---------------------------------------------------------------------------
// Order listener
// ---------------------------------------------------------------------------

TEST_CASE("an update for an untracked order is refused, never guessed",
          "[live][listener][leakage]") {
    // Applying a fill to an order we cannot identify would corrupt the
    // portfolio silently.
    LiveOrderListener listener;
    REQUIRE(listener.track("ptl-1", oms::OrderId{1}).has_value());

    BrokerMessage message;
    message.kind = MessageKind::Fill;
    message.client_order_id = "ptl-999";  // never tracked
    message.quantity = Qty{10};
    message.price = Price{500.0};

    auto refused = listener.interpret(message);
    REQUIRE_FALSE(refused.has_value());
    REQUIRE(refused.error().message.find("untracked order") != std::string::npos);
}

TEST_CASE("one client id cannot track two orders", "[live][listener][edge]") {
    // A reply could otherwise be attributed to the wrong order, applying a fill
    // to a position we do not hold.
    LiveOrderListener listener;
    REQUIRE(listener.track("ptl-1", oms::OrderId{1}).has_value());
    // Idempotent for the same order.
    REQUIRE(listener.track("ptl-1", oms::OrderId{1}).has_value());
    REQUIRE_FALSE(listener.track("ptl-1", oms::OrderId{2}).has_value());
    REQUIRE_FALSE(listener.track("", oms::OrderId{3}).has_value());
}

TEST_CASE("a fill with a bad quantity or price is refused", "[live][listener][leakage]") {
    LiveOrderListener listener;
    REQUIRE(listener.track("ptl-1", oms::OrderId{1}).has_value());

    BrokerMessage bad_quantity;
    bad_quantity.kind = MessageKind::Fill;
    bad_quantity.client_order_id = "ptl-1";
    bad_quantity.quantity = Qty{0.0};
    bad_quantity.price = Price{500.0};
    REQUIRE_FALSE(listener.interpret(bad_quantity).has_value());

    BrokerMessage bad_price = bad_quantity;
    bad_price.quantity = Qty{10.0};
    bad_price.price = Price{0.0};
    REQUIRE_FALSE(listener.interpret(bad_price).has_value());
}

TEST_CASE("non-order messages are ignored, not rejected", "[live][listener][edge]") {
    // A quote arriving here is normal.
    LiveOrderListener listener;
    BrokerMessage quote;
    quote.kind = MessageKind::Quote;
    auto update = listener.interpret(quote);
    REQUIRE(update.has_value());
    REQUIRE(update->kind == OrderUpdate::Kind::Ignored);
}

TEST_CASE("the listener snapshot restores exactly", "[live][listener][serialization][recovery]") {
    LiveOrderListener listener;
    REQUIRE(listener.track("ptl-2", oms::OrderId{2}).has_value());
    REQUIRE(listener.track("ptl-1", oms::OrderId{1}).has_value());

    const auto snapshot = listener.snapshot();
    REQUIRE(snapshot.size() == 2);
    // Ordered, so a persisted snapshot is byte-identical between runs.
    REQUIRE(snapshot.front().first == "ptl-1");

    LiveOrderListener restored;
    REQUIRE(restored.restore(snapshot).has_value());
    REQUIRE(restored.tracked() == 2);
    REQUIRE(restored.resolve("ptl-2").value() == oms::OrderId{2});
}

// ---------------------------------------------------------------------------
// Fill ingress: the Phase 3 invariant under live conditions
// ---------------------------------------------------------------------------

namespace {

struct BrokerHarness {
    SimulatedClock clock{at("2024-07-02T15:00:00Z")};
    execution::StandardCostModel costs;
    execution::StandardLatencyModel latency;
    execution::BrokerSimulator simulator{clock, costs, latency, DeterministicRng{5}};
    portfolio::Portfolio portfolio;
};

}  // namespace

TEST_CASE("an external fill for an unknown order is refused", "[live][ingress][leakage]") {
    // A venue message is untrusted input. A fill admitted without checking
    // would corrupt the portfolio in a way reconciliation catches only later.
    BrokerHarness h;
    execution::ExternalFillReport report;
    report.order_id = oms::OrderId{99};
    report.quantity = Qty{10};
    report.price = Price{500.0};
    report.fill_time = h.clock.now();

    auto refused = h.simulator.ingest_external_fill(report);
    REQUIRE_FALSE(refused.has_value());
    REQUIRE(refused.error().message.find("not working") != std::string::npos);
}

TEST_CASE("an external fill cannot exceed the order quantity",
          "[live][ingress][leakage][property]") {
    // Over-filling would create shares the order never asked for.
    BrokerHarness h;
    const auto order = make_order(1, Side::Buy, 10, 500.0, h.clock.now());
    REQUIRE(h.simulator.submit(order).has_value());

    execution::ExternalFillReport report;
    report.order_id = order.id();
    report.quantity = Qty{25};  // more than the order
    report.price = Price{500.0};
    report.fill_time = h.clock.now() + seconds{1};

    auto refused = h.simulator.ingest_external_fill(report);
    REQUIRE_FALSE(refused.has_value());
    REQUIRE(refused.error().message.find("exceeds") != std::string::npos);
}

TEST_CASE("external fills use venue costs, not the cost model", "[live][ingress][property]") {
    // The model estimates what a fill WOULD cost; the venue reports what it DID
    // cost, and a live P&L must use the latter.
    BrokerHarness h;
    const auto order = make_order(1, Side::Buy, 10, 500.0, h.clock.now());
    REQUIRE(h.simulator.submit(order).has_value());

    execution::ExternalFillReport report;
    report.order_id = order.id();
    report.quantity = Qty{10};
    report.price = Price{500.25};
    report.fill_time = h.clock.now() + seconds{1};
    report.commission = Notional{1.23};
    report.exchange_fee = Notional{0.07};

    auto fill = h.simulator.ingest_external_fill(report);
    REQUIRE(fill.has_value());
    REQUIRE(fill->commission().get() == Catch::Approx(1.23));
    REQUIRE(fill->exchange_fee().get() == Catch::Approx(0.07));
    REQUIRE(fill->price().get() == Catch::Approx(500.25));
    REQUIRE(fill->quantity().get() == Catch::Approx(10.0));
}

TEST_CASE("an external fill must satisfy the timestamp chain", "[live][ingress][leakage]") {
    // A venue clock that disagrees with ours must not produce a fill that
    // precedes its own decision.
    BrokerHarness h;
    const auto order = make_order(1, Side::Buy, 10, 500.0, h.clock.now());
    REQUIRE(h.simulator.submit(order).has_value());

    execution::ExternalFillReport report;
    report.order_id = order.id();
    report.quantity = Qty{10};
    report.price = Price{500.0};
    // Before the decision instant.
    report.fill_time = h.clock.now() - hours{1};

    auto refused = h.simulator.ingest_external_fill(report);
    REQUIRE_FALSE(refused.has_value());
    REQUIRE(refused.error().message.find("timestamp chain") != std::string::npos);
}

TEST_CASE("partial external fills accumulate to completion", "[live][ingress][property]") {
    BrokerHarness h;
    const auto order = make_order(1, Side::Buy, 10, 500.0, h.clock.now());
    REQUIRE(h.simulator.submit(order).has_value());

    execution::ExternalFillReport first;
    first.order_id = order.id();
    first.quantity = Qty{4};
    first.price = Price{500.0};
    first.fill_time = h.clock.now() + seconds{1};
    REQUIRE(h.simulator.ingest_external_fill(first).has_value());

    execution::ExternalFillReport second = first;
    second.quantity = Qty{6};
    second.fill_time = h.clock.now() + seconds{2};
    REQUIRE(h.simulator.ingest_external_fill(second).has_value());

    // Fully filled, so a third report finds nothing working.
    execution::ExternalFillReport third = first;
    third.quantity = Qty{1};
    REQUIRE_FALSE(h.simulator.ingest_external_fill(third).has_value());
}

// ---------------------------------------------------------------------------
// Reconciliation
// ---------------------------------------------------------------------------

TEST_CASE("reconciliation reports drift in both directions", "[live][reconciliation][property]") {
    SimulatedClock clock{at("2024-07-02T15:00:00Z")};
    ScriptedTransport transport;
    LiveConnection connection{clock, transport};
    const auto table = spy_table();
    AlpacaOrderTranslator translator{AlpacaOrderTranslator::Config{}, &table};

    execution::StandardCostModel costs;
    execution::StandardLatencyModel latency;
    execution::BrokerSimulator simulator{clock, costs, latency, DeterministicRng{3}};
    portfolio::Portfolio portfolio;

    LiveBroker broker{clock, connection, translator, simulator, portfolio};

    BrokerAccountSnapshot venue;
    venue.ts = clock.now();
    venue.cash = portfolio.cash();
    auto clean = broker.reconcile(venue);
    REQUIRE(clean.clean());

    // A position the VENUE holds and we do not is the dangerous direction: it
    // is risk nobody local is watching.
    venue.positions[0] = 100.0;
    auto drifted = broker.reconcile(venue);
    REQUIRE_FALSE(drifted.clean());
    REQUIRE(drifted.position_drift.contains(0));
    REQUIRE(drifted.position_drift.at(0) == Catch::Approx(-100.0));

    // Cash drift beyond tolerance is flagged.
    venue.positions.clear();
    venue.cash = Notional{portfolio.cash().get() - 500.0};
    auto cash_drift = broker.reconcile(venue);
    REQUIRE_FALSE(cash_drift.cash_matches);
    REQUIRE(cash_drift.cash_drift.get() == Catch::Approx(500.0));
}

TEST_CASE("small cash differences are tolerated", "[live][reconciliation][edge]") {
    // Fees post asynchronously at most venues; demanding exactness would fire
    // on every ordinary session.
    SimulatedClock clock{at("2024-07-02T15:00:00Z")};
    ScriptedTransport transport;
    LiveConnection connection{clock, transport};
    AlpacaOrderTranslator translator;

    execution::StandardCostModel costs;
    execution::StandardLatencyModel latency;
    execution::BrokerSimulator simulator{clock, costs, latency, DeterministicRng{3}};
    portfolio::Portfolio portfolio;

    LiveBrokerConfig config;
    config.cash_tolerance = 0.01;
    LiveBroker broker{clock, connection, translator, simulator, portfolio, config};

    BrokerAccountSnapshot venue;
    venue.cash = Notional{portfolio.cash().get() - 0.005};
    REQUIRE(broker.reconcile(venue).cash_matches);
}

// ---------------------------------------------------------------------------
// Broker routing
// ---------------------------------------------------------------------------

namespace {

struct LiveHarness {
    SimulatedClock clock{at("2024-07-02T15:00:00Z")};
    ScriptedTransport transport;
    InstrumentTable table = spy_table();
    LiveConnection connection{clock, transport};
    AlpacaOrderTranslator translator{AlpacaOrderTranslator::Config{}, &table};
    execution::StandardCostModel costs;
    execution::StandardLatencyModel latency;
    execution::BrokerSimulator simulator{clock, costs, latency, DeterministicRng{11}};
    portfolio::Portfolio portfolio;
    LiveBroker broker{clock, connection, translator, simulator, portfolio};

    void ready() {
        REQUIRE(connection.connect().has_value());
        REQUIRE(connection.mark_synchronized().has_value());
    }
};

}  // namespace

TEST_CASE("the broker refuses to send while not ready", "[live][broker][leakage]") {
    // A queued order sent on reconnect would execute against a market that has
    // moved, on a decision made minutes ago.
    LiveHarness h;
    const auto order = make_order(1, Side::Buy, 10, 500.0, h.clock.now());

    auto refused = h.broker.submit(order);
    REQUIRE_FALSE(refused.has_value());
    REQUIRE(h.transport.sent.empty());

    h.ready();
    REQUIRE(h.broker.submit(order).has_value());
    REQUIRE(h.transport.sent.size() == 1);
    REQUIRE(h.broker.working_orders().size() == 1);
}

TEST_CASE("a failed send stops tracking the order", "[live][broker][recovery][leakage]") {
    // The venue never saw it, so a later reply must not be matched to it.
    LiveHarness h;
    h.ready();
    h.transport.fail_send = true;

    const auto order = make_order(1, Side::Buy, 10, 500.0, h.clock.now());
    REQUIRE_FALSE(h.broker.submit(order).has_value());
    REQUIRE(h.broker.listener().tracked() == 0);
    REQUIRE(h.broker.working_orders().empty());
}

TEST_CASE("a venue fill flows through the simulator into a Fill", "[live][broker][e2e]") {
    LiveHarness h;
    h.ready();
    const auto order = make_order(1, Side::Buy, 10, 500.0, h.clock.now());
    REQUIRE(h.broker.submit(order).has_value());

    h.clock.advance_by(seconds{1});
    BrokerMessage accepted;
    accepted.kind = MessageKind::OrderAccepted;
    accepted.sequence = 1;
    accepted.client_order_id = h.translator.client_order_id(order.id());
    accepted.venue_order_id = "venue-1";

    BrokerMessage filled;
    filled.kind = MessageKind::Fill;
    filled.sequence = 2;
    filled.client_order_id = accepted.client_order_id;
    filled.venue_order_id = "venue-1";
    filled.venue_fill_id = "vf-1";
    filled.quantity = Qty{10};
    filled.price = Price{500.25};
    filled.venue_time = h.clock.now();
    filled.commission = Notional{1.0};

    h.transport.inbound = {accepted, filled};
    auto fills = h.broker.poll();
    REQUIRE(fills.has_value());
    REQUIRE(fills->size() == 1);
    REQUIRE(fills->front().price().get() == Catch::Approx(500.25));
    REQUIRE(h.broker.venue_id_of(order.id()) == "venue-1");
    REQUIRE(h.broker.stats().fills_ingested == 1);
    // Fully filled: no longer working.
    REQUIRE(h.broker.working_orders().empty());
}

TEST_CASE("a venue reject clears the order locally", "[live][broker][property]") {
    LiveHarness h;
    h.ready();
    const auto order = make_order(1, Side::Buy, 10, 500.0, h.clock.now());
    REQUIRE(h.broker.submit(order).has_value());

    BrokerMessage rejected;
    rejected.kind = MessageKind::OrderRejected;
    rejected.sequence = 1;
    rejected.client_order_id = h.translator.client_order_id(order.id());
    rejected.text = "insufficient buying power";

    h.transport.inbound = {rejected};
    auto fills = h.broker.poll();
    REQUIRE(fills.has_value());
    REQUIRE(fills->empty());
    REQUIRE(h.broker.working_orders().empty());
    REQUIRE(h.broker.listener().tracked() == 0);
}

TEST_CASE("a cancel is not applied until the venue confirms", "[live][broker][leakage]") {
    // Assuming success would leave us flat locally while a live order rests at
    // the exchange.
    LiveHarness h;
    h.ready();
    const auto order = make_order(1, Side::Buy, 10, 500.0, h.clock.now());
    REQUIRE(h.broker.submit(order).has_value());

    BrokerMessage accepted;
    accepted.kind = MessageKind::OrderAccepted;
    accepted.sequence = 1;
    accepted.client_order_id = h.translator.client_order_id(order.id());
    accepted.venue_order_id = "venue-1";
    h.transport.inbound = {accepted};
    REQUIRE(h.broker.poll().has_value());

    REQUIRE(h.broker.cancel(order.id()).has_value());
    // Still working: the venue has not confirmed.
    REQUIRE(h.broker.working_orders().size() == 1);

    BrokerMessage cancelled;
    cancelled.kind = MessageKind::OrderCancelled;
    cancelled.sequence = 2;
    cancelled.client_order_id = accepted.client_order_id;
    h.transport.inbound = {cancelled};
    REQUIRE(h.broker.poll().has_value());
    REQUIRE(h.broker.working_orders().empty());
}

// ---------------------------------------------------------------------------
// Market data adapter
// ---------------------------------------------------------------------------

TEST_CASE("live quotes become the existing quote model", "[live][marketdata][property]") {
    // No new market structure: nothing downstream can tell a live quote from a
    // replayed one.
    SimulatedClock clock{at("2024-07-02T15:00:00Z")};
    ScriptedTransport transport;
    LiveConnection connection{clock, transport};
    REQUIRE(connection.connect().has_value());

    LiveMarketDataAdapter adapter{clock, connection};

    BrokerMessage quote;
    quote.kind = MessageKind::Quote;
    quote.instrument = kSpy;
    quote.venue_time = clock.now();
    quote.bid = Price{499.95};
    quote.ask = Price{500.05};
    quote.bid_size = Qty{100};
    quote.ask_size = Qty{200};

    transport.inbound = {quote};
    auto pumped = adapter.pump();
    REQUIRE(pumped.has_value());
    REQUIRE(*pumped == 1);

    auto event = adapter.next();
    REQUIRE(event.has_value());
    // An empty buffer means "nothing right now", not "the stream ended".
    REQUIRE_FALSE(adapter.next().has_value());
}

TEST_CASE("out-of-order live events are dropped, not applied", "[live][marketdata][leakage]") {
    // The engine assumes non-decreasing event time; walking the clock backwards
    // breaks every rolling feature downstream.
    SimulatedClock clock{at("2024-07-02T15:00:00Z")};
    ScriptedTransport transport;
    LiveConnection connection{clock, transport};
    REQUIRE(connection.connect().has_value());
    LiveMarketDataAdapter adapter{clock, connection};

    BrokerMessage later;
    later.kind = MessageKind::Quote;
    later.instrument = kSpy;
    // Within the connection's clock-skew tolerance, so this test exercises the
    // ADAPTER's monotonicity guard rather than the connection's skew guard.
    later.venue_time = clock.now() + seconds{2};
    later.bid = Price{499.95};
    later.ask = Price{500.05};
    later.bid_size = Qty{100};
    later.ask_size = Qty{100};

    BrokerMessage earlier = later;
    earlier.venue_time = clock.now();  // arrives second, but is older

    transport.inbound = {later, earlier};
    auto pumped = adapter.pump();
    REQUIRE(pumped.has_value());
    REQUIRE(*pumped == 1);
    REQUIRE(adapter.dropped_stale() == 1);
}

TEST_CASE("a venue timestamp far ahead of our clock is dropped", "[live][connection][edge]") {
    // Clock skew, not a valid future event. This guard sits at the CONNECTION,
    // ahead of the adapter's monotonicity check -- a message dropped here never
    // reaches the market data buffer at all, which is worth knowing when
    // debugging a missing quote.
    SimulatedClock clock{at("2024-07-02T15:00:00Z")};
    ScriptedTransport transport;
    LiveConnection connection{clock, transport};
    REQUIRE(connection.connect().has_value());

    BrokerMessage skewed;
    skewed.kind = MessageKind::Quote;
    skewed.venue_time = clock.now() + seconds{30};

    transport.inbound = {skewed};
    REQUIRE(connection.poll().empty());
    REQUIRE(connection.stats().out_of_order_dropped == 1);
}

// ---------------------------------------------------------------------------
// Session
// ---------------------------------------------------------------------------

TEST_CASE("live session state round-trips and detects corruption",
          "[live][session][serialization][recovery]") {
    LiveSessionState state;
    state.session_id = "live1";
    state.sequence = 4;
    state.config_fingerprint = 0xFEEDFACE;
    state.events_processed = 1234;
    state.last_venue_sequence = 987;
    state.account.cash = Notional{50'000.123456789};
    state.account.equity = Notional{101'250.987654321};
    state.tracked_orders.emplace_back("ptl-1", 1);
    state.tracked_orders.emplace_back("ptl-2", 2);

    const std::string json = state.to_json();
    auto restored = LiveSessionState::from_json(json);
    REQUIRE(restored.has_value());
    REQUIRE(restored->events_processed == 1234);
    REQUIRE(restored->last_venue_sequence == 987);
    REQUIRE(restored->tracked_orders.size() == 2);
    REQUIRE(restored->tracked_orders.front().first == "ptl-1");
    // Round-trip EXACT: the checksum covers the raw bits.
    REQUIRE(restored->account.cash.get() == 50'000.123456789);

    std::string tampered = json;
    const auto pos = tampered.find("1234");
    REQUIRE(pos != std::string::npos);
    tampered.replace(pos, 4, "9999");
    REQUIRE_FALSE(LiveSessionState::from_json(tampered).has_value());
    REQUIRE_FALSE(LiveSessionState::from_json("{}").has_value());
}

TEST_CASE("a live session refuses to trade before it is synchronized", "[live][session][leakage]") {
    const auto root = std::filesystem::temp_directory_path() / "ptl_live_sync";
    std::filesystem::remove_all(root);
    storage::ArtifactStore artifacts{root.string()};

    LiveHarness h;
    LiveMarketDataAdapter adapter{h.clock, h.connection};

    class NullStrategy final : public engine::IStrategy {
    public:
        [[nodiscard]] std::string_view name() const noexcept override { return "null"; }
    };
    NullStrategy strategy;
    oms::OrderManager oms;
    risk::RiskManager risk{risk::RiskLimits{}};
    accounting::Journal journal;

    LiveSessionConfig config;
    config.session_id = "sync";
    LiveSession session{config,      h.clock,     h.connection, adapter, h.broker, strategy,
                        h.simulator, h.portfolio, oms,          risk,    journal,  artifacts};

    // Before start, trading is not permitted.
    REQUIRE_FALSE(session.trading_permitted());
    REQUIRE(session.start().has_value());
    // start() connects AND synchronises, so trading is now permitted.
    REQUIRE(session.phase() == LiveSessionPhase::Running);
    REQUIRE(session.trading_permitted());

    session.halt("market closed");
    REQUIRE(session.phase() == LiveSessionPhase::Halted);
    REQUIRE_FALSE(session.trading_permitted());
    REQUIRE(session.resume().has_value());

    REQUIRE(session.shutdown().has_value());
    REQUIRE(session.phase() == LiveSessionPhase::Stopped);
    REQUIRE(artifacts.contains("live/sync/state"));

    std::filesystem::remove_all(root);
}

TEST_CASE("a live session recovers its venue sequence after restart",
          "[live][session][recovery][leakage]") {
    // Without this a redelivered message after a restart would be applied as
    // new -- the duplicate-fill scenario, across a process boundary.
    const auto root = std::filesystem::temp_directory_path() / "ptl_live_recover";
    std::filesystem::remove_all(root);
    storage::ArtifactStore artifacts{root.string()};

    LiveSessionState prior;
    prior.session_id = "recover";
    prior.config_fingerprint = 0x1234;
    prior.events_processed = 500;
    prior.last_venue_sequence = 4242;
    prior.tracked_orders.emplace_back("ptl-7", 7);
    REQUIRE(artifacts.put("live/recover/state", prior.to_json()).has_value());

    LiveHarness h;
    LiveMarketDataAdapter adapter{h.clock, h.connection};

    class NullStrategy final : public engine::IStrategy {
    public:
        [[nodiscard]] std::string_view name() const noexcept override { return "null"; }
    };
    NullStrategy strategy;
    oms::OrderManager oms;
    risk::RiskManager risk{risk::RiskLimits{}};
    accounting::Journal journal;

    LiveSessionConfig config;
    config.session_id = "recover";
    config.config_fingerprint = 0x1234;
    LiveSession session{config,      h.clock,     h.connection, adapter, h.broker, strategy,
                        h.simulator, h.portfolio, oms,          risk,    journal,  artifacts};

    REQUIRE(session.start().has_value());
    REQUIRE(session.stats().events_processed == 500);
    REQUIRE(h.connection.last_sequence() == 4242);
    REQUIRE(h.broker.listener().tracked() == 1);

    std::filesystem::remove_all(root);
}

TEST_CASE("a live session refuses to resume under a different configuration",
          "[live][session][recovery][leakage]") {
    const auto root = std::filesystem::temp_directory_path() / "ptl_live_cfg";
    std::filesystem::remove_all(root);
    storage::ArtifactStore artifacts{root.string()};

    LiveSessionState prior;
    prior.session_id = "cfg";
    prior.config_fingerprint = 0xAAAA;
    REQUIRE(artifacts.put("live/cfg/state", prior.to_json()).has_value());

    LiveHarness h;
    LiveMarketDataAdapter adapter{h.clock, h.connection};
    class NullStrategy final : public engine::IStrategy {
    public:
        [[nodiscard]] std::string_view name() const noexcept override { return "null"; }
    };
    NullStrategy strategy;
    oms::OrderManager oms;
    risk::RiskManager risk{risk::RiskLimits{}};
    accounting::Journal journal;

    LiveSessionConfig config;
    config.session_id = "cfg";
    config.config_fingerprint = 0xBBBB;
    LiveSession session{config,      h.clock,     h.connection, adapter, h.broker, strategy,
                        h.simulator, h.portfolio, oms,          risk,    journal,  artifacts};

    auto started = session.start();
    REQUIRE_FALSE(started.has_value());
    REQUIRE(started.error().message.find("different configuration") != std::string::npos);
    REQUIRE(session.phase() == LiveSessionPhase::Failed);

    std::filesystem::remove_all(root);
}
