#pragma once

/// \file signal.hpp
/// A trading intention, with its full provenance.
///
/// A Signal is an immutable value carrying not just "buy" but WHY, HOW
/// CONFIDENT, OVER WHAT HORIZON and FROM WHICH MODEL. That provenance is what
/// makes a later post-mortem possible: without the model id and horizon, a
/// disappointing month cannot be attributed to a specific model or to a horizon
/// mismatch, and the investigation has nowhere to start.
///
/// `as_of` is the decision instant. Nothing downstream may use information
/// stamped later, and the cost-aware net edge is computed at construction so a
/// signal whose expected move cannot pay for its own round trip never becomes
/// an order.

#include <cstdint>
#include <string>
#include <string_view>

#include "ptl/core/result.hpp"
#include "ptl/core/time.hpp"
#include "ptl/core/types.hpp"

namespace ptl::signal {

enum class Direction : std::int8_t { Short = -1, Flat = 0, Long = 1 };

[[nodiscard]] std::string_view to_string(Direction) noexcept;

[[nodiscard]] constexpr int sign_of(Direction d) noexcept {
    return static_cast<int>(d);
}

/// The costs a signal must clear before it is worth acting on.
///
/// Every component is a COST in the same units as the expected return, so the
/// net edge is one subtraction rather than a chain of special cases.
struct CostEstimate {
    double half_spread = 0.0;
    double commission = 0.0;
    double slippage = 0.0;
    double borrow = 0.0;
    /// Penalty proportional to the position change. Discourages churning
    /// between two nearly identical targets, which costs real money and
    /// produces no expected return.
    double turnover_penalty = 0.0;

    [[nodiscard]] constexpr double total() const noexcept {
        return half_spread + commission + slippage + borrow + turnover_penalty;
    }
    [[nodiscard]] std::string describe() const;
};

class Signal {
public:
    /// The only way to build one. Rejects anything incoherent and computes the
    /// net edge, so an unprofitable signal cannot exist as a valid object.
    [[nodiscard]] static Result<Signal> create(Timestamp as_of, InstrumentId instrument,
                                               Direction direction, double expected_return,
                                               double confidence, Duration horizon,
                                               std::uint64_t model_id, CostEstimate costs = {});

    /// A deliberate no-position signal. Distinct from "no signal at all":
    /// flat means the model spoke and said stay out, which a filter counting
    /// coverage needs to distinguish from silence.
    [[nodiscard]] static Signal flat(Timestamp as_of, InstrumentId instrument,
                                     std::uint64_t model_id);

    [[nodiscard]] Timestamp as_of() const noexcept { return as_of_; }
    [[nodiscard]] InstrumentId instrument() const noexcept { return instrument_; }
    [[nodiscard]] Direction direction() const noexcept { return direction_; }

    /// Expected return over the horizon, GROSS of costs, signed by direction.
    [[nodiscard]] double expected_return() const noexcept { return expected_return_; }

    /// Expected return after every modelled cost. THE NUMBER THAT MATTERS: a
    /// signal with positive gross and negative net edge is a losing trade
    /// wearing an attractive label.
    [[nodiscard]] double net_edge() const noexcept { return net_edge_; }

    /// In [0, 1]. For a classifier this is calibrated probability; for a
    /// regressor it is a monotone transform of predicted magnitude, and the
    /// two are NOT interchangeable -- which is why the model id travels along.
    [[nodiscard]] double confidence() const noexcept { return confidence_; }

    [[nodiscard]] Duration horizon() const noexcept { return horizon_; }
    [[nodiscard]] std::uint64_t model_id() const noexcept { return model_id_; }
    [[nodiscard]] const CostEstimate& costs() const noexcept { return costs_; }

    [[nodiscard]] bool is_flat() const noexcept { return direction_ == Direction::Flat; }

    /// True when acting on this signal is expected to make money after costs.
    [[nodiscard]] bool is_actionable() const noexcept {
        return direction_ != Direction::Flat && net_edge_ > 0.0;
    }

    /// The instant this signal stops being meaningful.
    [[nodiscard]] Timestamp expires_at() const noexcept { return as_of_ + horizon_; }

    [[nodiscard]] std::string describe() const;

private:
    Signal() = default;

    Timestamp as_of_{kNoTimestamp};
    InstrumentId instrument_{kInvalidInstrument};
    Direction direction_{Direction::Flat};
    double expected_return_ = 0.0;
    double net_edge_ = 0.0;
    double confidence_ = 0.0;
    Duration horizon_{};
    std::uint64_t model_id_ = 0;
    CostEstimate costs_{};
};

}  // namespace ptl::signal
