#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "support/ptl_catch.hpp"

#include <array>
#include <cmath>
#include <vector>

#include "ptl/core/rng.hpp"

using namespace ptl;

TEST_CASE("rng sequence is a fixed function of the seed", "[core][rng][determinism]") {
    // GOLDEN VALUES. These are not arbitrary: they are the reason results
    // generated on macOS reproduce byte-for-byte on Linux CI. We deliberately
    // do not use std::uniform_real_distribution or std::normal_distribution,
    // which are specified by their statistical properties and NOT by the
    // sequence they emit -- libstdc++ and libc++ genuinely differ. If this test
    // fails, either the engine changed or someone reached for <random>, and
    // every previously recorded RunId is now unreproducible.
    DeterministicRng rng{20240101};
    const std::array<std::uint64_t, 4> expected{
        0xd05ef55272cdfb14ULL, 0x2e2f422341add64eULL, 0x1c120f3d1ce63170ULL, 0x760a212e6c3beec9ULL};
    for (std::size_t i = 0; i < expected.size(); ++i) {
        INFO("draw " << i);
        REQUIRE(rng.next_u64() == expected[i]);
    }
}

TEST_CASE("same seed reproduces different seed diverges", "[core][rng][determinism]") {
    DeterministicRng a{42};
    DeterministicRng b{42};
    DeterministicRng c{43};
    for (int i = 0; i < 64; ++i) REQUIRE(a.next_u64() == b.next_u64());

    DeterministicRng d{42};
    bool any_differ = false;
    for (int i = 0; i < 64; ++i) {
        if (d.next_u64() != c.next_u64()) any_differ = true;
    }
    REQUIRE(any_differ);
}

TEST_CASE("uniform01 stays in 01)", "[core][rng]") {
    DeterministicRng rng{7};
    double lo = 1.0;
    double hi = 0.0;
    for (int i = 0; i < 200000; ++i) {
        const double u = rng.uniform01();
        REQUIRE(u >= 0.0);
        REQUIRE(u < 1.0);  // strictly less: log(1-u) must never be -inf
        lo = std::min(lo, u);
        hi = std::max(hi, u);
    }
    REQUIRE(lo < 0.001);
    REQUIRE(hi > 0.999);
}

TEST_CASE("forked streams are independent and order-insensitive", "[core][rng][determinism]") {
    // A fork is a pure function of (root seed, stream id), NOT of the parent's
    // current state. That is what lets us add a latency model without changing
    // the fills the slippage model produces -- if forks consumed parent state,
    // every new consumer would silently perturb every existing one.
    const DeterministicRng root{20240101};

    DeterministicRng lat_a = root.fork(kStreamLatency);
    DeterministicRng slip_a = root.fork(kStreamSlippage);

    // Same forks, requested in the opposite order, after the root has advanced.
    DeterministicRng advanced{20240101};
    for (int i = 0; i < 1000; ++i) (void)advanced.next_u64();
    DeterministicRng slip_b = advanced.fork(kStreamSlippage);
    DeterministicRng lat_b = advanced.fork(kStreamLatency);

    for (int i = 0; i < 32; ++i) {
        REQUIRE(lat_a.next_u64() == lat_b.next_u64());
        REQUIRE(slip_a.next_u64() == slip_b.next_u64());
    }

    DeterministicRng l{root.fork(kStreamLatency)};
    DeterministicRng s{root.fork(kStreamSlippage)};
    bool differ = false;
    for (int i = 0; i < 32; ++i) {
        if (l.next_u64() != s.next_u64()) differ = true;
    }
    REQUIRE(differ);
}

TEST_CASE("bounded is unbiased across the range", "[core][rng]") {
    // Lemire with rejection, not `% n`. The naive modulo is biased toward low
    // values whenever n does not divide 2^64; at the sample sizes a backtest
    // uses that bias is invisible in a histogram but real in aggregate.
    constexpr std::uint64_t kN = 7;  // does not divide 2^64
    constexpr int kDraws = 700000;
    DeterministicRng rng{99};
    std::vector<int> counts(kN, 0);
    for (int i = 0; i < kDraws; ++i) {
        const auto v = rng.bounded(kN);
        REQUIRE(v < kN);
        counts[v]++;
    }
    const double expected = static_cast<double>(kDraws) / static_cast<double>(kN);
    for (const int c : counts) {
        REQUIRE(std::abs(static_cast<double>(c) - expected) / expected < 0.02);
    }
    REQUIRE(rng.bounded(0) == 0);
}

TEST_CASE("normal has the requested moments", "[core][rng]") {
    DeterministicRng rng{123};
    constexpr int kN = 400000;
    double sum = 0.0;
    double sumsq = 0.0;
    for (int i = 0; i < kN; ++i) {
        const double x = rng.normal(2.0, 3.0);
        sum += x;
        sumsq += x * x;
    }
    const double mean = sum / kN;
    const double var = sumsq / kN - mean * mean;
    REQUIRE(mean == Catch::Approx(2.0).margin(0.03));
    REQUIRE(std::sqrt(var) == Catch::Approx(3.0).margin(0.03));
}

TEST_CASE("exponential is positive and has the right mean", "[core][rng]") {
    DeterministicRng rng{5};
    constexpr int kN = 200000;
    double sum = 0.0;
    for (int i = 0; i < kN; ++i) {
        const double x = rng.exponential(4.0);
        REQUIRE(x >= 0.0);
        REQUIRE(std::isfinite(x));  // 1-u keeps log() away from -inf
        sum += x;
    }
    REQUIRE(sum / kN == Catch::Approx(0.25).margin(0.005));
}
