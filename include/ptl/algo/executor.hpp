#pragma once

/// \file executor.hpp
/// Drives execution algorithms and routes their children to the venue.
///
/// THE NO-BYPASS GUARANTEE. The executor holds an engine::OrderSink and nothing
/// else. It has no broker reference, no OMS reference, and cannot construct a
/// Fill. Every child order it emits travels risk -> OMS -> broker exactly as a
/// strategy's own order does, so an algorithm cannot create exposure the risk
/// gate has not seen.
///
/// The executor owns no clock either: `on_market` is called by the engine with
/// the current instant. That is what keeps execution scheduling deterministic
/// under replay (ADR-0004).

#include <map>
#include <memory>
#include <optional>
#include <vector>

#include "ptl/algo/algorithms.hpp"
#include "ptl/core/result.hpp"
#include "ptl/engine/context.hpp"

namespace ptl::algo {

/// One live execution: the request, its plan, and its progress.
struct ExecutionPlan {
    oms::OrderId parent_id{oms::kNoOrder};
    /// std::optional, NOT an ExecutionRequest by value. The request holds an
    /// oms::Order, whose default constructor is private on purpose so that
    /// every order goes through a validating factory. Holding it by value
    /// would make ExecutionPlan non-default-constructible; the fix is to
    /// change the CONTAINER, never to relax the invariant.
    std::optional<ExecutionRequest> request;
    ExecutionSchedule schedule;
    ExecutionProgress progress;
    ExecutionState state{ExecutionState::Pending};
    Timestamp started{kNoTimestamp};
    Timestamp finished{kNoTimestamp};
    Price arrival_price{};
    /// Child order ids belonging to this parent, in emission order.
    std::vector<oms::OrderId> children;
    /// The child currently resting, for cancel/replace.
    std::optional<oms::OrderId> working_child;

    [[nodiscard]] ExecutionResult result() const;
};

class Executor {
public:
    /// \param algorithm cloned on submit, so each execution starts clean rather
    ///        than inheriting the previous one's state.
    explicit Executor(std::unique_ptr<IExecutionAlgorithm> algorithm);

    /// Begin working a parent order.
    ///
    /// The parent is NOT sent to the venue. It is a statement of intent that
    /// the algorithm decomposes; only children reach the market.
    [[nodiscard]] Result<bool> submit(const ExecutionRequest&, oms::OrderId parent_id,
                                      Price arrival_price);

    /// Advance every live execution for this instrument and emit any children
    /// due. Called by the engine on each market event.
    [[nodiscard]] Result<std::size_t> on_market(InstrumentId, const ExecutionContext&,
                                                engine::OrderSink&);

    /// Attribute a fill to its parent execution.
    ///
    /// Fills arrive by CHILD id; the mapping back to the parent is what lets
    /// progress be tracked without the algorithm ever seeing a Fill.
    [[nodiscard]] Result<bool> on_fill(const oms::Fill&);

    /// Cancel an execution and any resting child.
    [[nodiscard]] Result<bool> cancel(oms::OrderId parent_id, engine::OrderSink&,
                                      std::string reason = {});

    /// Expire executions whose window has closed.
    ///
    /// \returns the number expired. With `expire_rather_than_dump` set, the
    ///          outstanding quantity is abandoned rather than sent at market:
    ///          dumping converts a patient execution into the worst possible
    ///          one at the worst possible moment.
    [[nodiscard]] std::size_t expire_stale(Timestamp now, engine::OrderSink&);

    [[nodiscard]] const ExecutionPlan* find(oms::OrderId parent_id) const noexcept;
    [[nodiscard]] std::vector<oms::OrderId> active() const;
    [[nodiscard]] const ExecutionStatistics& stats() const noexcept { return stats_; }
    [[nodiscard]] std::size_t size() const noexcept { return plans_.size(); }
    [[nodiscard]] std::string_view algorithm_name() const noexcept { return algorithm_->name(); }

    /// Hash over every child order emitted. Two identical runs must match.
    [[nodiscard]] std::uint64_t content_hash() const noexcept;

    void reset() noexcept;

private:
    [[nodiscard]] Result<oms::OrderId> emit_child(ExecutionPlan&, const ChildOrder&, Timestamp now,
                                                  engine::OrderSink&);

    std::unique_ptr<IExecutionAlgorithm> algorithm_;
    // std::map throughout: iteration order is part of the determinism contract.
    std::map<std::uint64_t, ExecutionPlan> plans_;
    /// child id -> parent id, for fill attribution.
    std::map<std::uint64_t, std::uint64_t> child_to_parent_;
    ExecutionStatistics stats_;
    /// Accumulated over every child emitted, for the determinism hash.
    std::uint64_t emitted_hash_ = 0xcbf29ce484222325ULL;
};

}  // namespace ptl::algo
