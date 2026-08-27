#pragma once

/// \file ptl_catch.hpp
/// Catch2 stringification for ptl vocabulary types.
///
/// WHY THIS FILE EXISTS -- a real libc++/libstdc++ divergence.
///
/// Catch2 ships a partial specialisation
///
///     StringMaker<std::chrono::time_point<std::chrono::system_clock, Duration>>
///
/// whose convert() calls std::chrono::system_clock::to_time_t(). That function
/// takes time_point<system_clock, system_clock::duration>, and
/// system_clock::duration is NOT the same type across standard libraries:
///
///     libstdc++ (GCC/Linux)      -> nanoseconds
///     libc++    (AppleClang)     -> microseconds
///
/// ptl::Timestamp is time_point<system_clock, nanoseconds> by deliberate
/// choice. On libstdc++ it converts to to_time_t's parameter implicitly and
/// nobody notices. On libc++ the conversion is nanoseconds -> microseconds,
/// which is lossy, and chrono refuses to do that implicitly. The result is a
/// hard compile error inside catch_tostring.hpp on macOS only.
///
/// The fix is to specialise StringMaker for ptl::Timestamp ourselves. An
/// explicit (full) specialisation is more specialised than Catch2's partial
/// one, so ours is selected and Catch2's is never instantiated -- on ANY
/// platform. No third-party source is modified, and the divergence stops
/// mattering rather than being papered over for one toolchain.
///
/// It also produces strictly better failure output: Catch2's default renders a
/// second-resolution date, which is useless when the assertion that failed was
/// about a one-nanosecond ordering violation in the point-in-time chain.
///
/// Include this header BEFORE any use of these types in an assertion. An
/// explicit specialisation must be declared before the first use that would
/// implicitly instantiate the primary template.

#include <catch2/catch_tostring.hpp>
#include <chrono>
#include <string>

#include "ptl/core/named_type.hpp"
#include "ptl/core/time.hpp"
#include "ptl/core/types.hpp"

namespace Catch {

/// ISO-8601 with full nanosecond precision, plus readable sentinels.
template <>
struct StringMaker<ptl::Timestamp> {
    static std::string convert(const ptl::Timestamp& ts) {
        if (ts == ptl::kNoTimestamp) return "<unset>";
        if (ts == ptl::kMaxTimestamp) return "<max>";
        return ptl::to_iso8601(ts);
    }
};

/// Strong typedefs print as Price(512.25) rather than as a bare double, so a
/// failure message says which unit was wrong, not just which number.
template <class T, class Tag, template <class> class... Skills>
struct StringMaker<ptl::NamedType<T, Tag, Skills...>> {
    static std::string convert(const ptl::NamedType<T, Tag, Skills...>& v) {
        return StringMaker<T>::convert(v.get());
    }
};

template <>
struct StringMaker<ptl::Stage> {
    static std::string convert(const ptl::Stage& s) { return std::string{ptl::to_string(s)}; }
};

template <>
struct StringMaker<ptl::Side> {
    static std::string convert(const ptl::Side& s) { return std::string{ptl::to_string(s)}; }
};

template <>
struct StringMaker<ptl::InstrumentId> {
    static std::string convert(const ptl::InstrumentId& id) {
        return "InstrumentId(" + std::to_string(ptl::index_of(id)) + ")";
    }
};

template <>
struct StringMaker<ptl::ChainRule> {
    static std::string convert(const ptl::ChainRule& r) {
        return r == ptl::ChainRule::StrictlyAfter ? "StrictlyAfter" : "Monotonic";
    }
};

}  // namespace Catch
