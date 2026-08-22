#pragma once

/// \file compiler.hpp
/// Toolchain capability probes.
///
/// AppleClang ships a libc++ that trails upstream LLVM by one to two releases,
/// so a feature being "C++23" says nothing about whether the developer's Xcode
/// actually has it. Everything here is a probe, never an assumption. Code that
/// needs a capability asks these macros; it never tests __clang_major__.

#include <version>

// std::expected -- libc++ 16, libstdc++ 12. Xcode < 15.3 may require
// -fexperimental-library. ptl::Result falls back to a local implementation.
// PTL_FORCE_RESULT_FALLBACK exists so CI compiles the fallback path on every
// build. An untested fallback is worse than no fallback: it fails only on the
// machine that needed it, which is by definition the machine you cannot debug.
#if defined(PTL_FORCE_RESULT_FALLBACK) && PTL_FORCE_RESULT_FALLBACK
#define PTL_HAS_STD_EXPECTED 0
#elif defined(__cpp_lib_expected) && __cpp_lib_expected >= 202202L
#define PTL_HAS_STD_EXPECTED 1
#else
#define PTL_HAS_STD_EXPECTED 0
#endif

// std::format -- libc++ 17, libstdc++ 13.
#if defined(__cpp_lib_format) && __cpp_lib_format >= 201907L
#define PTL_HAS_STD_FORMAT 1
#else
#define PTL_HAS_STD_FORMAT 0
#endif

// std::chrono time-zone database (std::chrono::tzdb, zoned_time).
//
// This landed in libc++ only in LLVM 19 and needs a matching runtime, so on
// macOS it is effectively unavailable. THE PROJECT THEREFORE NEVER USES IT.
// All timestamps are UTC nanoseconds since epoch; exchange session boundaries
// load as precomputed UTC instants from data/reference/calendars/. That is
// also what production systems do -- it removes a runtime dependency on the
// host tz database and keeps the simulation bit-reproducible across machines.
#if defined(__cpp_lib_chrono) && __cpp_lib_chrono >= 201907L
#define PTL_HAS_CHRONO_TZDB 1
#else
#define PTL_HAS_CHRONO_TZDB 0
#endif

#if defined(_MSC_VER)
#define PTL_ALWAYS_INLINE __forceinline
#define PTL_NOINLINE __declspec(noinline)
#else
#define PTL_ALWAYS_INLINE inline __attribute__((always_inline))
#define PTL_NOINLINE __attribute__((noinline))
#endif

namespace ptl {
// 64 on x86-64 and on Apple Silicon for coherency purposes, though M-series
// has a 128-byte prefetch granule. Revisit under measurement in Phase 12
// rather than guessing now.
inline constexpr int kCacheLineSize = 64;
}  // namespace ptl
