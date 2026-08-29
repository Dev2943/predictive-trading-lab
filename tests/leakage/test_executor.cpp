#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <vector>

#include "ptl/algo/executor.hpp"
#include "ptl/engine/engine.hpp"
#include "support/ptl_catch.hpp"

using namespace ptl;
using namespace ptl::algo;
using namespace std::chrono;

namespace {

Timestamp at(const char* iso) {
    Timestamp ts{};
    REQUIRE(parse_timestamp(iso, ts));
    return ts;
}

constexpr InstrumentId kSpy{0};

/// A sink that records everything and can refuse, standing in for the engine.
///
/// It mirrors the real OrderSink contract exactly: ids come from a monotonic
/// counter and submit() may refuse. That is what lets the executor's routing be
/// tested without standing up an entire engine, while still proving the
/// executor holds nothing but a sink.
class RecordingSink final : public engine::OrderSink {
public:
    explicit RecordingSink(bool accept = true) : accept_(accept) {}

    [[nodiscard]] oms::OrderId next_order_id() override {
        return static_cast<oms::OrderId>(++next_);
    }
    [[nodiscard]] Result<oms::OrderId> submit(const oms::Order& order) override {
        submitted.push_back(order);
        if (!accept_) {
            return fail(make_error(ErrorCode::ValidationFailed, "risk refused"));
        }
        return order.id();
    }
    [[nodiscard]] Result<bool> cancel(oms::OrderId id) override {
        cancelled.push_back(id);
        return true;
    }

    std::vector<oms::Order> submitted;
    std::vector<oms::OrderId> cancelled;

private:
    bool accept_;
    std::uint64_t next_ = 100;
};

oms::Order parent_order(double qty = 1000.0) {
    LifecycleTimes t;
    t.decision_time = at("2024-07-02T15:00:00Z");
    auto o = oms::Order::market(oms::OrderId{1}, kSpy, Side::Buy, Qty{qty}, t);
    REQUIRE(o.has_value());
    return *o;
}

ExecutionRequest request_for(double qty = 1000.0) {
    ExecutionRequest r{parent_order(qty),
                       at("2024-07-02T15:00:00Z"),
                       at("2024-07-02T16:00:00Z"),
                       ExecutionPolicy{},
                       10,
                       {}};
    r.policy.min_clip_size = Qty{1.0};
    return r;
}

ExecutionContext context_at(const char* iso, double volume = 1e9) {
    ExecutionContext ctx;
    ctx.now = at(iso);
    ctx.market.state.bid = Price{499.90};
    ctx.market.state.ask = Price{500.10};
    ctx.market.state.bid_size = Qty{1e6};
    ctx.market.state.ask_size = Qty{1e6};
    ctx.market.state.has_quote = true;
    ctx.market.executable = true;
    ctx.interval_volume = Volume{volume};
    ctx.session_open = at("2024-07-02T13:30:00Z");
    ctx.session_close = at("2024-07-02T20:00:00Z");
    return ctx;
}

/// Fills come only from a BrokerSimulator; this drives a real one so the
/// executor is never handed a fabricated Fill.
oms::Fill fill_for(const oms::Order& child, double qty, double price) {
    SimulatedClock clock{child.decision_time()};
    execution::CostConfig cc;
    cc.commission_per_share = 0.0;
    cc.minimum_commission = 0.0;
    cc.stochastic_slippage_bps = Bps{0.0};
    cc.impact_coefficient = 0.0;
    execution::StandardCostModel costs{cc};
    execution::StandardLatencyModel latency;
    execution::FillConfig fc;
    fc.max_participation_rate = 1.0;
    fc.respect_displayed_size = false;
    execution::BrokerSimulator broker{clock, costs, latency, DeterministicRng{1}, fc};

    LifecycleTimes t;
    t.decision_time = clock.now();
    auto order = oms::Order::market(child.id(), child.instrument(), child.side(), Qty{qty}, t,
                                    oms::TimeInForce::Day, child.parent_id());
    REQUIRE(broker.submit(*order).has_value());

    execution::MarketState st;
    st.bid = Price{price};
    st.ask = Price{price};
    st.interval_volume = Volume{1e9};
    st.has_quote = true;
    clock.advance_by(seconds{1});
    auto fills = broker.on_market(child.instrument(), st, clock.now());
    REQUIRE(fills.has_value());
    REQUIRE(fills->size() == 1);
    return fills->front();
}

}  // namespace

