#include "ptl/market/calendar.hpp"

#include <algorithm>
#include <chrono>

#include "ptl/core/time.hpp"

namespace ptl::market {
namespace {

using namespace std::chrono;

constexpr unsigned kMonday = 1;
constexpr unsigned kThursday = 4;
constexpr unsigned kFriday = 5;
constexpr unsigned kSaturday = 6;
constexpr unsigned kSunday = 7;

[[nodiscard]] Timestamp from_ymd(int y, unsigned m, unsigned d) noexcept {
    const sys_days sd{year{y} / month{m} / day{d}};
    return Timestamp{duration_cast<nanoseconds>(sd.time_since_epoch())};
}

[[nodiscard]] unsigned iso_weekday(Timestamp ts) noexcept {
    const weekday wd{floor<days>(ts)};
    return wd.iso_encoding();
}

[[nodiscard]] int year_of(Timestamp ts) noexcept {
    return static_cast<int>(year_month_day{floor<days>(ts)}.year());
}

}  // namespace

std::string_view to_string(SessionKind k) noexcept {
    switch (k) {
        case SessionKind::Closed:
            return "closed";
        case SessionKind::Regular:
            return "regular";
        case SessionKind::HalfDay:
            return "half_day";
    }
    return "unknown";
}

// ---------------------------------------------------------------------------
// Rule primitives
// ---------------------------------------------------------------------------

Timestamp easter_sunday(int y) noexcept {
    // Anonymous Gregorian algorithm (Meeus/Jones/Butcher). Integer arithmetic
    // only, valid for the whole Gregorian calendar.
    const int a = y % 19;
    const int b = y / 100;
    const int c = y % 100;
    const int d = b / 4;
    const int e = b % 4;
    const int f = (b + 8) / 25;
    const int g = (b - f + 1) / 3;
    const int h = (19 * a + b - d - g + 15) % 30;
    const int i = c / 4;
    const int k = c % 4;
    const int l = (32 + 2 * e + 2 * i - h - k) % 7;
    const int m = (a + 11 * h + 22 * l) / 451;
    const int month_ = (h + l - 7 * m + 114) / 31;
    const int day_ = ((h + l - 7 * m + 114) % 31) + 1;
    return from_ymd(y, static_cast<unsigned>(month_), static_cast<unsigned>(day_));
}

Timestamp nth_weekday(int y, unsigned month_, unsigned weekday_iso, int n) noexcept {
    if (n > 0) {
        const year_month_day first{std::chrono::year{y} / month{month_} / day{1}};
        const unsigned first_wd = weekday{sys_days{first}}.iso_encoding();
        unsigned offset = (weekday_iso + 7 - first_wd) % 7;
        offset += static_cast<unsigned>(n - 1) * 7;
        return from_ymd(y, month_, 1 + offset);
    }
    // Count back from the last day of the month.
    const year_month_day_last last{std::chrono::year{y} / month{month_} / std::chrono::last};
    const unsigned last_day = static_cast<unsigned>(last.day());
    const unsigned last_wd = weekday{sys_days{last}}.iso_encoding();
    unsigned back = (last_wd + 7 - weekday_iso) % 7;
    back += static_cast<unsigned>(-n - 1) * 7;
    return from_ymd(y, month_, last_day - back);
}

Timestamp observed_holiday(Timestamp actual) noexcept {
    const unsigned wd = iso_weekday(actual);
    if (wd == kSaturday) return actual - days{1};
    if (wd == kSunday) return actual + days{1};
    return actual;
}

bool is_us_daylight_time(Timestamp utc_date, int y) noexcept {
    // Second Sunday in March through first Sunday in November. The transition
    // happens at 02:00 local, but the calendar only ever asks about whole
    // exchange DATES, and no US market session straddles the switch: the
    // changeover is on a Sunday. Comparing dates is therefore exact, not an
    // approximation.
    const Timestamp dst_start = nth_weekday(y, 3, kSunday, 2);
    const Timestamp dst_end = nth_weekday(y, 11, kSunday, 1);
    return utc_date >= dst_start && utc_date < dst_end;
}

// ---------------------------------------------------------------------------
// Holiday generation
// ---------------------------------------------------------------------------

namespace {

/// Full-closure holidays for a year, as observed dates.
[[nodiscard]] std::vector<Timestamp> full_holidays(int y) {
    std::vector<Timestamp> h;
    h.push_back(observed_holiday(from_ymd(y, 1, 1)));  // New Year
    h.push_back(nth_weekday(y, 1, kMonday, 3));        // MLK
    h.push_back(nth_weekday(y, 2, kMonday, 3));        // Presidents
    h.push_back(easter_sunday(y) - days{2});           // Good Friday
    h.push_back(nth_weekday(y, 5, kMonday, -1));       // Memorial
    if (y >= 2022) {
        h.push_back(observed_holiday(from_ymd(y, 6, 19)));  // Juneteenth
    }
    h.push_back(observed_holiday(from_ymd(y, 7, 4)));    // Independence
    h.push_back(nth_weekday(y, 9, kMonday, 1));          // Labor
    h.push_back(nth_weekday(y, 11, kThursday, 4));       // Thanksgiving
    h.push_back(observed_holiday(from_ymd(y, 12, 25)));  // Christmas

    // A New Year holiday for year Y+1 observed on Dec 31 of year Y (Jan 1
    // falling on a Saturday) belongs to THIS year's calendar. Missing it leaves
    // a phantom trading day on the last session of the year.
    const Timestamp next_ny = observed_holiday(from_ymd(y + 1, 1, 1));
    if (year_of(next_ny) == y) h.push_back(next_ny);
    return h;
}

/// Scheduled early closes (13:00 ET).
[[nodiscard]] std::vector<Timestamp> half_days(int y, const std::vector<Timestamp>& closed) {
    const auto is_closed = [&closed](Timestamp t) {
        return std::find(closed.begin(), closed.end(), t) != closed.end();
    };
    std::vector<Timestamp> h;

    // July 3, when it is a weekday and Independence Day itself is a weekday.
    // If July 4 falls at a weekend the observed holiday absorbs the early
    // close, so there is no half day.
    const Timestamp jul3 = from_ymd(y, 7, 3);
    const Timestamp jul4 = from_ymd(y, 7, 4);
    const unsigned jul3_wd = iso_weekday(jul3);
    const unsigned jul4_wd = iso_weekday(jul4);
    if (jul3_wd <= kFriday && jul4_wd <= kFriday && !is_closed(jul3)) h.push_back(jul3);

    // The Friday after Thanksgiving, always.
    h.push_back(nth_weekday(y, 11, kThursday, 4) + days{1});

    // Christmas Eve, when it is a weekday and is not itself the observed
    // Christmas holiday (Dec 25 on a Saturday makes Dec 24 a full closure).
    const Timestamp dec24 = from_ymd(y, 12, 24);
    if (iso_weekday(dec24) <= kFriday && !is_closed(dec24)) h.push_back(dec24);

    return h;
}

}  // namespace

CalendarSpec Calendar::us_equities_spec() {
    CalendarSpec s;
    s.mic = "XNYS";
    s.regular_open = hours{9} + minutes{30};
    s.regular_close = hours{16};
    s.half_day_close = hours{13};
    s.standard_utc_offset_hours = -5;
    s.daylight_utc_offset_hours = -4;

    // Ad-hoc deviations no rule can predict. Kept small and cited; extend as
    // events occur rather than pre-emptively.
    s.exceptions = {
        {from_ymd(2018, 12, 5), SessionKind::Closed, "national day of mourning: G.H.W. Bush"},
        {from_ymd(2025, 1, 9), SessionKind::Closed, "national day of mourning: J. Carter"},
    };
    return s;
}

Result<Calendar> Calendar::build(const CalendarSpec& spec, int first_year, int last_year) {
    if (last_year < first_year) {
        return fail(
            make_error(ErrorCode::InvalidArgument, "calendar last_year precedes first_year"));
    }
    if (first_year < 2008) {
        // The DST rule implemented here is the post-Energy-Policy-Act one.
        // Applying it to earlier years would shift session boundaries by an
        // hour for several weeks a year, silently.
        return fail(make_error(ErrorCode::Unsupported,
                               "calendar generation requires first_year >= 2008 "
                               "(DST rule changed in 2007)"));
    }
    if (spec.regular_close <= spec.regular_open || spec.half_day_close <= spec.regular_open) {
        return fail(make_error(ErrorCode::InvalidArgument, "calendar close must be after open"));
    }

    std::map<Timestamp, CalendarException> overrides;
    for (const auto& e : spec.exceptions) overrides.emplace(utc_date_floor(e.date), e);

    Calendar cal;
    cal.mic_ = spec.mic;

    for (int y = first_year; y <= last_year; ++y) {
        const auto closed = full_holidays(y);
        const auto early = half_days(y, closed);

        Timestamp d = from_ymd(y, 1, 1);
        const Timestamp end = from_ymd(y + 1, 1, 1);
        for (; d < end; d += days{1}) {
            Session s;
            s.date = d;

            const unsigned wd = iso_weekday(d);
            const bool weekend = wd >= kSaturday;
            const bool holiday = std::find(closed.begin(), closed.end(), d) != closed.end();
            const bool early_close = std::find(early.begin(), early.end(), d) != early.end();

            if (weekend || holiday) {
                s.kind = SessionKind::Closed;
            } else {
                s.kind = early_close ? SessionKind::HalfDay : SessionKind::Regular;
            }

            if (const auto it = overrides.find(d); it != overrides.end()) {
                s.kind = it->second.kind;
            }

            if (s.kind != SessionKind::Closed) {
                const int offset_h = is_us_daylight_time(d, y) ? spec.daylight_utc_offset_hours
                                                               : spec.standard_utc_offset_hours;
                const Duration to_utc = -hours{offset_h};
                s.open = d + spec.regular_open + to_utc;
                s.close =
                    d +
                    (s.kind == SessionKind::HalfDay ? spec.half_day_close : spec.regular_close) +
                    to_utc;
            }
            cal.all_.push_back(s);
            if (s.is_open()) cal.open_.push_back(s);
        }
    }
    return cal;
}

// ---------------------------------------------------------------------------
// Lookup
// ---------------------------------------------------------------------------

namespace {

[[nodiscard]] const Session* find_by_date(const std::vector<Session>& v, Timestamp date) noexcept {
    const auto it = std::lower_bound(v.begin(), v.end(), date,
                                     [](const Session& s, Timestamp t) { return s.date < t; });
    if (it == v.end() || it->date != date) return nullptr;
    return &*it;
}

}  // namespace

std::optional<Session> Calendar::session_on(Timestamp ts) const noexcept {
    const Session* s = find_by_date(all_, utc_date_floor(ts));
    if (s == nullptr) return std::nullopt;
    return *s;
}

std::optional<Session> Calendar::session_containing(Timestamp ts) const noexcept {
    // A UTC date is not an exchange date: 2024-01-02T00:30:00Z is still the
    // 2024-01-01 session in New York. Checking the neighbouring dates costs
    // nothing and removes a whole class of off-by-one-day bug.
    for (int offset = -1; offset <= 1; ++offset) {
        const Session* s = find_by_date(all_, utc_date_floor(ts) + days{offset});
        if (s != nullptr && s->contains(ts)) return *s;
    }
    return std::nullopt;
}

bool Calendar::is_trading_day(Timestamp ts) const noexcept {
    const auto s = session_on(ts);
    return s.has_value() && s->is_open();
}

bool Calendar::is_open_at(Timestamp ts) const noexcept {
    return session_containing(ts).has_value();
}

std::optional<Session> Calendar::next_session(Timestamp ts) const noexcept {
    const auto it = std::upper_bound(open_.begin(), open_.end(), utc_date_floor(ts),
                                     [](Timestamp t, const Session& s) { return t < s.date; });
    if (it == open_.end()) return std::nullopt;
    return *it;
}

std::optional<Session> Calendar::previous_session(Timestamp ts) const noexcept {
    const auto it = std::lower_bound(open_.begin(), open_.end(), utc_date_floor(ts),
                                     [](const Session& s, Timestamp t) { return s.date < t; });
    if (it == open_.begin()) return std::nullopt;
    return *std::prev(it);
}

Timestamp Calendar::range_begin() const noexcept {
    return all_.empty() ? kNoTimestamp : all_.front().date;
}
Timestamp Calendar::range_end() const noexcept {
    return all_.empty() ? kNoTimestamp : all_.back().date;
}

}  // namespace ptl::market
