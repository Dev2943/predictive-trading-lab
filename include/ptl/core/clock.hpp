#pragma once

/// \file clock.hpp
/// The seam that lets one Engine serve both backtesting and paper trading.
///
/// Strategy code never calls std::chrono::system_clock::now(). It asks the
/// StrategyContext, which asks an IClock. In a backtest that clock is driven
/// by event timestamps; in paper trading it is the wall clock. Nothing else
/// about the strategy changes.

#include "ptl/core/types.hpp"

namespace ptl {

class IClock {
public:
    IClock() = default;
    virtual ~IClock() = default;
    IClock(const IClock&) = delete;
    IClock& operator=(const IClock&) = delete;
    IClock(IClock&&) = delete;
    IClock& operator=(IClock&&) = delete;

    [[nodiscard]] virtual Timestamp now() const noexcept = 0;
};

/// Advanced explicitly by the Engine as it drains the event queue.
/// Monotonicity is enforced: an out-of-order event feed is a data bug, and
/// silently accepting it would let a backtest consume information out of
/// sequence. Better to fail loudly at ingest time.
class SimulatedClock final : public IClock {
public:
    explicit SimulatedClock(Timestamp start = Timestamp{}) noexcept : now_(start) {}

    [[nodiscard]] Timestamp now() const noexcept override { return now_; }

    /// \throws std::logic_error if `ts` precedes the current time.
    void advance_to(Timestamp ts);

    void advance_by(Duration d) { now_ += d; }

    /// Only for test setup and for resetting between walk-forward folds.
    void reset(Timestamp ts) noexcept { now_ = ts; }

private:
    Timestamp now_;
};

class WallClock final : public IClock {
public:
    [[nodiscard]] Timestamp now() const noexcept override;
};

}  // namespace ptl
