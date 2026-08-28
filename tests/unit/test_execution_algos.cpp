#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cmath>

#include "ptl/algo/algorithms.hpp"
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

oms::Order parent_order(double qty = 1000.0, Side side = Side::Buy) {
    LifecycleTimes t;
    t.decision_time = at("2024-07-02T15:00:00Z");
    auto o = oms::Order::market(oms::OrderId{1}, kSpy, side, Qty{qty}, t);
    REQUIRE(o.has_value());
    return *o;
}

ExecutionRequest request_for(double qty = 1000.0, Side side = Side::Buy) {
    ExecutionRequest r{parent_order(qty, side),
                       at("2024-07-02T15:00:00Z"),
                       at("2024-07-02T16:00:00Z"),
                       ExecutionPolicy{},
                       10,
                       {}};
    r.policy.min_clip_size = Qty{1.0};
    return r;
}

ExecutionContext context_at(const char* iso, double volume = 100000.0) {
    ExecutionContext ctx;
    ctx.now = at(iso);
    ctx.market.state.bid = Price{499.90};
    ctx.market.state.ask = Price{500.10};
    ctx.market.state.bid_size = Qty{5000};
    ctx.market.state.ask_size = Qty{5000};
    ctx.market.state.has_quote = true;
    ctx.market.executable = true;
    ctx.market.source = execution::PriceSource::Quote;
    ctx.interval_volume = Volume{volume};
    ctx.session_open = at("2024-07-02T13:30:00Z");
    ctx.session_close = at("2024-07-02T20:00:00Z");
    return ctx;
}

}  // namespace

// ---------------------------------------------------------------------------
// Schedules
// ---------------------------------------------------------------------------

TEST_CASE("TWAP divides quantity and time equally", "[algo][twap]") {
    auto s = ExecutionSchedule::twap(Qty{1000}, at("2024-07-02T15:00:00Z"),
                                     at("2024-07-02T16:00:00Z"), 10);
    REQUIRE(s.has_value());
    REQUIRE(s->size() == 10);
    REQUIRE(s->total_quantity().get() == Catch::Approx(1000.0));

    for (const auto& slice : s->slices()) {
        REQUIRE(slice.target_quantity.get() == Catch::Approx(100.0));
        REQUIRE(slice.end - slice.begin == minutes{6});
    }
    // The window closes exactly at the requested end, not short of it.
    REQUIRE(s->begin() == at("2024-07-02T15:00:00Z"));
    REQUIRE(s->end() == at("2024-07-02T16:00:00Z"));
    REQUIRE(s->slices().back().cumulative_target.get() == Catch::Approx(1000.0));
}

TEST_CASE("the cumulative target interpolates within a slice", "[algo][twap]") {
    // Stepping only at slice boundaries would make an algorithm alternate
    // between far behind and exactly on schedule, and every clip would be a
    // full slice regardless of elapsed time.
    auto s = ExecutionSchedule::twap(Qty{1000}, at("2024-07-02T15:00:00Z"),
                                     at("2024-07-02T16:00:00Z"), 10);
    REQUIRE(s->target_by(at("2024-07-02T14:59:00Z")).get() == Catch::Approx(0.0));
    // Half of the first six-minute slice.
    REQUIRE(s->target_by(at("2024-07-02T15:03:00Z")).get() == Catch::Approx(50.0));
    REQUIRE(s->target_by(at("2024-07-02T15:06:00Z")).get() == Catch::Approx(100.0));
    REQUIRE(s->target_by(at("2024-07-02T15:30:00Z")).get() == Catch::Approx(500.0));
    // Past the end, everything is due.
    REQUIRE(s->target_by(at("2024-07-02T17:00:00Z")).get() == Catch::Approx(1000.0));
}

TEST_CASE("VWAP allocates in proportion to the volume profile", "[algo][vwap]") {
    // Trade more where the market trades more -- the whole point of a VWAP.
    const std::vector<double> profile{0.4, 0.1, 0.1, 0.4};
    auto s = ExecutionSchedule::vwap(Qty{1000}, at("2024-07-02T15:00:00Z"),
                                     at("2024-07-02T16:00:00Z"), profile);
    REQUIRE(s.has_value());
    REQUIRE(s->size() == 4);
    REQUIRE(s->slices()[0].target_quantity.get() == Catch::Approx(400.0));
    REQUIRE(s->slices()[1].target_quantity.get() == Catch::Approx(100.0));
    REQUIRE(s->slices()[3].target_quantity.get() == Catch::Approx(400.0));
    REQUIRE(s->total_quantity().get() == Catch::Approx(1000.0));
}

