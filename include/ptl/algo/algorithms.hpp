#pragma once

/// \file algorithms.hpp
/// The execution algorithm interface and its implementations.
///
/// ONE INTERFACE, SIX ALGORITHMS, NO SWITCH. Every algorithm derives from
/// IExecutionAlgorithm and is selected by holding a different pointer. There is
/// no runtime kind check anywhere in the driver: adding a seventh algorithm
/// means adding a class, not editing a dispatch table that some other
/// translation unit also switches over.
///
/// An algorithm NEVER fills anything. Its only output is a ChildOrder, and the
/// driver routes every child through risk, the OMS and the broker.

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "ptl/algo/schedule.hpp"
#include "ptl/core/result.hpp"

namespace ptl::algo {

/// The parent order being worked, plus its execution window.
struct ExecutionRequest {
    oms::Order parent;
    /// Half-open [begin, end). Slices are released inside it and nowhere else.
    Timestamp window_begin{kNoTimestamp};
    Timestamp window_end{kNoTimestamp};
    ExecutionPolicy policy;
    /// Number of time slices for TWAP; ignored by algorithms that derive their
    /// own cadence.
    std::size_t slice_count = 10;
    /// Expected volume profile for VWAP. Empty means no profile is available,
    /// and VWAP will refuse rather than silently degenerating to TWAP.
    std::vector<double> volume_profile;

    [[nodiscard]] Result<bool> validate() const;
};

class IExecutionAlgorithm {
public:
    IExecutionAlgorithm() = default;
    virtual ~IExecutionAlgorithm() = default;
    IExecutionAlgorithm(const IExecutionAlgorithm&) = delete;
    IExecutionAlgorithm& operator=(const IExecutionAlgorithm&) = delete;

protected:
    IExecutionAlgorithm(IExecutionAlgorithm&&) = default;
    IExecutionAlgorithm& operator=(IExecutionAlgorithm&&) = default;

public:
    [[nodiscard]] virtual std::string_view name() const noexcept = 0;
    [[nodiscard]] virtual AlgoKind kind() const noexcept = 0;

    /// Compute the schedule for a request. Called once, before any child is
    /// sent, so the plan can be inspected up front rather than reconstructed
    /// from fills afterwards.
    [[nodiscard]] virtual Result<ExecutionSchedule> plan(const ExecutionRequest&) const = 0;

    /// Decide what to send at this instant.
    ///
    /// \returns nullopt when nothing should be sent now -- which is a normal
    ///          and frequent answer, not an error. Being behind schedule is the
    ///          only reason to send.
    [[nodiscard]] virtual std::optional<ChildOrder> next_child(const ExecutionRequest&,
                                                               const ExecutionSchedule&,
                                                               const ExecutionProgress&,
                                                               const ExecutionContext&) const = 0;

    /// A fresh, unconfigured instance. The driver uses this so each execution
    /// starts clean rather than inheriting the previous one's state.
    [[nodiscard]] virtual std::unique_ptr<IExecutionAlgorithm> clone() const = 0;
};

/// Shared slice sizing, policy application and price selection.
///
/// A base class rather than six copies. The CONSTRAINTS are identical across
/// algorithms -- participation caps, minimum clips, collars, market hours --
/// and duplicating them is how one copy ends up missing a cap.
class AlgorithmBase : public IExecutionAlgorithm {
public:
    /// Quantity to send now: the shortfall against the schedule, then clamped
    /// by every policy constraint. Exposed as a static so it can be tested
    /// directly rather than only through a full execution.
    [[nodiscard]] static Qty clip_quantity(const ExecutionRequest&, const ExecutionSchedule&,
                                           const ExecutionProgress&, const ExecutionContext&,
                                           ExecutionStatistics* stats = nullptr);

