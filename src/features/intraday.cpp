#include "ptl/features/intraday.hpp"

#include "ptl/features/feature.hpp"

namespace ptl::features {

std::int32_t minute_of_session(Timestamp ts, const market::Session& session) noexcept {
    if (!session.is_open() || ts < session.open) return -1;
    // SESSION-relative, not clock-relative: a half day and a regular day both
    // start at minute 0, so a per-minute statistic stays aligned on the ~6
    // sessions a year that close early (ADR-0001 Addendum A2).
    const auto elapsed = ts - session.open;
    const auto minutes = std::chrono::duration_cast<std::chrono::minutes>(elapsed).count();
    if (minutes < 0) return -1;
    return static_cast<std::int32_t>(minutes);
}

MinuteOfDayVolumeProfile::MinuteOfDayVolumeProfile(std::size_t slots, std::size_t lookback)
    : lookback_(std::max<std::size_t>(1, lookback)) {
    means_.reserve(std::max<std::size_t>(1, slots));
    for (std::size_t i = 0; i < std::max<std::size_t>(1, slots); ++i) {
        means_.emplace_back(lookback_);
    }
}

void MinuteOfDayVolumeProfile::update(std::int32_t minute, double volume) noexcept {
    if (minute < 0 || !is_finite(volume) || volume < 0.0) return;
    const auto slot = static_cast<std::size_t>(minute);
    if (slot >= means_.size()) return;
    means_[slot].update(volume);
}

double MinuteOfDayVolumeProfile::baseline(std::int32_t minute) const noexcept {
    if (minute < 0) return 0.0;
    const auto slot = static_cast<std::size_t>(minute);
    if (slot >= means_.size()) return 0.0;
    return means_[slot].value();
}

double MinuteOfDayVolumeProfile::relative_volume(std::int32_t minute,
                                                 double volume) const noexcept {
    const double base = baseline(minute);
    // Neutral rather than infinite. A slot with no volume history is
    // uninformative, not infinitely surprising, and an inf here would flow into
    // a feature vector and poison every downstream aggregate.
    if (base <= 0.0 || !is_finite(base) || !is_finite(volume)) return 1.0;
    const double r = volume / base;
    return is_finite(r) ? r : 1.0;
}

bool MinuteOfDayVolumeProfile::ready(std::int32_t minute) const noexcept {
    if (minute < 0) return false;
    const auto slot = static_cast<std::size_t>(minute);
    if (slot >= means_.size()) return false;
    return means_[slot].ready();
}

void MinuteOfDayVolumeProfile::reset() noexcept {
    for (auto& m : means_) m.reset();
}

std::uint64_t compute_feature_set_id(std::span<const std::string> signatures) {
    // Order-sensitive by design: the values array is positional, so a
    // reordering produces a different matrix and must not reuse a cache entry.
    std::uint64_t h = 0xcbf29ce484222325ULL;
    for (const auto& s : signatures) {
        for (const char c : s) {
            h ^= static_cast<std::uint64_t>(static_cast<unsigned char>(c));
            h *= 0x100000001b3ULL;
        }
        h ^= 0x1fULL;  // separator, so "ab"+"c" and "a"+"bc" differ
        h *= 0x100000001b3ULL;
    }
    return h;
}

}  // namespace ptl::features
