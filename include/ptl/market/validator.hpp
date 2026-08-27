#pragma once

/// \file validator.hpp
/// Cross-record validation of a normalised event stream.
///
/// Distinct from the per-record invariants in Bar/Quote/Trade, which reject a
/// single malformed row at construction. This layer catches defects only
/// visible across records or against the calendar: gaps, duplicates, prices
/// that jump like an unadjusted split, bars outside a session.
///
/// The distinction matters because the remedies differ. A malformed row is a
/// parse error; a gap may be a genuine halt; an unexplained 40% jump is almost
/// always a missing corporate action, and treating it as a return would put a
/// fictional 40% day into the backtest.

#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "ptl/core/result.hpp"
#include "ptl/market/calendar.hpp"
#include "ptl/market/event.hpp"

namespace ptl::market {

enum class IssueCode : std::uint8_t {
    NonMonotonicTimestamp,
    DuplicateTimestamp,
    GapInSession,
    OutsideSession,
    UnknownSession,
    SuspiciousPriceJump,
    ZeroVolume,
    StaleQuote,
    CrossedQuote,
    TimeframeMismatch,
};

[[nodiscard]] std::string_view to_string(IssueCode c) noexcept;

enum class Severity : std::uint8_t {
    /// The data is wrong. Proceeding produces fiction.
    Fatal,
    /// Plausible but worth surfacing. Zero-volume minutes are real; a hundred
    /// consecutive ones mean the feed stopped.
    Warning,
};

struct ValidationIssue {
    IssueCode code{IssueCode::NonMonotonicTimestamp};
    Severity severity{Severity::Warning};
    Timestamp at{kNoTimestamp};
    InstrumentId instrument{kInvalidInstrument};
    std::string detail;

    [[nodiscard]] std::string describe() const;
};

struct ValidationStats {
    std::size_t events = 0;
    std::size_t bars = 0;
    std::size_t quotes = 0;
    std::size_t trades = 0;
    std::size_t sessions = 0;
    std::size_t zero_volume_bars = 0;
    Timestamp first{kNoTimestamp};
    Timestamp last{kNoTimestamp};
};

struct ValidationReport {
    std::vector<ValidationIssue> issues;
    ValidationStats stats;

    [[nodiscard]] bool ok() const noexcept { return fatal_count() == 0; }
    [[nodiscard]] std::size_t fatal_count() const noexcept;
    [[nodiscard]] std::size_t warning_count() const noexcept;
    [[nodiscard]] std::string summary() const;
};

struct ValidatorConfig {
    Duration expected_bar_timeframe{std::chrono::minutes{1}};

    /// A single-interval log return beyond this is treated as an unadjusted
    /// corporate action rather than a real move. Liquid ETFs do not move 20%
    /// in a minute; if one appears, a split is missing.
    double max_abs_log_return = 0.20;

    /// Require every expected bar in a session to be present.
    bool require_complete_sessions = true;

    /// Consecutive zero-volume bars before it stops being a quiet minute and
    /// starts being a dead feed.
    std::size_t max_consecutive_zero_volume = 30;

    /// Promote every warning to fatal. Used by ingest, where a surprise should
    /// stop the pipeline rather than scroll past.
    bool strict = false;
};

class DataValidator {
public:
    explicit DataValidator(ValidatorConfig cfg = {}) : cfg_(cfg) {}

    /// `calendar` may be null, in which case session checks are skipped and
    /// that fact is recorded rather than silently assumed.
    [[nodiscard]] ValidationReport validate(std::span<const MarketEvent> events,
                                            const Calendar* calendar) const;

private:
    ValidatorConfig cfg_;
};

}  // namespace ptl::market