    /// Apply every policy constraint to a desired quantity.
    ///
    /// Shared by the schedule-driven algorithms and by those that derive their
    /// own quantity (POV, Iceberg, Immediate). Separated from clip_quantity()
    /// because WHAT to send differs by algorithm while HOW MUCH is permitted
    /// does not -- and duplicating the caps is how one copy ends up missing one.
    [[nodiscard]] static Qty apply_policy_caps(const ExecutionRequest&, const ExecutionContext&,
                                               double desired, Qty remaining,
                                               ExecutionStatistics* stats = nullptr);

    /// Whether a slice may be released at this instant.
    [[nodiscard]] static bool releasable(const ExecutionRequest&, const ExecutionContext&,
                                         ExecutionStatistics* stats = nullptr);

    /// A limit price offset from the touch, clamped by the collar.
    [[nodiscard]] static std::optional<Price> collared_limit(const ExecutionRequest&,
                                                             const ExecutionContext&, Bps offset);
};

/// Everything at once, as a marketable order.
class ImmediateAlgorithm final : public AlgorithmBase {
public:
    [[nodiscard]] std::string_view name() const noexcept override { return "immediate"; }
    [[nodiscard]] AlgoKind kind() const noexcept override { return AlgoKind::Immediate; }
    [[nodiscard]] Result<ExecutionSchedule> plan(const ExecutionRequest&) const override;
    [[nodiscard]] std::optional<ChildOrder> next_child(const ExecutionRequest&,
                                                       const ExecutionSchedule&,
                                                       const ExecutionProgress&,
                                                       const ExecutionContext&) const override;
    [[nodiscard]] std::unique_ptr<IExecutionAlgorithm> clone() const override;
};

/// Equal quantity per equal time slice.
class TwapAlgorithm final : public AlgorithmBase {
public:
    [[nodiscard]] std::string_view name() const noexcept override { return "twap"; }
    [[nodiscard]] AlgoKind kind() const noexcept override { return AlgoKind::Twap; }
    [[nodiscard]] Result<ExecutionSchedule> plan(const ExecutionRequest&) const override;
    [[nodiscard]] std::optional<ChildOrder> next_child(const ExecutionRequest&,
                                                       const ExecutionSchedule&,
                                                       const ExecutionProgress&,
                                                       const ExecutionContext&) const override;
    [[nodiscard]] std::unique_ptr<IExecutionAlgorithm> clone() const override;
};

/// Quantity proportional to a historical volume profile.
class VwapAlgorithm final : public AlgorithmBase {
public:
    [[nodiscard]] std::string_view name() const noexcept override { return "vwap"; }
    [[nodiscard]] AlgoKind kind() const noexcept override { return AlgoKind::Vwap; }
    [[nodiscard]] Result<ExecutionSchedule> plan(const ExecutionRequest&) const override;
    [[nodiscard]] std::optional<ChildOrder> next_child(const ExecutionRequest&,
                                                       const ExecutionSchedule&,
                                                       const ExecutionProgress&,
                                                       const ExecutionContext&) const override;
    [[nodiscard]] std::unique_ptr<IExecutionAlgorithm> clone() const override;
};

/// A fixed share of OBSERVED market volume.
///
/// Distinct from VWAP: VWAP follows an expected profile computed in advance,
/// whereas POV reacts to what is actually trading. POV therefore has no
/// meaningful schedule -- its pace is set by the market, not the clock -- and
/// its plan() returns a single-slice window used only to bound the execution.
class ParticipationAlgorithm final : public AlgorithmBase {
public:
    explicit ParticipationAlgorithm(double target_rate = 0.10) : target_rate_(target_rate) {}

    [[nodiscard]] std::string_view name() const noexcept override { return "pov"; }
    [[nodiscard]] AlgoKind kind() const noexcept override { return AlgoKind::Participation; }
    [[nodiscard]] Result<ExecutionSchedule> plan(const ExecutionRequest&) const override;
    [[nodiscard]] std::optional<ChildOrder> next_child(const ExecutionRequest&,
                                                       const ExecutionSchedule&,
                                                       const ExecutionProgress&,
                                                       const ExecutionContext&) const override;
    [[nodiscard]] std::unique_ptr<IExecutionAlgorithm> clone() const override;

