#pragma once

/// \file time.hpp
/// Point-in-time semantics: the timestamp chain every observation and order
/// carries from venue to fill.
///
/// This is the backbone of the project. Two timestamps are not enough -- they
/// cannot express market-data latency separately from decision latency, and
/// they cannot express label-interval overlap for purging. The chain below is
/// the fix, and validate_chain() is the single place its ordering is enforced.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "ptl/core/types.hpp"

namespace ptl {

// ---------------------------------------------------------------------------
// Stages
// ---------------------------------------------------------------------------

enum class Stage : std::uint8_t {
    ExchangeTime = 0,  ///< the venue stamped the event
    ReceiveTime,       ///< earliest instant a live system could have seen it
    FeatureEndTime,    ///< max information timestamp used to build the features
    DecisionTime,      ///< model ran; signal formed
    SubmittedTime,     ///< order left the strategy
    ArrivalTime,       ///< order reached the venue; fill price is sampled HERE
    FillTime,          ///< execution stamped
    AckTime,           ///< confirmation observed
    Count
};

inline constexpr std::size_t kStageCount = static_cast<std::size_t>(Stage::Count);

[[nodiscard]] std::string_view to_string(Stage s) noexcept;

// ---------------------------------------------------------------------------
// EventTime -- carried by every market event
// ---------------------------------------------------------------------------

/// Two timestamps, never one.
///
/// exchange_time is when the venue acted. receive_time is the earliest instant
/// a live system could have known about it: exchange_time plus modelled
/// market-data latency. A backtest that consumes an event at its exchange_time
/// is claiming zero feed latency. That is a legitimate choice, but it should be
/// a configured one rather than an accident of having only a single field to
/// put a timestamp in.
struct EventTime {
    Timestamp exchange_time{kNoTimestamp};
    Timestamp receive_time{kNoTimestamp};

    [[nodiscard]] constexpr Duration feed_latency() const noexcept {
        return receive_time - exchange_time;
    }

    [[nodiscard]] constexpr bool ok() const noexcept {
        return is_set(exchange_time) && is_set(receive_time) && receive_time >= exchange_time;
    }
};

// ---------------------------------------------------------------------------
// LifecycleTimes -- the full chain
// ---------------------------------------------------------------------------

/// Stages not yet reached hold kNoTimestamp and are skipped by validation, so
/// one type is usable at every point in the pipeline.
struct LifecycleTimes {
    Timestamp exchange_time{kNoTimestamp};
    Timestamp receive_time{kNoTimestamp};
    Timestamp feature_end_time{kNoTimestamp};
    Timestamp decision_time{kNoTimestamp};
    Timestamp submitted_time{kNoTimestamp};
    Timestamp arrival_time{kNoTimestamp};
    Timestamp fill_time{kNoTimestamp};
    Timestamp ack_time{kNoTimestamp};

    [[nodiscard]] Timestamp at(Stage s) const noexcept;
    void                    set(Stage s, Timestamp ts) noexcept;
};

enum class ChainRule : std::uint8_t {
    Monotonic,      ///< later stage must be >= earlier stage
    StrictlyAfter,  ///< later stage must be >  earlier stage
};

struct ChainViolation {
    Stage     earlier{Stage::ExchangeTime};
    Stage     later{Stage::ReceiveTime};
    Timestamp earlier_ts{kNoTimestamp};
    Timestamp later_ts{kNoTimestamp};
    ChainRule rule{ChainRule::Monotonic};

    [[nodiscard]] std::string describe() const;
};

/// Validate the chain over populated stages.
///
/// Adjacent populated stages must be non-decreasing. One pair is stricter:
///
///     arrival_time  >  decision_time      (StrictlyAfter)
///
/// That single strict inequality IS the no-same-bar-execution rule. A system
/// that decides on a bar close and fills at that same close produces
/// arrival_time == decision_time and fails here. Encoding it as a property of
/// the timestamps rather than as a convention inside the execution code means
/// it cannot be honoured in one path and forgotten in another.
///
/// \returns std::nullopt when the chain is well-ordered.
[[nodiscard]] std::optional<ChainViolation> validate_chain(const LifecycleTimes& t) noexcept;

/// Process-wide count of chain violations, for release builds where validation
/// is counted rather than asserted. A non-zero value at the end of a run
/// invalidates that run and is recorded in the manifest.
[[nodiscard]] std::uint64_t chain_violation_count() noexcept;
void                        record_chain_violation() noexcept;
void                        reset_chain_violation_count() noexcept;

// ---------------------------------------------------------------------------
// Label intervals
// ---------------------------------------------------------------------------

/// The four stamps a training observation carries so that purging can test
/// interval OVERLAP rather than compare endpoints.
///
/// When the label horizon exceeds the decision step, consecutive labels
/// overlap. A purge that only compares label_end_time against the test start
/// leaves contaminated rows in the training set. The full interval is required.
/// See docs/01-research-reconciliation.md rows D2 and V2.
struct ObservationInterval {
    Timestamp sample_start_time{kNoTimestamp};
    Timestamp feature_end_time{kNoTimestamp};
    Timestamp label_start_time{kNoTimestamp};
    Timestamp label_end_time{kNoTimestamp};

    [[nodiscard]] constexpr bool ok() const noexcept {
        return sample_start_time <= feature_end_time &&
               feature_end_time <= label_start_time && label_start_time < label_end_time;
    }

    /// Half-open overlap against [begin, end).
    [[nodiscard]] constexpr bool label_overlaps(Timestamp begin, Timestamp end) const noexcept {
        return label_start_time < end && begin < label_end_time;
    }
};

}  // namespace ptl