TEST_CASE("an unnormalised profile is normalised, a zero one refused", "[algo][vwap][edge]") {
    // A profile that does not sum to one is far more common than a bug.
    const std::vector<double> unnormalised{4.0, 1.0, 1.0, 4.0};
    auto s = ExecutionSchedule::vwap(Qty{1000}, at("2024-07-02T15:00:00Z"),
                                     at("2024-07-02T16:00:00Z"), unnormalised);
    REQUIRE(s.has_value());
    REQUIRE(s->slices()[0].target_quantity.get() == Catch::Approx(400.0));

    // A profile summing to zero carries no information at all.
    const std::vector<double> zeros{0.0, 0.0, 0.0};
    auto refused = ExecutionSchedule::vwap(Qty{1000}, at("2024-07-02T15:00:00Z"),
                                           at("2024-07-02T16:00:00Z"), zeros);
    REQUIRE_FALSE(refused.has_value());
    REQUIRE(refused.error().message.find("sums to zero") != std::string::npos);

    const std::vector<double> negative{1.0, -1.0};
    REQUIRE_FALSE(ExecutionSchedule::vwap(Qty{1000}, at("2024-07-02T15:00:00Z"),
                                          at("2024-07-02T16:00:00Z"), negative)
                      .has_value());
}

TEST_CASE("schedules refuse incoherent windows", "[algo][schedule][validation]") {
    REQUIRE_FALSE(
        ExecutionSchedule::twap(Qty{0}, at("2024-07-02T15:00:00Z"), at("2024-07-02T16:00:00Z"), 10)
            .has_value());
    REQUIRE_FALSE(ExecutionSchedule::twap(Qty{1000}, at("2024-07-02T16:00:00Z"),
                                          at("2024-07-02T15:00:00Z"), 10)
                      .has_value());
    REQUIRE_FALSE(ExecutionSchedule::twap(Qty{1000}, at("2024-07-02T15:00:00Z"),
                                          at("2024-07-02T16:00:00Z"), 0)
                      .has_value());
}

TEST_CASE("the volume profile is built from history only", "[algo][vwap][leakage]") {
    // A profile estimated from the session being traded would be textbook
    // lookahead: the algorithm would know where the volume was going to be.
    VolumeProfile profile{4};
    REQUIRE_FALSE(profile.ready());
    // With no history the window profile is refused rather than silently
    // degenerating to uniform.
    REQUIRE_FALSE(profile.window_profile(0, 4).has_value());

    REQUIRE(profile.add_session(std::vector<double>{400.0, 100.0, 100.0, 400.0}).has_value());
    REQUIRE(profile.ready());
    REQUIRE(profile.sessions_observed() == 1);
    REQUIRE(profile.shares()[0] == Catch::Approx(0.4));

    auto window = profile.window_profile(1, 2);
    REQUIRE(window.has_value());
    REQUIRE(window->size() == 2);

    // A mismatched session length is refused rather than truncated.
    REQUIRE_FALSE(profile.add_session(std::vector<double>{1.0}).has_value());
    REQUIRE_FALSE(profile.window_profile(2, 10).has_value());
}

// ---------------------------------------------------------------------------
// Clip sizing and policy
// ---------------------------------------------------------------------------

TEST_CASE("the clip chases the cumulative shortfall", "[algo][clip]") {
    // A slice skipped or capped is made up later rather than lost, which keeps
    // an execution on track after a halt or a thin interval.
    const auto request = request_for(1000.0);
    auto schedule =
        ExecutionSchedule::twap(Qty{1000}, request.window_begin, request.window_end, 10);
    ExecutionProgress progress;
    progress.remaining = Qty{1000};

    // Thirty minutes in, 500 is due and nothing is done.
    const auto clip = AlgorithmBase::clip_quantity(request, *schedule, progress,
                                                   context_at("2024-07-02T15:30:00Z"));
    REQUIRE(clip.get() == Catch::Approx(500.0));

    // Already ahead: nothing to send.
    progress.filled = Qty{600};
    progress.remaining = Qty{400};
    REQUIRE(AlgorithmBase::clip_quantity(request, *schedule, progress,
                                         context_at("2024-07-02T15:30:00Z"))
                .get() == Catch::Approx(0.0));
}

