#include "ptl/core/rng.hpp"

#include <cmath>

namespace ptl {
namespace {

[[nodiscard]] constexpr std::uint64_t splitmix64(std::uint64_t& x) noexcept {
    std::uint64_t z = (x += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

[[nodiscard]] constexpr std::uint64_t rotl(std::uint64_t x, int k) noexcept {
    return (x << k) | (x >> (64 - k));
}

/// Full 64x64 -> 128 multiply. Returns the high word and writes the low word.
///
/// __int128 is a compiler extension, not ISO C++, so -Wpedantic rejects naming
/// it directly. The split below is portable to any conforming compiler; the
/// extension path is kept because it is a single instruction on both x86-64 and
/// AArch64 and this is used inside the rejection loop of bounded().
[[nodiscard]] inline std::uint64_t mul64x64_hi(std::uint64_t a, std::uint64_t b,
                                               std::uint64_t& low) noexcept {
#if defined(__SIZEOF_INT128__)
#  if defined(__GNUC__) || defined(__clang__)
#    pragma GCC diagnostic push
#    pragma GCC diagnostic ignored "-Wpedantic"
#  endif
    using u128 = unsigned __int128;
    const u128 prod = static_cast<u128>(a) * static_cast<u128>(b);
    low = static_cast<std::uint64_t>(prod);
    return static_cast<std::uint64_t>(prod >> 64);
#  if defined(__GNUC__) || defined(__clang__)
#    pragma GCC diagnostic pop
#  endif
#else
    const std::uint64_t a_lo = a & 0xFFFFFFFFULL;
    const std::uint64_t a_hi = a >> 32;
    const std::uint64_t b_lo = b & 0xFFFFFFFFULL;
    const std::uint64_t b_hi = b >> 32;

    const std::uint64_t p0 = a_lo * b_lo;
    const std::uint64_t p1 = a_lo * b_hi;
    const std::uint64_t p2 = a_hi * b_lo;
    const std::uint64_t p3 = a_hi * b_hi;

    const std::uint64_t carry = ((p0 >> 32) + (p1 & 0xFFFFFFFFULL) + (p2 & 0xFFFFFFFFULL)) >> 32;
    low = p0 + (p1 << 32) + (p2 << 32);
    return p3 + (p1 >> 32) + (p2 >> 32) + carry;
#endif
}

}  // namespace

DeterministicRng::DeterministicRng(std::uint64_t seed) noexcept : seed_(seed) {
    // splitmix64 expansion avoids the poor initial output xoshiro gives from a
    // low-entropy state such as seed = 1.
    std::uint64_t x = seed;
    for (auto& word : s_) word = splitmix64(x);
}

std::uint64_t DeterministicRng::next_u64() noexcept {
    const std::uint64_t result = rotl(s_[1] * 5, 7) * 9;  // xoshiro256**
    const std::uint64_t t = s_[1] << 17;
    s_[2] ^= s_[0];
    s_[3] ^= s_[1];
    s_[1] ^= s_[2];
    s_[0] ^= s_[3];
    s_[2] ^= t;
    s_[3] = rotl(s_[3], 45);
    return result;
}

double DeterministicRng::uniform01() noexcept {
    // Top 53 bits scaled by 2^-53: exactly representable, uniform on [0,1),
    // and never returns 1.0.
    return static_cast<double>(next_u64() >> 11) * 0x1.0p-53;
}

double DeterministicRng::uniform(double lo, double hi) noexcept {
    return lo + (hi - lo) * uniform01();
}

std::uint64_t DeterministicRng::bounded(std::uint64_t n) noexcept {
    if (n == 0) return 0;
    // Lemire: multiply into the high half, reject the short interval that
    // would otherwise bias low values. `% n` alone is biased whenever n does
    // not divide 2^64.
    const auto mul = [n](std::uint64_t x, std::uint64_t& low) {
        return mul64x64_hi(x, n, low);
    };
    std::uint64_t low = 0;
    std::uint64_t hi_ = mul(next_u64(), low);
    if (low < n) {
        const std::uint64_t threshold = (~n + 1U) % n;  // (2^64 - n) % n
        while (low < threshold) hi_ = mul(next_u64(), low);
    }
    return hi_;
}

double DeterministicRng::normal(double mean, double stddev) noexcept {
    if (has_spare_) {
        has_spare_ = false;
        return mean + stddev * spare_normal_;
    }
    // Polar Box-Muller. Rejection sampling makes the number of engine draws
    // data-dependent, which is fine: the sequence is still a pure function of
    // the seed.
    double u = 0.0, v = 0.0, sq = 0.0;
    do {
        u  = 2.0 * uniform01() - 1.0;
        v  = 2.0 * uniform01() - 1.0;
        sq = u * u + v * v;
    } while (sq >= 1.0 || sq == 0.0);

    const double f = std::sqrt(-2.0 * std::log(sq) / sq);
    spare_normal_  = v * f;
    has_spare_     = true;
    return mean + stddev * (u * f);
}

double DeterministicRng::exponential(double rate) noexcept {
    // 1 - uniform01() keeps the argument strictly positive, since uniform01()
    // can return exactly 0.0 and log(0) is -inf.
    return -std::log(1.0 - uniform01()) / rate;
}

DeterministicRng DeterministicRng::fork(std::uint64_t stream_id) const noexcept {
    // Mix the stream id into the ROOT seed rather than the current state, so a
    // fork is a pure function of (seed, stream_id). Consumers can therefore be
    // constructed in any order without perturbing each other.
    std::uint64_t x = seed_ ^ (stream_id * 0x9E3779B97F4A7C15ULL);
    return DeterministicRng{splitmix64(x)};
}

}  // namespace ptl
