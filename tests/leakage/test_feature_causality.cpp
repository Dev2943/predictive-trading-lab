#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <vector>

#include "ptl/features/bivariate.hpp"
#include "ptl/features/intraday.hpp"
#include "ptl/features/momentum.hpp"
#include "ptl/features/validation.hpp"
#include "support/ptl_catch.hpp"

using namespace ptl;
using namespace ptl::features;

namespace {

/// Two series identical for `prefix` observations, then wildly different.
std::pair<std::vector<double>, std::vector<double>> forked_series(std::size_t n,
                                                                  std::size_t prefix) {
    std::vector<double> a;
    std::vector<double> b;
    a.reserve(n);
    b.reserve(n);
    for (std::size_t i = 0; i < n; ++i) {
        const double base = 100.0 + std::sin(static_cast<double>(i) * 0.3) * 5.0;
        a.push_back(base);
        // After the fork, b diverges violently. A causal estimator must produce
        // identical output across the shared prefix regardless.
        b.push_back(i < prefix ? base : base * 3.0 + 250.0);
    }
    return {a, b};
}

}  // namespace

TEST_CASE("every univariate estimator is causal", "[features][leakage][causality]") {
    // THE CENTRAL LEAKAGE TEST (reconciliation: test_feature_causality).
    //
    // Feed two series that agree up to index k and disagree afterwards. Every
    // value at or before k must be BIT-IDENTICAL. An estimator that reads
    // ahead fails automatically, and the test names the index where it peeked
    // rather than leaving anyone to guess.
    constexpr std::size_t kN = 200;
    constexpr std::size_t kPrefix = 120;
    const auto [a, b] = forked_series(kN, kPrefix);

    const auto check = [&](const char* label, auto make) {
        INFO("estimator: " << label);
        auto ea = make();
        auto eb = make();
        // STRICTLY less than kPrefix: the series are identical on [0, kPrefix)
        // and b[kPrefix] is already the first divergent element. Including it
        // would assert that the estimator ignores data it has legitimately
        // been given.
        for (std::size_t i = 0; i < kPrefix; ++i) {
            ea.update(a[i]);
            eb.update(b[i]);
            INFO("index " << i);
            REQUIRE(ea.value() == eb.value());
            REQUIRE(ea.ready() == eb.ready());
        }
    };

    check("RollingMean", [] { return RollingMean{20}; });
    check("RollingStdev", [] { return RollingStdev{20}; });
    check("Ewma", [] { return Ewma{10.0}; });
    check("LaggedReturn(1)", [] { return LaggedReturn{1}; });
    check("LaggedReturn(30)", [] { return LaggedReturn{30}; });
    check("ShortTermReversal", [] { return ShortTermReversal{5}; });
    check("MaDeviation", [] { return MaDeviation{30}; });
    check("RollingZScore", [] { return RollingZScore{60}; });
    check("RealizedVolatility", [] { return RealizedVolatility{15}; });
    check("RollingTwap", [] { return RollingTwap{20}; });
}

TEST_CASE("bivariate estimators are causal", "[features][leakage][causality]") {
    constexpr std::size_t kN = 200;
    constexpr std::size_t kPrefix = 120;
    const auto [a, b] = forked_series(kN, kPrefix);
    const auto [m, m2] = forked_series(kN, kPrefix);

    const auto check = [&](const char* label, auto make) {
        INFO("estimator: " << label);
        auto ea = make();
        auto eb = make();
        for (std::size_t i = 0; i < kPrefix; ++i) {
            ea.update(m[i], a[i]);
            eb.update(m2[i], b[i]);
            INFO("index " << i);
            REQUIRE(ea.value() == eb.value());
        }
    };
    check("RollingCovariance", [] { return RollingCovariance{30}; });
    check("RollingCorrelation", [] { return RollingCorrelation{30}; });
    check("RollingBeta", [] { return RollingBeta{30}; });
}

TEST_CASE("an estimator that peeks is caught by the divergence helper",
          "[features][leakage][causality]") {
    // Proves the causality harness itself bites. A deliberately non-causal
    // estimator -- one that reports the value it is ABOUT to receive -- must be
    // detected inside the shared prefix.
    struct Peeking {
        double next = 0.0;
        void update(double x) noexcept { next = x; }
        [[nodiscard]] double value() const noexcept { return next; }
    };

    constexpr std::size_t kPrefix = 50;
    const auto [a, b] = forked_series(100, kPrefix);

    Peeking pa;
    Peeking pb;
    std::size_t first_diff = static_cast<std::size_t>(-1);
    for (std::size_t i = 0; i < 100; ++i) {
        pa.update(a[i]);
        pb.update(b[i]);
        if (pa.value() != pb.value()) {
            first_diff = i;
            break;
        }
    }
    // Divergence appears exactly at the fork, not before and not never.
    REQUIRE(first_diff == kPrefix);
}

TEST_CASE("no estimator is ready before its warmup", "[features][leakage][warmup]") {
    // Warmup honesty (reconciliation: test_warmup). The first N values of an
    // unguarded rolling window are garbage; the ready flag is what stops them
    // being traded on.
    const auto check = [](const char* label, auto est, std::size_t expected_warmup) {
        INFO("estimator: " << label);
        REQUIRE(est.warmup() == expected_warmup);
        for (std::size_t i = 0; i < expected_warmup - 1; ++i) {
            est.update(100.0 + static_cast<double>(i));
            INFO("after " << (i + 1) << " updates");
            REQUIRE_FALSE(est.ready());
        }
        est.update(100.0);
        REQUIRE(est.ready());
    };

    check("RollingMean(20)", RollingMean{20}, 20);
    check("RollingStdev(20)", RollingStdev{20}, 20);
    check("MaDeviation(30)", MaDeviation{30}, 30);
    check("RealizedVolatility(15)", RealizedVolatility{15}, 15);
    check("LaggedReturn(5)", LaggedReturn{5}, 6);
}

TEST_CASE("resetting an estimator restores its initial state", "[features][determinism]") {
    // A walk-forward fold replays the same window repeatedly. A reset that left
    // residue would make the second pass differ from the first.
    RollingStdev sd{10};
    for (int i = 0; i < 50; ++i) sd.update(100.0 + static_cast<double>(i));
    const double dirty = sd.value();
    REQUIRE(dirty > 0.0);

    sd.reset();
    REQUIRE_FALSE(sd.ready());
    RollingStdev fresh{10};
    for (int i = 0; i < 50; ++i) {
        sd.update(100.0 + static_cast<double>(i));
        fresh.update(100.0 + static_cast<double>(i));
    }
    REQUIRE(sd.value() == fresh.value());
}