    [[nodiscard]] double target_rate() const noexcept { return target_rate_; }

private:
    double target_rate_;
};

/// Shows a small clip, refreshing as it fills.
///
/// ⚠ THIS IS NOT QUEUE MODELLING (ADR-0003). An iceberg here refreshes its
/// displayed quantity when the previous clip completes; it makes no claim about
/// where in a queue the refreshed clip lands, because sampled top-of-book data
/// cannot establish that.
class IcebergAlgorithm final : public AlgorithmBase {
public:
    explicit IcebergAlgorithm(Qty display_size = Qty{100.0}) : display_size_(display_size) {}

    [[nodiscard]] std::string_view name() const noexcept override { return "iceberg"; }
    [[nodiscard]] AlgoKind kind() const noexcept override { return AlgoKind::Iceberg; }
    [[nodiscard]] Result<ExecutionSchedule> plan(const ExecutionRequest&) const override;
    [[nodiscard]] std::optional<ChildOrder> next_child(const ExecutionRequest&,
                                                       const ExecutionSchedule&,
                                                       const ExecutionProgress&,
                                                       const ExecutionContext&) const override;
    [[nodiscard]] std::unique_ptr<IExecutionAlgorithm> clone() const override;

    [[nodiscard]] Qty display_size() const noexcept { return display_size_; }

private:
    Qty display_size_;
};

/// Rests at a limit and reprices as the quote moves.
///
/// Passive until it falls behind, then crosses. The aggression schedule is a
/// deliberate policy: an adaptive algorithm that never crosses will simply fail
/// to complete in a trending market, which is worse than paying the spread.
class AdaptiveLimitAlgorithm final : public AlgorithmBase {
public:
    struct Config {
        /// Passive offset from the touch, in bps, while on schedule.
        Bps passive_offset{5.0};
        /// Completion shortfall beyond which the algorithm crosses the spread.
        double urgency_threshold = 0.25;
        std::size_t slice_count = 10;
    };

    /// Two constructors rather than a defaulted aggregate argument: the
    /// default-argument form requires Config to be copy-list-initializable
    /// from {}, which the strong-typed Bps member does not permit.
    AdaptiveLimitAlgorithm() = default;
    explicit AdaptiveLimitAlgorithm(Config cfg) : cfg_(cfg) {}

    [[nodiscard]] std::string_view name() const noexcept override { return "adaptive_limit"; }
    [[nodiscard]] AlgoKind kind() const noexcept override { return AlgoKind::AdaptiveLimit; }
    [[nodiscard]] Result<ExecutionSchedule> plan(const ExecutionRequest&) const override;
    [[nodiscard]] std::optional<ChildOrder> next_child(const ExecutionRequest&,
                                                       const ExecutionSchedule&,
                                                       const ExecutionProgress&,
                                                       const ExecutionContext&) const override;
    [[nodiscard]] std::unique_ptr<IExecutionAlgorithm> clone() const override;

private:
    Config cfg_{};
};

/// Name-to-algorithm registry.
///
/// Explicit, like the strategy registry: no self-registering globals, because
/// static initialisation order is unspecified and a registry that fills itself
/// during static init makes the available set depend on link order.
class AlgorithmRegistry {
public:
    [[nodiscard]] Result<bool> register_algorithm(std::string name,
                                                  std::unique_ptr<IExecutionAlgorithm>);
    [[nodiscard]] Result<std::unique_ptr<IExecutionAlgorithm>> create(std::string_view name) const;
    [[nodiscard]] bool contains(std::string_view) const noexcept;
    [[nodiscard]] std::vector<std::string_view> names() const;
    [[nodiscard]] std::size_t size() const noexcept { return algorithms_.size(); }

    /// Registers all six built-ins.
    [[nodiscard]] static Result<AlgorithmRegistry> with_defaults();

private:
    std::map<std::string, std::unique_ptr<IExecutionAlgorithm>, std::less<>> algorithms_;
};

}  // namespace ptl::algo
