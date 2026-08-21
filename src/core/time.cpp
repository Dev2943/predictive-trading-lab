#include "ptl/core/time.hpp"

#include <array>
#include <atomic>

namespace ptl {
namespace {

// Relaxed ordering: this is a diagnostic counter reported at end of run, never
// a synchronisation point. The simulation core is single-threaded by design
// (docs/01-research-reconciliation.md E5); the atomic exists so that when
// coarse-grained parallelism over folds arrives, the count stays correct.
std::atomic<std::uint64_t> g_chain_violations{0};

/// Adjacent-stage rules. StrictlyAfter appears exactly once, between decision
/// and arrival, and that entry is the no-same-bar-execution rule.
struct Edge {
    Stage     from;
    Stage     to;
    ChainRule rule;
};

constexpr std::array<Edge, 7> kEdges{{
    {Stage::ExchangeTime,   Stage::ReceiveTime,    ChainRule::Monotonic},
    {Stage::ReceiveTime,    Stage::FeatureEndTime, ChainRule::Monotonic},
    {Stage::FeatureEndTime, Stage::DecisionTime,   ChainRule::Monotonic},
    {Stage::DecisionTime,   Stage::SubmittedTime,  ChainRule::Monotonic},
    {Stage::SubmittedTime,  Stage::ArrivalTime,    ChainRule::Monotonic},
    {Stage::ArrivalTime,    Stage::FillTime,       ChainRule::Monotonic},
    {Stage::FillTime,       Stage::AckTime,        ChainRule::Monotonic},
}};

}  // namespace

std::string_view to_string(Stage s) noexcept {
    switch (s) {
        case Stage::ExchangeTime:   return "exchange_time";
        case Stage::ReceiveTime:    return "receive_time";
        case Stage::FeatureEndTime: return "feature_end_time";
        case Stage::DecisionTime:   return "decision_time";
        case Stage::SubmittedTime:  return "submitted_time";
        case Stage::ArrivalTime:    return "arrival_time";
        case Stage::FillTime:       return "fill_time";
        case Stage::AckTime:        return "ack_time";
        case Stage::Count:          break;
    }
    return "unknown";
}

Timestamp LifecycleTimes::at(Stage s) const noexcept {
    switch (s) {
        case Stage::ExchangeTime:   return exchange_time;
        case Stage::ReceiveTime:    return receive_time;
        case Stage::FeatureEndTime: return feature_end_time;
        case Stage::DecisionTime:   return decision_time;
        case Stage::SubmittedTime:  return submitted_time;
        case Stage::ArrivalTime:    return arrival_time;
        case Stage::FillTime:       return fill_time;
        case Stage::AckTime:        return ack_time;
        case Stage::Count:          break;
    }
    return kNoTimestamp;
}

void LifecycleTimes::set(Stage s, Timestamp ts) noexcept {
    switch (s) {
        case Stage::ExchangeTime:   exchange_time = ts;    return;
        case Stage::ReceiveTime:    receive_time = ts;     return;
        case Stage::FeatureEndTime: feature_end_time = ts; return;
        case Stage::DecisionTime:   decision_time = ts;    return;
        case Stage::SubmittedTime:  submitted_time = ts;   return;
        case Stage::ArrivalTime:    arrival_time = ts;     return;
        case Stage::FillTime:       fill_time = ts;        return;
        case Stage::AckTime:        ack_time = ts;         return;
        case Stage::Count:          return;
    }
}

std::string ChainViolation::describe() const {
    std::string out{to_string(later)};
    out += (rule == ChainRule::StrictlyAfter) ? " must be strictly after " : " must be at or after ";
    out += to_string(earlier);
    out += ": ";
    out += to_iso8601(later_ts);
    out += " vs ";
    out += to_iso8601(earlier_ts);
    if (rule == ChainRule::StrictlyAfter && later_ts == earlier_ts) {
        out += "  [same-bar execution: a decision cannot fill at the price that produced it]";
    }
    return out;
}

std::optional<ChainViolation> validate_chain(const LifecycleTimes& t) noexcept {
    // The strict decision -> arrival rule is checked FIRST, deliberately.
    //
    // An arrival at or before its own decision usually also inverts the
    // submitted -> arrival edge, so a naive left-to-right walk would report
    // "arrival before submission" -- technically true, but it buries the actual
    // conceptual error. Same-bar execution is the headline invariant of this
    // project and should be the diagnosis the developer sees.
    //
    // It is kept out of kEdges because submitted_time MAY legitimately equal
    // decision_time (a model of zero compute latency is optimistic, not
    // incoherent). It is specifically arrival that must be strictly later: an
    // order cannot reach the venue at the instant that produced it.
    if (is_set(t.decision_time) && is_set(t.arrival_time) &&
        t.arrival_time <= t.decision_time) {
        return ChainViolation{Stage::DecisionTime, Stage::ArrivalTime, t.decision_time,
                              t.arrival_time, ChainRule::StrictlyAfter};
    }

    // Then the adjacent pairs, skipping stages not yet populated. Skipping
    // rather than failing lets one type serve every point in the pipeline: an
    // order that has been submitted but not filled is valid, not incomplete.
    for (const auto& e : kEdges) {
        const Timestamp a = t.at(e.from);
        const Timestamp b = t.at(e.to);
        if (!is_set(a) || !is_set(b)) continue;
        if (b < a) return ChainViolation{e.from, e.to, a, b, e.rule};
    }
    return std::nullopt;
}

std::uint64_t chain_violation_count() noexcept {
    return g_chain_violations.load(std::memory_order_relaxed);
}
void record_chain_violation() noexcept {
    g_chain_violations.fetch_add(1, std::memory_order_relaxed);
}
void reset_chain_violation_count() noexcept {
    g_chain_violations.store(0, std::memory_order_relaxed);
}

}  // namespace ptl