TEST_CASE("children route through the sink and carry their parent id",
          "[algo][executor][leakage]") {
    // THE NO-BYPASS GUARANTEE. The executor holds an OrderSink and nothing
    // else -- no broker, no OMS -- so every child passes the Phase 3 risk gate.
    Executor executor{std::make_unique<TwapAlgorithm>()};
    RecordingSink sink;

    REQUIRE(executor.submit(request_for(), oms::OrderId{1}, Price{500.0}).has_value());
    auto emitted = executor.on_market(kSpy, context_at("2024-07-02T15:30:00Z"), sink);
    REQUIRE(emitted.has_value());
    REQUIRE(*emitted == 1);
    REQUIRE(sink.submitted.size() == 1);

    const auto& child = sink.submitted.front();
    REQUIRE(child.parent_id() == oms::OrderId{1});
    REQUIRE(child.is_child());
    REQUIRE(child.instrument() == kSpy);
    REQUIRE(child.side() == Side::Buy);
    // The child's decision instant is NOW, not the parent's: a child stamped at
    // the parent's time would claim to predate the data that motivated it.
    REQUIRE(child.decision_time() == at("2024-07-02T15:30:00Z"));
    REQUIRE(child.decision_time() > parent_order().decision_time());
}

TEST_CASE("a risk rejection is counted and the execution continues", "[algo][executor][risk]") {
    // A limit that risk refuses now may be acceptable once the book has moved,
    // so a rejection is not fatal to the execution.
    Executor executor{std::make_unique<TwapAlgorithm>()};
    RecordingSink refusing{false};

    REQUIRE(executor.submit(request_for(), oms::OrderId{1}, Price{500.0}).has_value());
    auto emitted = executor.on_market(kSpy, context_at("2024-07-02T15:30:00Z"), refusing);
    REQUIRE(emitted.has_value());
    REQUIRE(*emitted == 0);
    REQUIRE(executor.stats().child_orders_rejected == 1);
    REQUIRE(executor.stats().child_orders_emitted == 0);

    // The execution is still live and tries again.
    const auto* plan = executor.find(oms::OrderId{1});
    REQUIRE(plan != nullptr);
    REQUIRE_FALSE(is_terminal(plan->state));
}

TEST_CASE("only one child rests at a time", "[algo][executor][leakage]") {
    // Sending another while one is working would double the exposure the
    // algorithm intended.
    Executor executor{std::make_unique<TwapAlgorithm>()};
    RecordingSink sink;
    REQUIRE(executor.submit(request_for(), oms::OrderId{1}, Price{500.0}).has_value());

    REQUIRE(*executor.on_market(kSpy, context_at("2024-07-02T15:30:00Z"), sink) == 1);
    // A second event with a child still working emits nothing.
    REQUIRE(*executor.on_market(kSpy, context_at("2024-07-02T15:36:00Z"), sink) == 0);
    REQUIRE(sink.submitted.size() == 1);

    // Once the child fills, the next slice may go.
    REQUIRE(executor.on_fill(fill_for(sink.submitted.front(), 500.0, 500.0)).has_value());
    REQUIRE(*executor.on_market(kSpy, context_at("2024-07-02T15:42:00Z"), sink) == 1);
    REQUIRE(sink.submitted.size() == 2);
}

TEST_CASE("progress tracks fills and detects completion", "[algo][executor][progress]") {
    Executor executor{std::make_unique<ImmediateAlgorithm>()};
    RecordingSink sink;
    REQUIRE(executor.submit(request_for(1000.0), oms::OrderId{1}, Price{500.0}).has_value());

    REQUIRE(*executor.on_market(kSpy, context_at("2024-07-02T15:30:00Z"), sink) == 1);
    const auto& child = sink.submitted.front();

    // A partial fill leaves the execution working.
    REQUIRE(executor.on_fill(fill_for(child, 400.0, 500.0)).has_value());
    const auto* plan = executor.find(oms::OrderId{1});
    REQUIRE(plan->progress.filled.get() == Catch::Approx(400.0));
    REQUIRE(plan->progress.remaining.get() == Catch::Approx(600.0));
    REQUIRE_FALSE(is_terminal(plan->state));
    REQUIRE(plan->progress.completion_ratio(Qty{1000}) == Catch::Approx(0.4));

    // The rest completes it.
    REQUIRE(executor.on_fill(fill_for(child, 600.0, 501.0)).has_value());
    REQUIRE(plan->state == ExecutionState::Completed);
    REQUIRE(plan->progress.complete(Qty{1000}));
    REQUIRE(executor.stats().executions_completed == 1);

    // Average price is quantity-weighted across both fills.
    const auto avg = plan->progress.average_price();
    REQUIRE(avg.has_value());
    REQUIRE(avg->get() == Catch::Approx((400.0 * 500.0 + 600.0 * 501.0) / 1000.0));
}

