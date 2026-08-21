#pragma once

/// \file result.hpp
/// Error propagation for fallible, non-hot-path operations: parsing, config
/// loading, file IO. The simulation loop does not use this -- it validates up
/// front and then runs over data it already trusts.

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

#include "ptl/core/compiler.hpp"

#if PTL_HAS_STD_EXPECTED
#  include <expected>
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
    ErrorCode   code = ErrorCode::Ok;
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
/// Deliberately a subset -- no monadic operations. If your compiler has the
/// real thing you get the real thing; this exists so the project still builds
/// if it does not.
template <class T>
class Result {
public:
    Result(T value) : has_(true), value_(std::move(value)) {}       // NOLINT(google-explicit-constructor)
    Result(Error error) : has_(false), error_(std::move(error)) {}  // NOLINT(google-explicit-constructor)

    [[nodiscard]] bool has_value() const noexcept { return has_; }
    explicit operator bool() const noexcept { return has_; }

    [[nodiscard]] T&           value()       { return value_; }
    [[nodiscard]] const T&     value() const { return value_; }
    [[nodiscard]] const Error& error() const { return error_; }

    T*       operator->()       { return &value_; }
    const T* operator->() const { return &value_; }
    T&       operator*()        { return value_; }
    const T& operator*() const  { return value_; }

private:
    bool  has_;
    T     value_{};
    Error error_{};
};

[[nodiscard]] inline Error fail(Error e) { return e; }

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