TEST_CASE("the participation cap binds against observed volume", "[algo][clip][property]") {
    auto request = request_for(10000.0);
    request.policy.max_participation_rate = 0.10;
    auto schedule =
        ExecutionSchedule::twap(Qty{10000}, request.window_begin, request.window_end, 10);
    ExecutionProgress progress;
    progress.remaining = Qty{10000};

    // 1000 due, but only 5000 traded this interval, so 500 is the cap.
    const auto clip = AlgorithmBase::clip_quantity(request, *schedule, progress,
                                                   context_at("2024-07-02T15:06:00Z", 5000.0));
    REQUIRE(clip.get() == Catch::Approx(500.0));
}

TEST_CASE("the minimum clip is applied after every reduction", "[algo][clip][edge]") {
    // Checking it earlier would let a participation cap shrink a clip below the
    // floor and send it anyway.
    auto request = request_for(1000.0);
    request.policy.min_clip_size = Qty{100.0};
    request.policy.max_participation_rate = 0.10;
    auto schedule =
        ExecutionSchedule::twap(Qty{1000}, request.window_begin, request.window_end, 10);
    ExecutionProgress progress;
    progress.remaining = Qty{1000};

    // The cap reduces the clip to 10, below the 100 floor: send nothing.
    ExecutionStatistics stats;
    const auto clip = AlgorithmBase::clip_quantity(
        request, *schedule, progress, context_at("2024-07-02T15:06:00Z", 100.0), &stats);
    REQUIRE(clip.get() == Catch::Approx(0.0));
    REQUIRE(stats.slices_skipped_min_clip == 1);
}

TEST_CASE("a final residue below the floor is still sent", "[algo][clip][edge]") {
    // Otherwise the execution could never complete: it would sit forever
    // holding a residue too small to be worth sending.
    auto request = request_for(1000.0);
    request.policy.min_clip_size = Qty{100.0};
    auto schedule =
        ExecutionSchedule::twap(Qty{1000}, request.window_begin, request.window_end, 10);
    ExecutionProgress progress;
    progress.filled = Qty{995};
    progress.remaining = Qty{5};

    const auto clip = AlgorithmBase::clip_quantity(request, *schedule, progress,
                                                   context_at("2024-07-02T16:00:00Z"));
    REQUIRE(clip.get() == Catch::Approx(5.0));
}

TEST_CASE("lot rounding never increases the clip", "[algo][clip][property]") {
    auto request = request_for(1000.0);
    request.policy.lot_size = 7.0;
    auto schedule =
        ExecutionSchedule::twap(Qty{1000}, request.window_begin, request.window_end, 10);
    ExecutionProgress progress;
    progress.remaining = Qty{1000};

    const auto clip = AlgorithmBase::clip_quantity(request, *schedule, progress,
                                                   context_at("2024-07-02T15:30:00Z"));
    REQUIRE(clip.get() <= 500.0);
    REQUIRE(std::fmod(clip.get(), 7.0) == Catch::Approx(0.0));
}

TEST_CASE("slices are not released outside the window or the session", "[algo][policy][leakage]") {
    const auto request = request_for();
    ExecutionStatistics stats;

    // Before the window opens.
    REQUIRE_FALSE(AlgorithmBase::releasable(request, context_at("2024-07-02T14:00:00Z"), &stats));
    // After it closes.
    REQUIRE_FALSE(AlgorithmBase::releasable(request, context_at("2024-07-02T17:00:00Z"), &stats));
    // Inside it, fine.
    REQUIRE(AlgorithmBase::releasable(request, context_at("2024-07-02T15:30:00Z"), &stats));

    // Auction protection: inside the opening buffer.
    auto opening = context_at("2024-07-02T15:30:00Z");
    opening.session_open = at("2024-07-02T15:30:00Z");
    REQUIRE_FALSE(AlgorithmBase::releasable(request, opening, &stats));

    // A non-executable market -- halted, stale or crossed -- releases nothing.
    auto halted = context_at("2024-07-02T15:30:00Z");
    halted.market.executable = false;
    REQUIRE_FALSE(AlgorithmBase::releasable(request, halted, &stats));
    REQUIRE(stats.slices_skipped_not_executable == 1);
}

