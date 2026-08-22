#pragma once

/// \file calendar.hpp
/// Exchange trading calendar.
///
/// Sessions are RULE-GENERATED, not table-driven, with a small table of
/// exceptions for events no rule can predict (funerals, hurricanes). A hardcoded
/// list of dates is unmaintainable past a few years and silently stops being
/// correct the moment the range is extended; a rule set extends to any year and
/// states its own assumptions.
///
/// Everything the Calendar returns is a UTC instant. The exchange's local
/// schedule and its DST transitions are resolved during construction from an
/// explicit offset table, so the engine never performs a zone conversion.

#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "ptl/core/result.hpp"
#include "ptl/core/types.hpp"
#include "ptl/market/session.hpp"

namespace ptl::market {

/// Why a date deviates from the rules. Recorded so a report can explain an
/// unexpected gap rather than leaving a hole in the equity curve.
struct CalendarException {
    Timestamp date{kNoTimestamp};
    SessionKind kind{SessionKind::Closed};
    std::string reason;
};

struct CalendarSpec {
    std::string mic = "XNYS";

    /// Local-time schedule of the exchange, as offsets from local midnight.
    Duration regular_open{};
    Duration regular_close{};
    Duration half_day_close{};

    /// UTC offsets, in hours, for the exchange's standard and daylight periods.
    /// Explicit rather than derived: this is the one place the exchange's zone
    /// is encoded, and encoding it as data means no tzdb lookup at runtime.
    int standard_utc_offset_hours = -5;  // EST
    int daylight_utc_offset_hours = -4;  // EDT

    /// Ad-hoc closures and early closes. Rules cannot predict these.
    std::vector<CalendarException> exceptions;
};

/// US equity market calendar (NYSE / Nasdaq share a schedule).
///
/// Construction generates every session in [first_year, last_year] eagerly. A
/// decade is a few thousand entries -- trivial memory, and it makes every
/// lookup a hash-free binary search over a sorted vector rather than an
/// on-demand computation whose cost would vary by call site.
class Calendar {
public:
    [[nodiscard]] static Result<Calendar> build(const CalendarSpec& spec, int first_year,
                                                int last_year);

    /// NYSE/Nasdaq defaults: 09:30-16:00 ET, 13:00 ET early close, with the
    /// known ad-hoc closures from 2016 onward.
    [[nodiscard]] static CalendarSpec us_equities_spec();

    /// The session for the UTC date containing `ts`, or nullopt when that date
    /// is outside the generated range. A closed date returns a Session with
    /// kind == Closed rather than nullopt: "the market was shut" and "I have no
    /// data for that date" are different answers and must not be conflated.
    [[nodiscard]] std::optional<Session> session_on(Timestamp ts) const noexcept;

    /// The session containing `ts`, i.e. one where open <= ts < close.
    [[nodiscard]] std::optional<Session> session_containing(Timestamp ts) const noexcept;

    [[nodiscard]] bool is_trading_day(Timestamp ts) const noexcept;
    [[nodiscard]] bool is_open_at(Timestamp ts) const noexcept;

    /// Next and previous sessions that are actually open, exclusive of `ts`.
    [[nodiscard]] std::optional<Session> next_session(Timestamp ts) const noexcept;
    [[nodiscard]] std::optional<Session> previous_session(Timestamp ts) const noexcept;

    /// Open sessions only, chronological.
    [[nodiscard]] std::span<const Session> sessions() const noexcept { return open_; }

    [[nodiscard]] Timestamp range_begin() const noexcept;
    [[nodiscard]] Timestamp range_end() const noexcept;
    [[nodiscard]] const std::string& mic() const noexcept { return mic_; }

private:
    Calendar() = default;

    std::string mic_;
    std::vector<Session> all_;   ///< every calendar date, including closed ones
    std::vector<Session> open_;  ///< trading days only
};

// --- rule primitives, exposed for testing -----------------------------------

/// Gregorian Easter (Anonymous / Meeus algorithm). Good Friday is Easter - 2.
/// Exposed because a wrong Easter silently removes a trading day every year,
/// and that deserves its own test rather than being buried in the generator.
[[nodiscard]] Timestamp easter_sunday(int y) noexcept;

/// nth weekday of a month, e.g. nth_weekday(2024, 1, Monday, 3) = MLK Day.
/// `n < 0` counts back from the end (-1 = last).
[[nodiscard]] Timestamp nth_weekday(int y, unsigned month, unsigned weekday_iso, int n) noexcept;

/// NYSE observation rule: a holiday falling on Saturday is observed the
/// preceding Friday, on Sunday the following Monday.
[[nodiscard]] Timestamp observed_holiday(Timestamp actual) noexcept;

/// US federal DST: second Sunday in March to first Sunday in November,
/// switching at 02:00 local. Post-2007 rule only -- the generator rejects years
/// before 2008 rather than silently applying the wrong rule.
[[nodiscard]] bool is_us_daylight_time(Timestamp utc_date, int y) noexcept;

}  // namespace ptl::market