TEST_CASE("a completed execution emits no further children", "[algo][executor][leakage]") {
    Executor executor{std::make_unique<TwapAlgorithm>()};
    RecordingSink sink;
    REQUIRE(executor.submit(request_for(100.0), oms::OrderId{1}, Price{500.0}).has_value());

    REQUIRE(*executor.on_market(kSpy, context_at("2024-07-02T15:59:00Z"), sink) == 1);
    REQUIRE(executor.on_fill(fill_for(sink.submitted.front(), 100.0, 500.0)).has_value());
    REQUIRE(executor.find(oms::OrderId{1})->state == ExecutionState::Completed);

    const std::size_t before = sink.submitted.size();
    REQUIRE(*executor.on_market(kSpy, context_at("2024-07-02T15:59:30Z"), sink) == 0);
    REQUIRE(sink.submitted.size() == before);
}

TEST_CASE("cancel stops the execution and the resting child", "[algo][executor][cancel]") {
    // Cancelling the child first, then marking terminal: the other order would
    // leave a live child belonging to an execution that no longer exists.
    Executor executor{std::make_unique<TwapAlgorithm>()};
    RecordingSink sink;
    REQUIRE(executor.submit(request_for(), oms::OrderId{1}, Price{500.0}).has_value());
    REQUIRE(*executor.on_market(kSpy, context_at("2024-07-02T15:30:00Z"), sink) == 1);

    REQUIRE(executor.cancel(oms::OrderId{1}, sink, "strategy stop").has_value());
    REQUIRE(sink.cancelled.size() == 1);
    REQUIRE(sink.cancelled.front() == sink.submitted.front().id());
    REQUIRE(executor.find(oms::OrderId{1})->state == ExecutionState::Cancelled);
    REQUIRE(executor.stats().executions_cancelled == 1);

    // Cancelling twice is refused rather than silently succeeding.
    REQUIRE_FALSE(executor.cancel(oms::OrderId{1}, sink).has_value());
    REQUIRE_FALSE(executor.cancel(oms::OrderId{999}, sink).has_value());
    // And no further children are emitted.
    REQUIRE(*executor.on_market(kSpy, context_at("2024-07-02T15:40:00Z"), sink) == 0);
}

TEST_CASE("an expired window abandons the remainder rather than dumping it",
          "[algo][executor][adr]") {
    // Dumping converts a patient execution into the worst possible one at the
    // worst possible moment -- the end of its own window, when everyone else's
    // window is closing too.
    Executor executor{std::make_unique<TwapAlgorithm>()};
    RecordingSink sink;
    REQUIRE(executor.submit(request_for(1000.0), oms::OrderId{1}, Price{500.0}).has_value());
    REQUIRE(*executor.on_market(kSpy, context_at("2024-07-02T15:30:00Z"), sink) == 1);

    const std::size_t before = sink.submitted.size();
    const std::size_t expired = executor.expire_stale(at("2024-07-02T16:00:00Z"), sink);
    REQUIRE(expired == 1);
    REQUIRE(executor.find(oms::OrderId{1})->state == ExecutionState::Expired);
    REQUIRE(executor.stats().executions_expired == 1);
    // No dump: nothing new was sent.
    REQUIRE(sink.submitted.size() == before);
    // The resting child was cancelled.
    REQUIRE(sink.cancelled.size() == 1);
}

TEST_CASE("a slice cannot be released before its window opens", "[algo][executor][leakage]") {
    Executor executor{std::make_unique<TwapAlgorithm>()};
    RecordingSink sink;
    REQUIRE(executor.submit(request_for(), oms::OrderId{1}, Price{500.0}).has_value());

    // Before the window: nothing.
    REQUIRE(*executor.on_market(kSpy, context_at("2024-07-02T14:00:00Z"), sink) == 0);
    REQUIRE(sink.submitted.empty());
    // Inside it: a child.
    REQUIRE(*executor.on_market(kSpy, context_at("2024-07-02T15:30:00Z"), sink) == 1);
}

