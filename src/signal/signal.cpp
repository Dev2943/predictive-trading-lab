#include "ptl/signal/signal.hpp"

#include <algorithm>
#include <cmath>
#include <sstream>

namespace ptl::signal {
namespace {

[[nodiscard]] Error bad(std::string message, std::string context = {}) {
    return make_error(ErrorCode::ValidationFailed, std::move(message), std::move(context));
}

}  // namespace

std::string_view to_string(Direction d) noexcept {
    switch (d) {
        case Direction::Long:
            return "long";
        case Direction::Short:
            return "short";
        case Direction::Flat:
            return "flat";
    }
    return "unknown";
}

std::string CostEstimate::describe() const {
    std::ostringstream ss;
    ss.precision(6);
    ss << std::fixed << "spread=" << half_spread << " commission=" << commission
       << " slippage=" << slippage << " borrow=" << borrow << " turnover=" << turnover_penalty
       << " total=" << total();
    return ss.str();
}

Result<Signal> Signal::create(Timestamp as_of, InstrumentId instrument, Direction direction,
                              double expected_return, double confidence, Duration horizon,
                              std::uint64_t model_id, CostEstimate costs) {
    if (!is_set(as_of)) return fail(bad("signal has no decision timestamp"));
    if (instrument == kInvalidInstrument) return fail(bad("signal has no instrument"));
    if (!is_finite(expected_return)) return fail(bad("expected return is not finite"));
    if (!is_finite(confidence) || confidence < 0.0 || confidence > 1.0) {
        return fail(bad("confidence must lie in [0, 1]", std::to_string(confidence)));
    }
    if (horizon <= Duration::zero()) {
        // A signal with no horizon cannot expire, cannot be matched to a label,
        // and cannot have its realised edge measured.
        return fail(bad("signal horizon must be positive"));
    }
    if (model_id == 0) {
        // Provenance is not optional. Without it a disappointing month cannot
        // be attributed to a model, and the investigation has nowhere to start.
        return fail(bad("signal must carry a model id"));
    }
    const double cost_total = costs.total();
    if (!is_finite(cost_total) || cost_total < 0.0) {
        return fail(bad("cost estimate must be finite and non-negative"));
    }

    Signal s;
    s.as_of_ = as_of;
    s.instrument_ = instrument;
    s.direction_ = direction;
    s.expected_return_ = expected_return;
    s.confidence_ = confidence;
    s.horizon_ = horizon;
    s.model_id_ = model_id;
    s.costs_ = costs;

    // Net edge is computed HERE, once, at construction. Costs are subtracted
    // from the MAGNITUDE of the expected move: a small move in either direction
    // fails to pay for its round trip, and a signal whose gross looks positive
    // but whose net does not is a losing trade wearing an attractive label.
    s.net_edge_ = direction == Direction::Flat ? 0.0 : std::abs(expected_return) - cost_total;
    return s;
}

Signal Signal::flat(Timestamp as_of, InstrumentId instrument, std::uint64_t model_id) {
    // A deliberate no-position. Distinct from silence: the model spoke and said
    // stay out, which a coverage statistic needs to tell apart from a filter
    // having removed the signal entirely.
    Signal s;
    s.as_of_ = as_of;
    s.instrument_ = instrument;
    s.direction_ = Direction::Flat;
    s.horizon_ = std::chrono::minutes{1};
    s.model_id_ = model_id;
    return s;
}

std::string Signal::describe() const {
    std::ostringstream ss;
    ss.precision(6);
    ss << std::fixed << to_iso8601(as_of_) << " instrument#" << index_of(instrument_) << ' '
       << to_string(direction_) << " expected=" << expected_return_ << " net=" << net_edge_
       << " confidence=" << confidence_ << " model=" << model_id_;
    return ss.str();
}

}  // namespace ptl::signal