TEST_CASE("limit prices are offset away from the touch and collared", "[algo][policy][property]") {
    auto request = request_for(1000.0, Side::Buy);
    request.policy.price_collar = Bps{1000.0};
    const auto ctx = context_at("2024-07-02T15:30:00Z");

    const auto buy_limit = AlgorithmBase::collared_limit(request, ctx, Bps{10.0});
    REQUIRE(buy_limit.has_value());
    // A buy limit sits BELOW the ask; offsetting the other way would cross the
    // spread and make the limit a market order in disguise.
    REQUIRE(buy_limit->get() < ctx.market.state.ask.get());

    auto sell_request = request_for(1000.0, Side::Sell);
    sell_request.policy.price_collar = Bps{1000.0};
    const auto sell_limit = AlgorithmBase::collared_limit(sell_request, ctx, Bps{10.0});
    REQUIRE(sell_limit->get() > ctx.market.state.bid.get());

    // A tight collar clamps the offset.
    request.policy.price_collar = Bps{1.0};
    const auto collared = AlgorithmBase::collared_limit(request, ctx, Bps{500.0});
    REQUIRE(collared.has_value());
    const double deviation = std::abs(to_bps(*collared, ctx.market.state.ask).get());
    REQUIRE(deviation <= 1.5);
}

// ---------------------------------------------------------------------------
// Algorithms
// ---------------------------------------------------------------------------

TEST_CASE("every algorithm satisfies the same interface", "[algo][interface]") {
    // No runtime kind checks anywhere: selection is holding a different
    // pointer, and adding a seventh algorithm means adding a class.
    auto registry = AlgorithmRegistry::with_defaults();
    REQUIRE(registry.has_value());
    REQUIRE(registry->size() == 6);

    const auto names = registry->names();
    // Ordered, so a report listing algorithms is identical between runs.
    REQUIRE(names.front() == "adaptive_limit");
    REQUIRE(registry->contains("twap"));
    REQUIRE(registry->contains("pov"));

    auto request = request_for();
    request.volume_profile = VolumeProfile::uniform(10);

    for (const auto& name : names) {
        INFO("algorithm: " << name);
        auto algo = registry->create(name);
        REQUIRE(algo.has_value());
        auto plan = (*algo)->plan(request);
        REQUIRE(plan.has_value());
        REQUIRE_FALSE(plan->empty());
    }

    auto unknown = registry->create("nonexistent");
    REQUIRE_FALSE(unknown.has_value());
    REQUIRE(unknown.error().message.find("twap") != std::string::npos);
}

TEST_CASE("VWAP refuses to run without a profile", "[algo][vwap][adr]") {
    // A VWAP without a profile IS a TWAP, and reporting it as a VWAP would be
    // false.
    const auto request = request_for();  // no profile
    VwapAlgorithm vwap;
    auto plan = vwap.plan(request);
    REQUIRE_FALSE(plan.has_value());
    REQUIRE(plan.error().message.find("would be a TWAP") != std::string::npos);
}

TEST_CASE("POV paces against observed volume not the clock", "[algo][pov]") {
    // The distinction from VWAP: VWAP follows an expected profile computed in
    // advance; POV reacts to what is actually trading.
    ParticipationAlgorithm pov{0.10};
    const auto request = request_for(10000.0);
    auto schedule = pov.plan(request);
    REQUIRE(schedule.has_value());

    ExecutionProgress progress;
    progress.remaining = Qty{10000};

    // 5000 traded, 10% target -> 500.
    auto child =
        pov.next_child(request, *schedule, progress, context_at("2024-07-02T15:30:00Z", 5000.0));
    REQUIRE(child.has_value());
    REQUIRE(child->quantity.get() == Catch::Approx(500.0));

    // Twice the volume, twice the clip -- the clock did not change.
    auto busier =
        pov.next_child(request, *schedule, progress, context_at("2024-07-02T15:30:00Z", 10000.0));
    REQUIRE(busier->quantity.get() == Catch::Approx(1000.0));

    // Nothing traded, nothing sent.
    REQUIRE_FALSE(
        pov.next_child(request, *schedule, progress, context_at("2024-07-02T15:30:00Z", 0.0))
            .has_value());
}

