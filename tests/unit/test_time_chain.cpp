#include <catch2/catch_test_macros.hpp>

#include "support/ptl_catch.hpp"

#include "ptl/core/time.hpp"

using namespace ptl;
using namespace std::chrono;

namespace {

Timestamp at(const char* text) {
    Timestamp ts{};
    REQUIRE(parse_timestamp(text, ts));
    return ts;
}

/// A realistic, well-ordered chain for one decision on the close of the
/// 14:52 bar, executing into the 14:53 bar.
LifecycleTimes reference_chain() {
    const Timestamp bar_open = at("2024-01-02T14:52:00Z");
    LifecycleTimes t;
    t.exchange_time    = bar_open;
    t.receive_time     = bar_open + milliseconds{2};      // feed latency
    t.feature_end_time = bar_open + seconds{60};          // bar CLOSE, not open
    t.decision_time    = bar_open + seconds{60} + microseconds{300};
    t.submitted_time   = t.decision_time + microseconds{250};
    t.arrival_time     = t.submitted_time + milliseconds{1};
    t.fill_time        = t.arrival_time + microseconds{80};
    t.ack_time         = t.fill_time + milliseconds{1};
    return t;
}

}  // namespace

TEST_CASE("a well-ordered chain validates", "[core][pit]") {
    REQUIRE_FALSE(validate_chain(reference_chain()).has_value());
}

TEST_CASE("partial chains validate over populated stages only", "[core][pit]") {
    // An order that has been submitted but not yet filled is valid, not
    // incomplete. One type must serve every point in the pipeline.
    LifecycleTimes t;
    t.exchange_time = at("2024-01-02T14:52:00Z");
    t.receive_time  = t.exchange_time + milliseconds{2};
    REQUIRE_FALSE(validate_chain(t).has_value());

    LifecycleTimes empty;
    REQUIRE_FALSE(validate_chain(empty).has_value());
}

TEST_CASE("every adjacent stage inversion is detected", "[core][pit]") {
    struct Case {
        const char* name;
        Stage       earlier;
        Stage       later;
    };
    const Case cases[] = {
        {"feed", Stage::ExchangeTime, Stage::ReceiveTime},
        {"features before receive", Stage::ReceiveTime, Stage::FeatureEndTime},
        {"decision before features", Stage::FeatureEndTime, Stage::DecisionTime},
        {"submit before decision", Stage::DecisionTime, Stage::SubmittedTime},
        {"arrive before submit", Stage::SubmittedTime, Stage::ArrivalTime},
        {"fill before arrival", Stage::ArrivalTime, Stage::FillTime},
        {"ack before fill", Stage::FillTime, Stage::AckTime},
    };

    for (const auto& c : cases) {
        INFO("case: " << c.name);
        LifecycleTimes t = reference_chain();
        // Push the later stage one nanosecond before the earlier one.
        t.set(c.later, t.at(c.earlier) - nanoseconds{1});
        const auto v = validate_chain(t);
        REQUIRE(v.has_value());
    }
}

TEST_CASE("same-bar execution is rejected by the timestamp chain", "[core][pit][leakage]") {
    // THE central invariant. A strategy that observes a bar close and fills at
    // that same instant has arrival_time == decision_time. This must fail, and
    // it must fail here -- in the type -- rather than by convention inside the
    // execution code, so that no code path can honour it while another forgets.
    LifecycleTimes t = reference_chain();
    t.arrival_time = t.decision_time;

    const auto v = validate_chain(t);
    REQUIRE(v.has_value());
    REQUIRE(v->rule == ChainRule::StrictlyAfter);
    REQUIRE(v->earlier == Stage::DecisionTime);
    REQUIRE(v->later == Stage::ArrivalTime);
    REQUIRE(v->describe().find("same-bar execution") != std::string::npos);
}

TEST_CASE("arrival strictly before decision is rejected", "[core][pit][leakage]") {
    LifecycleTimes t = reference_chain();
    t.arrival_time = t.decision_time - seconds{1};
    REQUIRE(validate_chain(t).has_value());
}

TEST_CASE("zero compute latency is legal but zero arrival latency is not", "[core][pit]") {
    // submitted_time == decision_time models an infinitely fast strategy, which
    // is optimistic but not incoherent. arrival_time == decision_time models an
    // order that reaches the venue before it was sent, which is.
    LifecycleTimes t = reference_chain();
    t.submitted_time = t.decision_time;
    REQUIRE_FALSE(validate_chain(t).has_value());

    t.arrival_time = t.decision_time;
    REQUIRE(validate_chain(t).has_value());
}

TEST_CASE("EventTime exposes feed latency", "[core][pit]") {
    EventTime e;
    e.exchange_time = at("2024-01-02T14:52:00Z");
    e.receive_time  = e.exchange_time + milliseconds{3};
    REQUIRE(e.ok());
    REQUIRE(e.feed_latency() == milliseconds{3});

    e.receive_time = e.exchange_time - nanoseconds{1};
    REQUIRE_FALSE(e.ok());

    EventTime unset;
    REQUIRE_FALSE(unset.ok());
}

TEST_CASE("observation intervals detect label overlap", "[core][pit][validation]") {
    // With a 15-minute label horizon and a 5-minute decision step, consecutive
    // labels overlap. A purge that compares only label_end_time against the
    // test start leaves contaminated rows behind; the overlap test is what
    // catches them.
    ObservationInterval obs;
    obs.sample_start_time = at("2024-01-02T14:00:00Z");
    obs.feature_end_time  = at("2024-01-02T14:52:00Z");
    obs.label_start_time  = at("2024-01-02T14:52:00Z");
    obs.label_end_time    = at("2024-01-02T15:07:00Z");
    REQUIRE(obs.ok());

    const Timestamp test_begin = at("2024-01-02T15:00:00Z");
    const Timestamp test_end   = at("2024-01-02T16:00:00Z");

    // The label ENDS inside the test window: contaminated, must be purged --
    // even though feature_end_time is comfortably before the test start.
    REQUIRE(obs.label_overlaps(test_begin, test_end));
    REQUIRE(obs.feature_end_time < test_begin);

    // A label that closes before the window opens is clean.
    ObservationInterval clean = obs;
    clean.label_end_time = at("2024-01-02T14:59:00Z");
    REQUIRE_FALSE(clean.label_overlaps(test_begin, test_end));

    // Half-open: a label ending exactly at test_begin does not overlap.
    ObservationInterval boundary = obs;
    boundary.label_end_time = test_begin;
    REQUIRE_FALSE(boundary.label_overlaps(test_begin, test_end));
}

TEST_CASE("malformed observation intervals are rejected", "[core][pit][validation]") {
    ObservationInterval obs;
    obs.sample_start_time = at("2024-01-02T14:00:00Z");
    obs.feature_end_time  = at("2024-01-02T14:52:00Z");
    obs.label_start_time  = at("2024-01-02T14:30:00Z");  // before features end
    obs.label_end_time    = at("2024-01-02T15:07:00Z");
    REQUIRE_FALSE(obs.ok());

    obs.label_start_time = obs.feature_end_time;
    obs.label_end_time   = obs.label_start_time;  // zero-length label
    REQUIRE_FALSE(obs.ok());
}

TEST_CASE("violation counter is monotonic and resettable", "[core][pit]") {
    reset_chain_violation_count();
    REQUIRE(chain_violation_count() == 0);
    record_chain_violation();
    record_chain_violation();
    REQUIRE(chain_violation_count() == 2);
    reset_chain_violation_count();
    REQUIRE(chain_violation_count() == 0);
}
