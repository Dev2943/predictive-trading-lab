#pragma once

/// \file rng.hpp
/// Deterministic pseudo-random numbers.
///
/// Two rules, both load-bearing for reproducibility:
///
/// 1. NO GLOBAL STATE. Every consumer receives its own generator, forked from
///    the run seed with a distinct stream id. Adding a latency model must not
///    change the fills produced by the slippage model.
///
/// 2. NO <random> DISTRIBUTIONS. std::uniform_real_distribution and
///    std::normal_distribution are specified by their statistical properties,
///    NOT by the sequence they emit. libstdc++ and libc++ genuinely produce
///    different numbers from the same engine and seed -- so a result set
///    generated on your Mac would not reproduce on Linux CI. The transforms
///    below are fixed and defined here, so the sequence is part of the
///    project rather than part of the standard library.
///
/// Engine is xoshiro256** (Blackman & Vigna), seeded through splitmix64.

#include <array>
#include <cstdint>

namespace ptl {

class DeterministicRng {
public:
    explicit DeterministicRng(std::uint64_t seed) noexcept;

    [[nodiscard]] std::uint64_t next_u64() noexcept;

    /// Uniform on [0, 1). 53 significant bits, the most a double can hold.
    [[nodiscard]] double uniform01() noexcept;

    [[nodiscard]] double uniform(double lo, double hi) noexcept;

    /// Unbiased integer in [0, n). Lemire's multiply-shift with rejection --
    /// the naive `next_u64() % n` is biased for n that do not divide 2^64.
    [[nodiscard]] std::uint64_t bounded(std::uint64_t n) noexcept;

    /// Box-Muller with a cached spare. Note the cache is part of the object's
    /// state, so a forked generator does not inherit it.
    [[nodiscard]] double normal(double mean = 0.0, double stddev = 1.0) noexcept;

    /// Exponential with the given rate. Used by the latency model.
    [[nodiscard]] double exponential(double rate) noexcept;

    /// A statistically independent stream derived from the same run seed.
    /// Callers pass a stable, hand-assigned stream id (see kStream* below) so
    /// that adding a new consumer never perturbs existing ones.
    [[nodiscard]] DeterministicRng fork(std::uint64_t stream_id) const noexcept;

    [[nodiscard]] std::uint64_t seed() const noexcept { return seed_; }

private:
    std::array<std::uint64_t, 4> s_{};
    std::uint64_t seed_{};
    double spare_normal_{0.0};
    bool has_spare_{false};
};

/// Stream ids. Append only -- never renumber, or historical runs stop
/// reproducing.
inline constexpr std::uint64_t kStreamLatency = 1;
inline constexpr std::uint64_t kStreamSlippage = 2;
inline constexpr std::uint64_t kStreamFillProb = 3;
inline constexpr std::uint64_t kStreamIntrabar = 4;
inline constexpr std::uint64_t kStreamBootstrap = 5;

}  // namespace ptl