TEST_CASE("a non-executable market releases nothing", "[algo][executor][leakage]") {
    // Halted, stale and crossed quotes all mean no venue would accept the
    // child, so none is sent.
    Executor executor{std::make_unique<TwapAlgorithm>()};
    RecordingSink sink;
    REQUIRE(executor.submit(request_for(), oms::OrderId{1}, Price{500.0}).has_value());

    auto halted = context_at("2024-07-02T15:30:00Z");
    halted.market.executable = false;
    REQUIRE(*executor.on_market(kSpy, halted, sink) == 0);
    REQUIRE(sink.submitted.empty());
    // The execution survives; it simply waits.
    REQUIRE_FALSE(is_terminal(executor.find(oms::OrderId{1})->state));
}

TEST_CASE("executions for other instruments are untouched", "[algo][executor]") {
    Executor executor{std::make_unique<TwapAlgorithm>()};
    RecordingSink sink;
    REQUIRE(executor.submit(request_for(), oms::OrderId{1}, Price{500.0}).has_value());

    REQUIRE(*executor.on_market(InstrumentId{7}, context_at("2024-07-02T15:30:00Z"), sink) == 0);
    REQUIRE(sink.submitted.empty());
}

TEST_CASE("a duplicate execution for one parent is refused", "[algo][executor][validation]") {
    Executor executor{std::make_unique<TwapAlgorithm>()};
    REQUIRE(executor.submit(request_for(), oms::OrderId{1}, Price{500.0}).has_value());
    REQUIRE_FALSE(executor.submit(request_for(), oms::OrderId{1}, Price{500.0}).has_value());
    REQUIRE_FALSE(executor.submit(request_for(), oms::kNoOrder, Price{500.0}).has_value());
}

TEST_CASE("an unknown fill is refused", "[algo][executor][validation]") {
    Executor executor{std::make_unique<TwapAlgorithm>()};
    RecordingSink sink;
    REQUIRE(executor.submit(request_for(), oms::OrderId{1}, Price{500.0}).has_value());
    REQUIRE(*executor.on_market(kSpy, context_at("2024-07-02T15:30:00Z"), sink) == 1);

    // A fill whose parent is not known to this executor.
    LifecycleTimes t;
    t.decision_time = at("2024-07-02T15:00:00Z");
    auto stranger = oms::Order::market(oms::OrderId{555}, kSpy, Side::Buy, Qty{10}, t);
    REQUIRE_FALSE(executor.on_fill(fill_for(*stranger, 10.0, 500.0)).has_value());
}

TEST_CASE("two identical runs emit identical children", "[algo][executor][determinism]") {
    // THE ADR-0004 DETERMINISM GUARANTEE. Same parent, same events, same
    // children -- to the bit.
    const auto run = [] {
        Executor executor{std::make_unique<TwapAlgorithm>()};
        RecordingSink sink;
        REQUIRE(executor.submit(request_for(), oms::OrderId{1}, Price{500.0}).has_value());

        for (int minute = 0; minute < 50; minute += 6) {
            const auto iso = "2024-07-02T15:" + std::string{minute < 10 ? "0" : ""} +
                             std::to_string(minute) + ":00Z";
            Timestamp ts{};
            REQUIRE(parse_timestamp(iso, ts));
            ExecutionContext ctx = context_at("2024-07-02T15:30:00Z");
            ctx.now = ts;
            (void)executor.on_market(kSpy, ctx, sink);
            if (!sink.submitted.empty() && executor.find(oms::OrderId{1})->working_child) {
                (void)executor.on_fill(
                    fill_for(sink.submitted.back(), sink.submitted.back().quantity().get(), 500.0));
            }
        }
        return std::make_pair(executor.content_hash(), sink.submitted.size());
    };
    REQUIRE(run() == run());
}

TEST_CASE("algorithms are swappable through one interface", "[algo][executor][interface]") {
    // No runtime kind checks in the driver: selecting an algorithm is holding a
    // different pointer.
    auto registry = AlgorithmRegistry::with_defaults();
    REQUIRE(registry.has_value());

    for (const auto& name : registry->names()) {
        INFO("algorithm: " << name);
        auto algo = registry->create(name);
        REQUIRE(algo.has_value());

        Executor executor{std::move(*algo)};
        RecordingSink sink;
        auto request = request_for();
        request.volume_profile = VolumeProfile::uniform(10);

        REQUIRE(executor.submit(request, oms::OrderId{1}, Price{500.0}).has_value());
        auto emitted = executor.on_market(kSpy, context_at("2024-07-02T15:30:00Z"), sink);
        REQUIRE(emitted.has_value());
        // Every algorithm produces a valid child through the same path.
        for (const auto& child : sink.submitted) {
            REQUIRE(child.parent_id() == oms::OrderId{1});
            REQUIRE(child.quantity().get() > 0.0);
        }
    }
}
