#pragma once

/// \file result.hpp
/// Error propagation for fallible, non-hot-path operations: parsing, config
/// loading, file IO. The simulation loop does not use this -- it validates up
/// front and then runs over data it already trusts.

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "ptl/core/compiler.hpp"

#if PTL_HAS_STD_EXPECTED
#include <expected>
#endif

namespace ptl {

enum class ErrorCode {
    Ok = 0,
    NotFound,
    ParseError,
    InvalidArgument,
    IoError,
    ValidationFailed,
    ConfigError,
    Unsupported,
};

[[nodiscard]] std::string_view to_string(ErrorCode c) noexcept;

struct Error {
    ErrorCode code = ErrorCode::Ok;
    std::string message;
    std::string context;  // file:line, symbol, row number -- whatever locates it

    [[nodiscard]] std::string describe() const;
};

[[nodiscard]] inline Error make_error(ErrorCode code, std::string message,
                                      std::string context = {}) {
    return Error{code, std::move(message), std::move(context)};
}

#if PTL_HAS_STD_EXPECTED

template <class T>
using Result = std::expected<T, Error>;

[[nodiscard]] inline std::unexpected<Error> fail(Error e) {
    return std::unexpected<Error>{std::move(e)};
}

#else

/// Minimal std::expected substitute for toolchains that lack it (older Xcode).
///
/// Storage is std::optional<T>, NOT a bare `T value_{}`.
///
/// That distinction is load-bearing. Several types in this project have PRIVATE
/// default constructors on purpose -- Calendar, Bar, Quote, Trade,
/// ReplaySource, FileCredentials -- so that every instance is forced through a
/// validating factory and an invalid one cannot exist to be passed around. A
/// member of type T would require T to be default-constructible, which would
/// make Result<T> unusable for exactly the types whose invariants matter most.
///
/// The fix is to change the CONTAINER, never to relax the invariant. Loosening
/// those constructors to satisfy a storage detail would trade a real guarantee
/// for a workaround, and on the one toolchain path that is hardest to test.
///
/// Deliberately a subset: no monadic operations. If your compiler has the real
/// std::expected you get the real one; this exists so the project still builds
/// if it does not.
template <class T>
class Result {
public:
    Result(T value) : value_(std::move(value)) {}      // NOLINT(google-explicit-constructor)
    Result(Error error) : error_(std::move(error)) {}  // NOLINT(google-explicit-constructor)

    [[nodiscard]] bool has_value() const noexcept { return value_.has_value(); }
    explicit operator bool() const noexcept { return value_.has_value(); }

    [[nodiscard]] T& value() { return *value_; }
    [[nodiscard]] const T& value() const { return *value_; }
    [[nodiscard]] const Error& error() const { return error_; }

    T* operator->() { return &*value_; }
    const T* operator->() const { return &*value_; }
    T& operator*() { return *value_; }
    const T& operator*() const { return *value_; }

private:
    std::optional<T> value_;
    Error error_{};
};

[[nodiscard]] inline Error fail(Error e) {
    return e;
}

#endif

/// FNV-1a 64. Not cryptographic -- it exists to build stable RunIds and cache
/// keys from config text. Chosen over xxhash to avoid a dependency for
/// something this small; swap it if collisions ever become a concern.
[[nodiscard]] constexpr std::uint64_t fnv1a64(std::string_view s) noexcept {
    std::uint64_t h = 0xcbf29ce484222325ULL;
    for (const char c : s) {
        h ^= static_cast<std::uint64_t>(static_cast<unsigned char>(c));
        h *= 0x100000001b3ULL;
    }
    return h;
}

}  // namespace ptl
