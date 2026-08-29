#pragma once

/// \file logger.hpp
/// Structured logging facade.
///
/// Three design points:
///
/// 1. STRUCTURED, NOT PRINTF. Records are message + typed key/value fields,
///    emitted as JSON Lines. `jq` over a run's log is how you answer "show me
///    every order rejected for a price collar" without writing a parser.
///
/// 2. COMPILE-TIME GATED. Trace-level statements vanish entirely unless
///    PTL_ENABLE_TRACE is set, via `if constexpr` in the macro. A log call in
///    the per-event hot path costs literally nothing in a normal build --
///    which is what makes it safe to instrument the simulation loop at all.
///
/// 3. SIMULATION-TIME STAMPED. When a clock is installed, every record carries
///    BOTH sim_time and wall_time. This is not cosmetic: the paper-trading
///    parity check in Phase 12 compares a live journal against a replayed one,
///    and that comparison is impossible if every line differs. Stripping the
///    single wall_time field makes two identical runs byte-identical.
///
/// 4. FACADE, NOT A DEPENDENCY. The backend is selected at build time
///    (-DPTL_LOG_BACKEND=builtin|spdlog). No ptl header includes spdlog, so
///    the choice is reversible and compile times stay low.

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <variant>

#include "ptl/core/clock.hpp"

namespace ptl::log {

enum class Level : int { Trace = 0, Debug, Info, Warn, Error, Critical, Off };

[[nodiscard]] std::string_view to_string(Level l) noexcept;
[[nodiscard]] bool parse_level(std::string_view text, Level& out) noexcept;

/// Levels below this are removed by the compiler. Nothing at runtime can
/// re-enable them; that is the point.
inline constexpr Level kCompiledMinLevel =
#if defined(PTL_ENABLE_TRACE) && PTL_ENABLE_TRACE
    Level::Trace;
#else
    Level::Debug;
#endif

using FieldValue =
    std::variant<std::int64_t, std::uint64_t, double, bool, std::string_view, std::string>;

struct Field {
    std::string_view key;
    FieldValue value;
};

/// Field constructors, so call sites read as kv("symbol", "SPY").
///
/// String literals become string_view (they have static storage, so a view is
/// safe and avoids an allocation on the logging path). A std::string is stored
/// by value: it may be a temporary, and a view into it would dangle by the
/// time the record is formatted.
template <class V>
[[nodiscard]] inline Field kv(std::string_view key, V&& v) {
    using D = std::decay_t<V>;
    if constexpr (std::is_same_v<D, bool>) {
        return Field{key, FieldValue{static_cast<bool>(v)}};
    } else if constexpr (std::is_integral_v<D> && std::is_signed_v<D>) {
        return Field{key, FieldValue{static_cast<std::int64_t>(v)}};
    } else if constexpr (std::is_integral_v<D>) {
        return Field{key, FieldValue{static_cast<std::uint64_t>(v)}};
    } else if constexpr (std::is_enum_v<D>) {
        return Field{key, FieldValue{static_cast<std::int64_t>(v)}};
    } else if constexpr (std::is_floating_point_v<D>) {
        return Field{key, FieldValue{static_cast<double>(v)}};
    } else if constexpr (std::is_same_v<D, std::string>) {
        return Field{key, FieldValue{std::forward<V>(v)}};
    } else if constexpr (std::is_convertible_v<D, std::string_view>) {
        return Field{key, FieldValue{std::string_view{v}}};
    } else {
        static_assert(sizeof(D) == 0, "unsupported log field type");
    }
}

template <class... Fs>
[[nodiscard]] inline auto fields(Fs&&... fs) {
    return std::array<Field, sizeof...(Fs)>{std::forward<Fs>(fs)...};
}

struct Config {
    Level level = Level::Info;
    std::string file;  // empty = no file sink
    bool console = true;
    bool json = true;    // false = human-readable, for terminals
    bool async = false;  // Phase 12: lock-free queue + writer thread
    std::size_t queue_size = 8192;

    /// Simulation clock. NOT OWNED -- the caller must outlive log::shutdown().
    ///
    /// When null (the default), records carry wall_time only, which is correct
    /// for tools that do not run a simulation. When set, records also carry
    /// sim_time, and a deterministic replay produces a log that is byte-identical
    /// across runs once wall_time is removed.
    const IClock* sim_clock = nullptr;
};

class Logger {
public:
    void log(Level level, std::string_view message, std::span<const Field> fields) const;

    [[nodiscard]] bool enabled(Level level) const noexcept {
        return static_cast<int>(level) >= static_cast<int>(level_);
    }

    void set_level(Level l) noexcept { level_ = l; }
    [[nodiscard]] Level level() const noexcept { return level_; }
    [[nodiscard]] std::string_view subsystem() const noexcept { return subsystem_; }

private:
    friend Logger& get(std::string_view);
    explicit Logger(std::string_view subsystem) : subsystem_(subsystem) {}

    std::string_view subsystem_;
    Level level_ = Level::Info;
};

/// Per-subsystem logger. `name` must have static storage duration (a literal).
/// Stable across calls, so it is safe to cache in a static local.
[[nodiscard]] Logger& get(std::string_view name);

void init(const Config& cfg);
void shutdown();

/// Number of records dropped by an async sink under back-pressure. Silently
/// losing log lines would undermine the audit trail, so it is counted and
/// reported at shutdown.
[[nodiscard]] std::uint64_t dropped_records() noexcept;

}  // namespace ptl::log

// --- macros ---------------------------------------------------------------
//
// The `if constexpr` is what erases sub-threshold calls at compile time; the
// runtime `enabled()` check then handles the configured level. Arguments are
// not evaluated when the level is compiled out.

#define PTL_LOG_AT(logger_, level_, msg_, ...)                        \
    do {                                                              \
        if constexpr ((level_) >= ::ptl::log::kCompiledMinLevel) {    \
            const auto& ptl_lg_ = (logger_);                          \
            if (ptl_lg_.enabled(level_)) {                            \
                const auto ptl_fs_ = ::ptl::log::fields(__VA_ARGS__); \
                ptl_lg_.log((level_), (msg_), ptl_fs_);               \
            }                                                         \
        }                                                             \
    } while (false)

#define PTL_TRACE(lg, msg, ...) PTL_LOG_AT(lg, ::ptl::log::Level::Trace, msg, __VA_ARGS__)
#define PTL_DEBUG(lg, msg, ...) PTL_LOG_AT(lg, ::ptl::log::Level::Debug, msg, __VA_ARGS__)
#define PTL_INFO(lg, msg, ...) PTL_LOG_AT(lg, ::ptl::log::Level::Info, msg, __VA_ARGS__)
#define PTL_WARN(lg, msg, ...) PTL_LOG_AT(lg, ::ptl::log::Level::Warn, msg, __VA_ARGS__)
#define PTL_ERROR(lg, msg, ...) PTL_LOG_AT(lg, ::ptl::log::Level::Error, msg, __VA_ARGS__)
#define PTL_CRIT(lg, msg, ...) PTL_LOG_AT(lg, ::ptl::log::Level::Critical, msg, __VA_ARGS__)
