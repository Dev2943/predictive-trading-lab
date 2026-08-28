#pragma once

/// \file execution_types.hpp
/// Vocabulary for execution algorithms.
///
/// An execution algorithm decides HOW a parent order is worked: over what
/// schedule, in what clips, at what prices. It never fills anything. The only
/// thing an algorithm produces is a ChildOrder, and every child travels the
/// same road as any other order -- risk, OMS, broker -- so an algorithm cannot
/// create exposure the risk gate has not seen.
///
/// DETERMINISM CONTRACT (ADR-0004): a schedule is a pure function of the parent
/// order, the simulated clock, and market state already observed. No algorithm
/// reads a wall clock, and none carries state that a replay would not
/// reconstruct identically.

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "ptl/core/result.hpp"
#include "ptl/core/time.hpp"
#include "ptl/core/types.hpp"
#include "ptl/execution/quote_router.hpp"
#include "ptl/oms/fill.hpp"
#include "ptl/oms/order.hpp"

namespace ptl::algo {

enum class AlgoKind : std::uint8_t {
    Immediate,
    Twap,
    Vwap,
    Participation,
    Iceberg,
    AdaptiveLimit,
};

[[nodiscard]] std::string_view to_string(AlgoKind) noexcept;

/// Where an execution stands.
enum class ExecutionState : std::uint8_t {
    Pending,    ///< accepted, no slice released yet
    Working,    ///< slices in flight
    Completed,  ///< parent quantity fully filled
    Cancelled,  ///< stopped before completion
    Expired,    ///< the execution window closed with quantity outstanding
    Failed,     ///< an unrecoverable error
};

[[nodiscard]] std::string_view to_string(ExecutionState) noexcept;
[[nodiscard]] bool is_terminal(ExecutionState) noexcept;

/// A slice the algorithm wants to send.
///
/// Deliberately NOT an oms::Order. A ChildOrder is an intention; converting it
/// into an order requires an id from the OMS and a decision time from the
/// clock, and keeping those out of the algorithm is what stops an algorithm
/// minting orders on its own.
struct ChildOrder {
    InstrumentId instrument{kInvalidInstrument};
    Side side{Side::Buy};
    Qty quantity{};
    oms::OrderType type{oms::OrderType::Market};
    /// Engaged for Limit and StopLimit only (ADR-0002: absence is absence).
    std::optional<Price> limit_price{};
    std::optional<Price> stop_price{};
    oms::TimeInForce time_in_force{oms::TimeInForce::Day};
    /// Slice index within the parent's schedule, for diagnostics.
    std::size_t slice_index = 0;
    /// The quantity NOT displayed, for an iceberg. Zero elsewhere.
    Qty hidden_quantity{};

    [[nodiscard]] bool valid() const noexcept;
};

/// Everything an algorithm may see at one instant.
///
/// A snapshot, not a series. There is no history accessor and no way to reach
/// the event source, so an algorithm cannot consult a future quote even by
/// mistake -- the same structural defence the feature engine uses.
struct ExecutionContext {
    Timestamp now{kNoTimestamp};
    /// Market state as routed in Phase 8, carrying its own executability.
    execution::RoutedMarket market;
    /// Volume observed in the current interval.
    Volume interval_volume{};
    /// Cumulative volume observed since the execution began, for POV.
    Volume cumulative_volume{};
    /// Expected share of the session's volume in this interval, from a
    /// historical profile. Used by VWAP.
    double expected_volume_share = 0.0;
    /// Session boundaries, so an algorithm can respect market hours without
    /// consulting a calendar itself.
    Timestamp session_open{kNoTimestamp};
    Timestamp session_close{kNoTimestamp};
};

/// How much of the parent is done.
struct ExecutionProgress {
    Qty filled{};
    Qty remaining{};
    Notional filled_notional{};
    Notional costs{};
    std::size_t slices_released = 0;
    std::size_t slices_filled = 0;

    [[nodiscard]] double completion_ratio(Qty parent_quantity) const noexcept;
    [[nodiscard]] std::optional<Price> average_price() const noexcept;
    [[nodiscard]] bool complete(Qty parent_quantity) const noexcept;
};

/// Constraints every algorithm must respect.
///
/// One policy object rather than per-algorithm parameters, because the
/// CONSTRAINTS are identical across algorithms and duplicating them is how one
/// copy ends up missing a collar.
struct ExecutionPolicy {
    /// Cap on the share of interval volume any one slice may take.
    double max_participation_rate = 0.10;

    /// Smallest slice worth sending. Below this the commission floor makes the
    /// clip unprofitable regardless of the parent's edge.
    Qty min_clip_size{1.0};

    /// Largest quantity shown at once. Zero means show everything.
    Qty max_display_quantity{};

    /// Limit prices may not sit further than this from the reference. A collar
    /// is what stops a stale reference price producing an order at an absurd
    /// level.
    Bps price_collar{100.0};

    oms::TimeInForce time_in_force{oms::TimeInForce::Day};

    /// Refuse to release slices outside continuous trading. The open and close
    /// auctions have different microstructure, and an algorithm calibrated on
    /// continuous trading does not describe them.
    bool respect_market_hours = true;
    Duration open_buffer{std::chrono::minutes{1}};
    Duration close_buffer{std::chrono::minutes{1}};

    /// Round every clip to this lot.
    double lot_size = 1.0;

    /// Abandon the execution if the window closes with quantity outstanding,
    /// rather than dumping the remainder at market. Dumping converts a patient
    /// execution into the worst possible one at the worst possible moment.
    bool expire_rather_than_dump = true;

    [[nodiscard]] std::string signature() const;
    [[nodiscard]] std::uint64_t hash() const;
};

/// The outcome of one execution.
struct ExecutionResult {
    ExecutionState state{ExecutionState::Pending};
    ExecutionProgress progress;
    Timestamp started{kNoTimestamp};
    Timestamp finished{kNoTimestamp};
    /// Benchmark price the execution is measured against: the reference at the
    /// instant the parent arrived.
    Price arrival_price{};
    std::string detail;

    /// Implementation shortfall in basis points, signed so POSITIVE IS COST for
    /// either side. A buy filling above arrival and a sell filling below both
    /// hurt, and one convention spares every aggregate a special case.
    [[nodiscard]] Bps shortfall_bps(Side) const noexcept;
    [[nodiscard]] Duration duration() const noexcept;
    [[nodiscard]] std::string describe() const;
};

/// Aggregate statistics across executions.
struct ExecutionStatistics {
    std::size_t executions_started = 0;
    std::size_t executions_completed = 0;
    std::size_t executions_expired = 0;
    std::size_t executions_cancelled = 0;
    std::size_t child_orders_emitted = 0;
    std::size_t child_orders_rejected = 0;
    std::size_t slices_skipped_participation = 0;
    std::size_t slices_skipped_min_clip = 0;
    std::size_t slices_skipped_market_closed = 0;
    std::size_t slices_skipped_not_executable = 0;
    std::size_t limit_repriced = 0;

    [[nodiscard]] std::string describe() const;
};

}  // namespace ptl::algo