TEST_CASE("the iceberg shows a clip and hides the rest", "[algo][iceberg][adr]") {
    // ADR-0003: refreshing on completion makes no claim about queue position.
    IcebergAlgorithm iceberg{Qty{100.0}};
    const auto request = request_for(1000.0);
    auto schedule = iceberg.plan(request);
    REQUIRE(schedule.has_value());

    ExecutionProgress progress;
    progress.remaining = Qty{1000};

    auto child =
        iceberg.next_child(request, *schedule, progress, context_at("2024-07-02T15:30:00Z"));
    REQUIRE(child.has_value());
    REQUIRE(child->quantity.get() == Catch::Approx(100.0));
    REQUIRE(child->hidden_quantity.get() == Catch::Approx(900.0));
    REQUIRE(child->type == oms::OrderType::Limit);
    REQUIRE(child->limit_price.has_value());

    // Near the end the clip shrinks to what remains.
    progress.filled = Qty{950};
    progress.remaining = Qty{50};
    auto tail =
        iceberg.next_child(request, *schedule, progress, context_at("2024-07-02T15:30:00Z"));
    REQUIRE(tail->quantity.get() == Catch::Approx(50.0));
    REQUIRE(tail->hidden_quantity.get() == Catch::Approx(0.0));
}

TEST_CASE("the adaptive limit crosses when it falls behind", "[algo][adaptive][property]") {
    // An adaptive algorithm that never crosses simply fails to complete in a
    // trending market, which is worse than paying the spread.
    AdaptiveLimitAlgorithm::Config cfg;
    cfg.urgency_threshold = 0.25;
    AdaptiveLimitAlgorithm adaptive{cfg};

    const auto request = request_for(1000.0);
    auto schedule = adaptive.plan(request);
    REQUIRE(schedule.has_value());

    // Early and on schedule: rest passively.
    ExecutionProgress on_track;
    on_track.filled = Qty{95};
    on_track.remaining = Qty{905};
    auto passive =
        adaptive.next_child(request, *schedule, on_track, context_at("2024-07-02T15:06:00Z"));
    REQUIRE(passive.has_value());
    REQUIRE(passive->type == oms::OrderType::Limit);

    // Badly behind: cross.
    ExecutionProgress behind;
    behind.filled = Qty{0};
    behind.remaining = Qty{1000};
    auto urgent =
        adaptive.next_child(request, *schedule, behind, context_at("2024-07-02T15:45:00Z"));
    REQUIRE(urgent.has_value());
    REQUIRE(urgent->type == oms::OrderType::Market);
}

TEST_CASE("immediate sends everything at once", "[algo][immediate]") {
    ImmediateAlgorithm immediate;
    const auto request = request_for(1000.0);
    auto schedule = immediate.plan(request);
    REQUIRE(schedule.has_value());
    REQUIRE(schedule->size() == 1);

    ExecutionProgress progress;
    progress.remaining = Qty{1000};
    auto child =
        immediate.next_child(request, *schedule, progress, context_at("2024-07-02T15:30:00Z", 1e9));
    REQUIRE(child.has_value());
    REQUIRE(child->quantity.get() == Catch::Approx(1000.0));
    REQUIRE(child->type == oms::OrderType::Market);
}

TEST_CASE("an execution window before the parent decision is refused",
          "[algo][validation][leakage]") {
    // A window opening before the decision would let a child arrive before its
    // own parent existed.
    ExecutionRequest r{parent_order(),
                       at("2024-07-02T14:00:00Z"),
                       at("2024-07-02T16:00:00Z"),
                       ExecutionPolicy{},
                       10,
                       {}};
    auto validated = r.validate();
    REQUIRE_FALSE(validated.has_value());
    REQUIRE(validated.error().message.find("before the parent") != std::string::npos);
}

TEST_CASE("algorithm planning is deterministic", "[algo][determinism]") {
    const auto build = [] {
        TwapAlgorithm twap;
        auto plan = twap.plan(request_for());
        REQUIRE(plan.has_value());
        std::vector<double> targets;
        for (const auto& s : plan->slices()) targets.push_back(s.cumulative_target.get());
        return targets;
    };
    REQUIRE(build() == build());
}

TEST_CASE("duplicate algorithm registration is refused", "[algo][registry]") {
    AlgorithmRegistry reg;
    REQUIRE(reg.register_algorithm("twap", std::make_unique<TwapAlgorithm>()).has_value());
    REQUIRE_FALSE(reg.register_algorithm("twap", std::make_unique<TwapAlgorithm>()).has_value());
    REQUIRE_FALSE(reg.register_algorithm("", std::make_unique<TwapAlgorithm>()).has_value());
    REQUIRE_FALSE(reg.register_algorithm("null", nullptr).has_value());
}
