#include "ptl/core/types.hpp"

#include <charconv>
#include <cstdio>

namespace ptl {
namespace {

/// Fixed-width unsigned parse. from_chars rather than atoi: no locale, no
/// errno, no allocation, and it reports exactly how many characters it used.
[[nodiscard]] bool parse_uint(std::string_view s, std::size_t pos, std::size_t len,
                              int& out) noexcept {
    if (pos + len > s.size()) return false;
    const char* first = s.data() + pos;
    const auto [ptr, ec] = std::from_chars(first, first + len, out);
    return ec == std::errc{} && ptr == first + len;
}

}  // namespace

bool parse_timestamp(std::string_view text, Timestamp& out) noexcept {
    using namespace std::chrono;

    if (text.size() < 10) return false;
    if (text[4] != '-' || text[7] != '-') return false;

    int y = 0, m = 0, d = 0;
    if (!parse_uint(text, 0, 4, y)) return false;
    if (!parse_uint(text, 5, 2, m)) return false;
    if (!parse_uint(text, 8, 2, d)) return false;

    const year_month_day ymd{year{y}, month{static_cast<unsigned>(m)},
                             day{static_cast<unsigned>(d)}};
    if (!ymd.ok()) return false;

    auto point = sys_days{ymd};
    auto ns    = nanoseconds{0};

    if (text.size() > 10) {
        const char sep = text[10];
        if (sep != 'T' && sep != ' ') return false;
        if (text.size() < 19) return false;
        if (text[13] != ':' || text[16] != ':') return false;

        int hh = 0, mm = 0, ss = 0;
        if (!parse_uint(text, 11, 2, hh)) return false;
        if (!parse_uint(text, 14, 2, mm)) return false;
        if (!parse_uint(text, 17, 2, ss)) return false;
        // Leap seconds are not represented in sys_time; 60 is rejected rather
        // than silently rolled over.
        if (hh > 23 || mm > 59 || ss > 59) return false;

        ns = hours{hh} + minutes{mm} + seconds{ss};

        if (text.size() > 19 && text[19] == '.') {
            std::size_t i = 20;
            std::int64_t frac = 0;
            int digits = 0;
            while (i < text.size() && text[i] >= '0' && text[i] <= '9' && digits < 9) {
                frac = frac * 10 + (text[i] - '0');
                ++i;
                ++digits;
            }
            if (digits == 0) return false;
            for (int k = digits; k < 9; ++k) frac *= 10;
            ns += nanoseconds{frac};
            // Ignore any excess precision beyond nanoseconds.
            while (i < text.size() && text[i] >= '0' && text[i] <= '9') ++i;
            if (i < text.size() && text[i] != 'Z') return false;
        } else if (text.size() > 19 && text[19] != 'Z') {
            return false;
        }
    }

    out = Timestamp{duration_cast<nanoseconds>(point.time_since_epoch()) + ns};
    return true;
}

std::string to_iso8601(Timestamp ts) {
    using namespace std::chrono;

    const auto days_part = floor<days>(ts);
    const year_month_day ymd{days_part};
    const auto since_midnight = ts - days_part;
    const auto h  = duration_cast<hours>(since_midnight);
    const auto mi = duration_cast<minutes>(since_midnight - h);
    const auto s  = duration_cast<seconds>(since_midnight - h - mi);
    const auto ns = duration_cast<nanoseconds>(since_midnight - h - mi - s);

    char buf[40];
    std::snprintf(buf, sizeof(buf), "%04d-%02u-%02uT%02lld:%02lld:%02lld.%09lldZ",
                  static_cast<int>(ymd.year()),
                  static_cast<unsigned>(ymd.month()),
                  static_cast<unsigned>(ymd.day()),
                  static_cast<long long>(h.count()),
                  static_cast<long long>(mi.count()),
                  static_cast<long long>(s.count()),
                  static_cast<long long>(ns.count()));
    return std::string{buf};
}

std::string to_date_string(Timestamp ts) {
    using namespace std::chrono;
    const year_month_day ymd{floor<days>(ts)};
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%04d-%02u-%02u",
                  static_cast<int>(ymd.year()),
                  static_cast<unsigned>(ymd.month()),
                  static_cast<unsigned>(ymd.day()));
    return std::string{buf};
}

Timestamp utc_date_floor(Timestamp ts) noexcept {
    using namespace std::chrono;
    return Timestamp{duration_cast<nanoseconds>(floor<days>(ts).time_since_epoch())};
}

}  // namespace ptl
